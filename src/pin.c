// ---------------------------------------------------------------------------
//  PIN-gated seed storage. See pin.h for the model.
//
//  On-disk layout in NVS namespace "pin":
//    "salt"   blob, 16 bytes — generated once at first setup, stable thereafter
//    "sealed" blob, 12 + 16 + N bytes — IV(12) | tag(16) | ciphertext(N)
//    "tries"  u8                       — wrong-PIN counter
//
//  Crypto:
//    KDF:  PBKDF2-HMAC-SHA256, 100k iterations, 32-byte key out
//    AEAD: AES-256-GCM (mbedtls_gcm), 12-byte IV, 16-byte tag
//    IV:   freshly random per seal (we never reuse with the same key)
// ---------------------------------------------------------------------------
#include "pin.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "mbedtls/gcm.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"

static const char *TAG = "pin";

#define PIN_NVS_NAMESPACE "pin"
#define PIN_NVS_KEY_SALT   "salt"
#define PIN_NVS_KEY_SEALED "sealed"
#define PIN_NVS_KEY_TRIES  "tries"

#define PIN_SALT_LEN  16
#define PIN_KEY_LEN   32       // AES-256
#define PIN_IV_LEN    12       // GCM standard
#define PIN_TAG_LEN   16       // GCM auth tag
#define PIN_PBKDF2_ITERATIONS 100000

static bool s_inited = false;

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
static pin_status_t open_handle(nvs_open_mode_t mode, nvs_handle_t *h)
{
    esp_err_t err = nvs_open(PIN_NVS_NAMESPACE, mode, h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", PIN_NVS_NAMESPACE, esp_err_to_name(err));
        return PIN_ERR_NVS;
    }
    return PIN_OK;
}

pin_status_t pin_init(void)
{
    if (s_inited) return PIN_OK;

    // nvs_flash_init() is already called at boot in app_main; skip a
    // re-init here. We just want to confirm the namespace is openable.
    nvs_handle_t h;
    pin_status_t st = open_handle(NVS_READONLY, &h);
    if (st == PIN_OK) {
        nvs_close(h);
    } else if (st == PIN_ERR_NVS) {
        // Namespace doesn't exist yet — opening read-write creates it.
        if (open_handle(NVS_READWRITE, &h) != PIN_OK) return PIN_ERR_NVS;
        nvs_close(h);
    }
    s_inited = true;
    return PIN_OK;
}

static int read_blob(nvs_handle_t h, const char *key, void *out, size_t cap, size_t *out_len)
{
    size_t len = cap;
    esp_err_t err = nvs_get_blob(h, key, out, &len);
    if (err == ESP_OK) {
        if (out_len) *out_len = len;
        return PIN_OK;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) return PIN_ERR_NOT_SET;
    ESP_LOGW(TAG, "nvs_get_blob(%s): %s", key, esp_err_to_name(err));
    return PIN_ERR_NVS;
}

static int read_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    esp_err_t err = nvs_get_u8(h, key, out);
    if (err == ESP_OK)               return PIN_OK;
    if (err == ESP_ERR_NVS_NOT_FOUND) return PIN_ERR_NOT_SET;
    return PIN_ERR_NVS;
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------
bool pin_is_set(void)
{
    if (pin_init() != PIN_OK) return false;
    nvs_handle_t h;
    if (open_handle(NVS_READONLY, &h) != PIN_OK) return false;
    size_t blob_len = 0;
    esp_err_t err = nvs_get_blob(h, PIN_NVS_KEY_SEALED, NULL, &blob_len);
    nvs_close(h);
    return err == ESP_OK && blob_len > 0;
}

int pin_attempts_remaining(void)
{
    if (pin_init() != PIN_OK) return PIN_MAX_ATTEMPTS;
    nvs_handle_t h;
    if (open_handle(NVS_READONLY, &h) != PIN_OK) return PIN_MAX_ATTEMPTS;
    uint8_t tries = 0;
    int rc = read_u8(h, PIN_NVS_KEY_TRIES, &tries);
    nvs_close(h);
    if (rc != PIN_OK) tries = 0;
    if (tries > PIN_MAX_ATTEMPTS) tries = PIN_MAX_ATTEMPTS;
    return PIN_MAX_ATTEMPTS - tries;
}

pin_status_t pin_wipe(void)
{
    if (pin_init() != PIN_OK) return PIN_ERR_NVS;
    nvs_handle_t h;
    pin_status_t st = open_handle(NVS_READWRITE, &h);
    if (st != PIN_OK) return st;
    // erase_key returns NOT_FOUND if the key never existed — that's fine.
    nvs_erase_key(h, PIN_NVS_KEY_SALT);
    nvs_erase_key(h, PIN_NVS_KEY_SEALED);
    nvs_erase_key(h, PIN_NVS_KEY_TRIES);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit on wipe failed: %s", esp_err_to_name(err));
        return PIN_ERR_NVS;
    }
    ESP_LOGW(TAG, "PIN state wiped");
    return PIN_OK;
}

// Derive a 32-byte AES key from `pin` + `salt` via PBKDF2-HMAC-SHA256.
// Returns true on success. The 100k iterations take ~1-2 s on ESP32-S3 —
// slow enough to deter brute force, fast enough to feel responsive.
static bool kdf_pbkdf2(const char *pin, const uint8_t *salt, size_t salt_len,
                      uint8_t out_key[PIN_KEY_LEN])
{
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256,
        (const uint8_t *)pin, strlen(pin),
        salt, salt_len,
        PIN_PBKDF2_ITERATIONS,
        PIN_KEY_LEN, out_key);
    if (rc != 0) {
        ESP_LOGE(TAG, "pbkdf2 rc=-0x%04x", -rc);
        return false;
    }
    return true;
}

pin_status_t pin_setup(const char *pin, const uint8_t *seed, size_t seed_len)
{
    if (!pin || !seed) return PIN_ERR_BAD_ARG;
    size_t pin_len = strlen(pin);
    if (pin_len < PIN_MIN_LEN || pin_len > PIN_MAX_LEN) return PIN_ERR_BAD_ARG;
    if (seed_len == 0 || seed_len > PIN_MAX_SEED_LEN)   return PIN_ERR_BAD_ARG;

    pin_status_t st = pin_init();
    if (st != PIN_OK) return st;

    // Fresh salt + IV for this device, every time setup runs.
    uint8_t salt[PIN_SALT_LEN], iv[PIN_IV_LEN], key[PIN_KEY_LEN];
    esp_fill_random(salt, sizeof salt);
    esp_fill_random(iv,   sizeof iv);

    if (!kdf_pbkdf2(pin, salt, sizeof salt, key)) return PIN_ERR_INTERNAL;

    // Encrypt seed → sealed blob = IV | tag | ciphertext
    uint8_t sealed[PIN_IV_LEN + PIN_TAG_LEN + PIN_MAX_SEED_LEN];
    memcpy(sealed, iv, PIN_IV_LEN);
    uint8_t tag[PIN_TAG_LEN];
    uint8_t *ct = sealed + PIN_IV_LEN + PIN_TAG_LEN;
    size_t  sealed_len = PIN_IV_LEN + PIN_TAG_LEN + seed_len;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, PIN_KEY_LEN * 8);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
                                       seed_len,
                                       iv, PIN_IV_LEN,
                                       NULL, 0,        // no associated data
                                       seed, ct,
                                       PIN_TAG_LEN, tag);
    }
    mbedtls_gcm_free(&gcm);
    // Zero the derived key right after use.
    memset(key, 0, sizeof key);
    if (rc != 0) {
        ESP_LOGE(TAG, "gcm encrypt rc=-0x%04x", -rc);
        return PIN_ERR_INTERNAL;
    }
    memcpy(sealed + PIN_IV_LEN, tag, PIN_TAG_LEN);

    // Persist
    nvs_handle_t h;
    st = open_handle(NVS_READWRITE, &h);
    if (st != PIN_OK) return st;

    esp_err_t err = nvs_set_blob(h, PIN_NVS_KEY_SALT, salt, sizeof salt);
    if (err == ESP_OK) err = nvs_set_blob(h, PIN_NVS_KEY_SEALED, sealed, sealed_len);
    if (err == ESP_OK) err = nvs_set_u8(h, PIN_NVS_KEY_TRIES, 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    // Zero the sealed scratch (it's already in NVS, no need to retain)
    memset(sealed, 0, sizeof sealed);
    memset(tag,    0, sizeof tag);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set on setup failed: %s", esp_err_to_name(err));
        return PIN_ERR_NVS;
    }
    ESP_LOGI(TAG, "PIN set; %zu-byte seed sealed", seed_len);
    return PIN_OK;
}

pin_status_t pin_unlock(const char *pin,
                        uint8_t *out_seed, size_t out_cap, size_t *out_len)
{
    if (!pin || !out_seed || out_cap == 0) return PIN_ERR_BAD_ARG;
    size_t pin_len = strlen(pin);
    if (pin_len < PIN_MIN_LEN || pin_len > PIN_MAX_LEN) return PIN_ERR_BAD_ARG;

    pin_status_t st = pin_init();
    if (st != PIN_OK) return st;

    nvs_handle_t h;
    st = open_handle(NVS_READONLY, &h);
    if (st != PIN_OK) return st;

    uint8_t salt[PIN_SALT_LEN];
    size_t salt_len = 0;
    int rc = read_blob(h, PIN_NVS_KEY_SALT, salt, sizeof salt, &salt_len);

    uint8_t sealed[PIN_IV_LEN + PIN_TAG_LEN + PIN_MAX_SEED_LEN];
    size_t  sealed_len = 0;
    if (rc == PIN_OK) rc = read_blob(h, PIN_NVS_KEY_SEALED, sealed, sizeof sealed, &sealed_len);
    nvs_close(h);

    if (rc == PIN_ERR_NOT_SET) return PIN_ERR_NOT_SET;
    if (rc != PIN_OK)          return PIN_ERR_NVS;
    if (salt_len != PIN_SALT_LEN || sealed_len < PIN_IV_LEN + PIN_TAG_LEN + 1) {
        ESP_LOGE(TAG, "sealed blob malformed (salt=%zu sealed=%zu)", salt_len, sealed_len);
        return PIN_ERR_INTERNAL;
    }
    size_t ct_len = sealed_len - PIN_IV_LEN - PIN_TAG_LEN;
    if (ct_len > out_cap || ct_len > PIN_MAX_SEED_LEN) return PIN_ERR_BAD_ARG;

    uint8_t key[PIN_KEY_LEN];
    if (!kdf_pbkdf2(pin, salt, salt_len, key)) {
        memset(sealed, 0, sizeof sealed);
        return PIN_ERR_INTERNAL;
    }

    const uint8_t *iv  = sealed;
    const uint8_t *tag = sealed + PIN_IV_LEN;
    const uint8_t *ct  = sealed + PIN_IV_LEN + PIN_TAG_LEN;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int gcm_rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, PIN_KEY_LEN * 8);
    if (gcm_rc == 0) {
        gcm_rc = mbedtls_gcm_auth_decrypt(&gcm,
                                          ct_len,
                                          iv, PIN_IV_LEN,
                                          NULL, 0,
                                          tag, PIN_TAG_LEN,
                                          ct, out_seed);
    }
    mbedtls_gcm_free(&gcm);
    memset(key, 0, sizeof key);
    memset(sealed, 0, sizeof sealed);

    if (gcm_rc != 0) {
        // Wrong PIN → bump counter, possibly wipe.
        nvs_handle_t hw;
        if (open_handle(NVS_READWRITE, &hw) == PIN_OK) {
            uint8_t tries = 0;
            nvs_get_u8(hw, PIN_NVS_KEY_TRIES, &tries);
            tries++;
            if (tries >= PIN_MAX_ATTEMPTS) {
                nvs_close(hw);
                pin_wipe();
                ESP_LOGW(TAG, "PIN attempts exhausted; sealed seed wiped");
                return PIN_ERR_WIPED;
            }
            nvs_set_u8(hw, PIN_NVS_KEY_TRIES, tries);
            nvs_commit(hw);
            nvs_close(hw);
            ESP_LOGW(TAG, "wrong PIN; %d attempts left", PIN_MAX_ATTEMPTS - tries);
        }
        return PIN_ERR_BAD_PIN;
    }

    // Right PIN → reset counter
    nvs_handle_t hw;
    if (open_handle(NVS_READWRITE, &hw) == PIN_OK) {
        nvs_set_u8(hw, PIN_NVS_KEY_TRIES, 0);
        nvs_commit(hw);
        nvs_close(hw);
    }

    if (out_len) *out_len = ct_len;
    ESP_LOGI(TAG, "PIN unlocked; %zu bytes restored", ct_len);
    return PIN_OK;
}
