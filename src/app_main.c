// ---------------------------------------------------------------------------
//  Daemon — ESP-IDF skeleton entry point.
//
//  First milestone: prove the build system + USB-CDC console + PSRAM are
//  working. Logs one heartbeat per second with heap stats so we can see on
//  the serial monitor that we're alive and how much memory we have to work
//  with before we start piling on LVGL / HTTPS / audio.
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "ai.h"
#include "creature_screen.h"
#include "devcfg.h"
#include "display.h"
#include "imu.h"
#include "pin.h"
#include "pin_screen.h"
#include "price.h"
#include "server.h"
#include "testharness.h"
#include "touch.h"
#include "ui.h"
#include "voice.h"
#include "wallet.h"
#include "wifi_sta.h"
#include "wizard.h"
#include "wizard_screen.h"

static const char *TAG = "daemon";

// ---------------------------------------------------------------------------
//  Boot-time PIN unlock. Runs before wallet_begin so the PIN-derived seed
//  populates wallet state instead of secrets.h's. Blocks the boot task on
//  a binary semaphore until the user enters the right PIN (or the device
//  wipes itself after PIN_MAX_ATTEMPTS wrong tries).
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_boot_pin_sem = NULL;
static bool              s_boot_unlocked = false;

static void boot_pin_cb(pin_screen_result_t r, const char *pin, void *user) {
    (void)user;
    if (r == PIN_SCR_CANCEL) {
        // No cancel path at boot — the user has to either enter the right
        // PIN or burn through 5 wrong attempts to wipe.
        pin_screen_set_status("PIN required");
        return;
    }

    uint8_t seed[PIN_MAX_SEED_LEN]; size_t seed_len = 0;
    pin_status_t st = pin_unlock(pin, seed, sizeof seed, &seed_len);
    if (st == PIN_OK) {
        bool ok = wallet_load_seed(seed, seed_len);
        memset(seed, 0, sizeof seed);
        if (ok) {
            s_boot_unlocked = true;
            pin_screen_close();
            xSemaphoreGive(s_boot_pin_sem);
        } else {
            pin_screen_set_status("wallet load failed");
        }
        return;
    }
    memset(seed, 0, sizeof seed);
    if (st == PIN_ERR_BAD_PIN) {
        char buf[32];
        snprintf(buf, sizeof buf, "wrong (%d left)", pin_attempts_remaining());
        pin_screen_set_status(buf);
        pin_screen_clear_input();
        return;
    }
    if (st == PIN_ERR_WIPED) {
        pin_screen_set_status("WIPED — reflash to recover");
        // Release the semaphore so boot continues; wallet will fall back
        // to secrets.h (or stay disabled if it's empty).
        xSemaphoreGive(s_boot_pin_sem);
        return;
    }
    pin_screen_set_status("err — try again");
    pin_screen_clear_input();
}

// Returns true if the wallet was successfully loaded via PIN; false if no
// PIN is configured (caller should fall through to wallet_begin's
// secrets.h-driven path) or the PIN flow was wiped.
static bool boot_unlock_wallet(void) {
    if (!pin_is_set()) return false;
    s_boot_pin_sem = xSemaphoreCreateBinary();
    if (!s_boot_pin_sem) return false;
    s_boot_unlocked = false;
    pin_screen_open(PIN_MIN_LEN, 6, boot_pin_cb, NULL);
    // Block here until the callback gives the semaphore (PIN ok or wipe).
    xSemaphoreTake(s_boot_pin_sem, portMAX_DELAY);
    vSemaphoreDelete(s_boot_pin_sem);
    s_boot_pin_sem = NULL;
    return s_boot_unlocked;
}

// Wraps ai_handle_say() so the creature face + TTS kick in as a side effect
// of answering /say. Keeping the forwarder here (rather than in ai.c or ui.c)
// lets each module stay unaware of the others — ai.c writes a reply, ui.c
// displays+speaks it, and this one-liner is the seam where the two meet.
static void say_with_ui(const char *user, char *reply_out, size_t reply_cap) {
    ai_handle_say(user, reply_out, reply_cap);
    // Only drive the UI when we got a non-empty answer. On error ai.c writes
    // a short human-readable blurb into reply_out which is still worth showing.
    if (reply_out && reply_out[0]) {
        ui_deliver_reply(reply_out);
    }
}

// Fired by the wallet task whenever a positive balance delta crosses the
// threshold (> 0.01 SOL, > 0.1 USDC, > 0.1 for other SPL tokens). Runs a
// one-shot AI prompt so Daemon reacts in character, then delivers the line
// through the usual ui_deliver_reply path (subtitle + TALK mouth + TTS).
// Skipped when offline — ai_ask_one_shot and voice_speak both need Wi-Fi.
// Uses a canned fallback line if the AI call fails so the user still gets
// audible confirmation that a transfer landed.
static void on_wallet_incoming(const char *symbol, double amount) {
    if (!wifi_sta_is_connected()) return;

    char prompt[256];
    snprintf(prompt, sizeof(prompt),
             "Your Solana wallet just received %.4f %s. "
             "React in one short sentence, in character. "
             "No emoji. No markdown.",
             amount, symbol ? symbol : "tokens");

    char reply[256] = {0};
    if (ai_ask_one_shot(prompt, reply, sizeof(reply)) && reply[0]) {
        ui_deliver_reply(reply);
    } else {
        char line[96];
        snprintf(line, sizeof(line),
                 "Incoming — %.4f %s just landed.", amount,
                 symbol ? symbol : "tokens");
        ui_deliver_reply(line);
    }
}

// Fired by the IMU task when it detects a shake. Plays one of three
// complaint lines through the normal ui_deliver_reply path (subtitle +
// TALK mouth + voice_speak) and jitters the creature screen for feedback.
static void on_shake(void) {
    static const char *LINES[] = {
        "Please stop shaking me!",
        "Please stop this!",
        "Come on, stop it!",
    };
    // Round-robin so two shakes in a row never pick the same line.
    static uint8_t idx = 0;
    const char *line = LINES[idx];
    idx = (idx + 1) % (sizeof(LINES) / sizeof(LINES[0]));
    creature_screen_shake();
    ui_deliver_reply(line);
}

static void log_boot_banner(void) {
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "== Daemon booting (ESP-IDF skeleton) ==");
    ESP_LOGI(TAG, "chip: %s rev v%d.%d  cores=%d  flash=%" PRIu32 " MB",
             CONFIG_IDF_TARGET,
             chip.revision / 100, chip.revision % 100,
             chip.cores,
             flash_size / (1024 * 1024));

    size_t heap_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "heap: internal=%u B  psram=%u B",
             (unsigned)heap_internal, (unsigned)heap_psram);
}

void app_main(void) {
    log_boot_banner();

    // ---- NVS ---------------------------------------------------------------
    // Everything that persists (settings, wifi creds, wallet keypair) lives
    // in the default NVS partition. Initialise it before anyone tries to
    // read from it. If the partition is corrupted or a new blob layout ships,
    // erase and retry once so a bad NVS doesn't brick the boot.
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s); wiping and retrying",
                 esp_err_to_name(nvs));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    // Device settings (backlight PWM comes online here at the stored duty).
    ESP_ERROR_CHECK(devcfg_init());

    // TEMP: wipe any stored custom personality so the built-in PERSONA wins.
    // A stale prompt (possibly from a previous build) was overriding ai.c's
    // PERSONA and the model kept slipping into Claude's default refusal. Once
    // confirmed, delete this line — it clears any future persona on boot.
    devcfg_set_personality("");

    // Bring the ST7789 online early so dead-panel failures show up in the
    // log before we start allocating for LVGL / WiFi / audio.
    ESP_ERROR_CHECK(display_init());

    // LVGL on top. The navy clear drawn by display_init() above is immediately
    // overdrawn by LVGL's first frame, but keeping it around means we still
    // get a "display is alive" signal even if LVGL itself fails to start.
    ESP_ERROR_CHECK(ui_init());

    // CST328 touch → LVGL pointer input. Must come after ui_init() because
    // it binds to lv_display_get_default().
    ESP_ERROR_CHECK(touch_init());

    // Dial the swipe threshold down now that the pointer indev exists. Kept
    // out of ui_init() because the indev is created by touch_init() above;
    // we'd otherwise find nothing to tune.
    ui_tune_gestures();

    // Wi-Fi: try stored creds first, then the compile-time fallback. 25 s
    // timeout matches the Arduino build. Not fatal on failure — the creature
    // should still be usable offline; log and continue.
    esp_err_t wifi_err = wifi_sta_begin(25000);
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "wifi unavailable (%s); continuing offline",
                 esp_err_to_name(wifi_err));
    } else {
        // HTTP phone UI comes up once the radio is on. Safe to skip on
        // wifi failure — no listener without a network to listen on.
        ESP_ERROR_CHECK(server_start());
        server_set_status("idle");
    }

    // Wizard auto-trigger. Fresh devices with no Wi-Fi creds (and no
    // PIN-sealed seed signaling prior provisioning) drop into the
    // captive portal: AP up, splash on the LCD, daemon halts here. The
    // wizard's POST handler reboots the device after persisting form
    // values, so this branch never returns.
    if (wifi_err != ESP_OK && wizard_is_first_run() && !pin_is_set()) {
        ESP_LOGI(TAG, "fresh device — running wizard");
        ESP_ERROR_CHECK(server_start());     // wizard borrows this httpd
        char ssid[33] = {0};
        wizard_compute_ap_ssid(ssid, sizeof ssid);
        wizard_screen_show(ssid);
        ESP_ERROR_CHECK(wizard_start());
        for (;;) vTaskDelay(pdMS_TO_TICKS(60000));   // wait for /wizard reboot
    }

    // AI client: clears conversation history and registers the /say handler.
    // Wire this even when Wi-Fi didn't come up so the callback is ready for
    // a later reconnect; ai_ask() itself rejects when offline.
    //
    // say_with_ui() is the forwarder that plumbs replies into the creature
    // subtitle + voice task — see the wrapper at the top of this file.
    ai_begin();
    server_set_say_handler(say_with_ui);

    // Voice: installs I2S, plays the boot-beep probe so a dead codec is
    // obvious, and spawns the audio task. Failure here is not fatal — the
    // rest of the app is useful without audio.
    if (!voice_begin()) {
        ESP_LOGW(TAG, "voice_begin failed; continuing muted");
    }

    // QMI8658C on the same I2C bus as the touch panel. Only the accel is
    // enabled; the polling task calls on_shake() whenever the user gives the
    // board a good rattle. Not fatal — a board revision without the IMU
    // simply leaves the face un-shakable.
    if (imu_begin()) {
        imu_set_shake_cb(on_shake);
    } else {
        ESP_LOGW(TAG, "imu_begin failed; shake-to-talk disabled");
    }

    // Wallet + price. Both are safe to initialise before Wi-Fi is up: the
    // wallet just decodes the static key, and price_begin() is a no-op.
    // Refreshes only attempt network traffic when wifi_sta_is_connected().
    //
    // PIN gate: if a sealed seed exists (set up via wizard or test verb),
    // pop the keypad now and block boot until the user unlocks. wallet_begin
    // detects the already-loaded seed and skips its secrets.h fallback. If
    // no PIN is configured, this is a no-op and the legacy secrets.h path
    // handles seed loading.
    if (boot_unlock_wallet()) {
        ESP_LOGI(TAG, "wallet unlocked via PIN");
    }
    wallet_begin();
    // React when incoming SOL / USDC / SPL crosses the announcement threshold.
    // Must be installed before the first refresh so the baseline snapshot
    // happens with the callback already set (the cb itself is skipped on the
    // first refresh; installing late would still work but this keeps ordering
    // obvious).
    wallet_set_incoming_cb(on_wallet_incoming);
    price_begin();
    if (wifi_err == ESP_OK) {
        wallet_request_refresh();
        price_request_refresh();
    }

    // Cadence for periodic pulls. 30 s matches the Arduino build.
    const uint32_t WALLET_REFRESH_EVERY_MS = 60000;
    const uint32_t PRICE_REFRESH_EVERY_MS  = 30000;
    const uint32_t UI_REFRESH_EVERY_MS     = 5000;   // redraw wallet/settings
    uint32_t last_wallet = 0, last_price = 0, last_ui_refresh = 0;

    // Seed the UI with the IP so the creature status shows "idle: 192.168…"
    // even before the first tick.
    {
        char ip[16] = {0};
        wifi_sta_ip_str(ip, sizeof(ip));
        if (ip[0]) {
            char s[32];
            snprintf(s, sizeof(s), "%s", ip);
            ui_set_status(s);
        } else {
            ui_set_status(wifi_sta_is_connected() ? "online" : "offline");
        }
    }

    // Boot greeting. Only when Wi-Fi is up — ElevenLabs needs the network, and
    // silence is better than a failed TTS call on an offline boot. Routed
    // through ui_deliver_reply() so the creature face goes to TALK + subtitle
    // mirrors what the speaker says, which also exercises the whole
    // ai→ui→voice pipeline on every cold boot (handy canary).
    if (wifi_err == ESP_OK) {
        ui_deliver_reply("Hello, I am Daemon.");
    }

    // Host-driven smoke-test harness. Spawns a task that blocks on stdin and
    // dispatches "TEST ..." lines. Always on — the task idles on fgets() when
    // no host is connected, so there's no runtime cost outside of an active
    // test run. See src/testharness.c and tests/run.py.
    if (!test_harness_begin()) {
        ESP_LOGW(TAG, "test harness failed to start; tests will be unreachable");
    }

    uint32_t tick = 0;
    while (true) {
        size_t free_total = esp_get_free_heap_size();
        size_t free_min   = esp_get_minimum_free_heap_size();
        char ip[16] = {0};
        wifi_sta_ip_str(ip, sizeof(ip));

        uint32_t now_ms = tick * 1000;   // heartbeat ticks once a second
        if (wifi_sta_is_connected()) {
            if (now_ms - last_wallet >= WALLET_REFRESH_EVERY_MS) {
                wallet_request_refresh();
                last_wallet = now_ms;
            }
            if (now_ms - last_price >= PRICE_REFRESH_EVERY_MS) {
                price_request_refresh();
                last_price = now_ms;
            }
        }

        char price_str[24];
        char usdc_str[24];
        price_display_string(price_str, sizeof(price_str));
        wallet_usdc_display_string(usdc_str, sizeof(usdc_str));

        // Push status-bar values to every screen so the ticker stays
        // current regardless of which screen the user is looking at.
        ui_set_price(price_str);
        ui_set_usdc(usdc_str);
        if (ip[0]) ui_set_status(ip);

        if (now_ms - last_ui_refresh >= UI_REFRESH_EVERY_MS) {
            ui_refresh_wallet();
            ui_refresh_settings();
            last_ui_refresh = now_ms;
        }

        // Polls voice_is_speaking() so the creature's mouth stops animating
        // when the audio task drains. Internal mouth frames are driven by
        // an LVGL timer — 1 Hz is fine for the on/off transition.
        ui_tick();

        ESP_LOGI(TAG, "tick=%" PRIu32 "  free=%u  lw=%u  ip=%s  %s  %s",
                 tick++, (unsigned)free_total, (unsigned)free_min,
                 ip[0] ? ip : "(none)", price_str, usdc_str);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
