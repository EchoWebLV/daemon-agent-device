// ---------------------------------------------------------------------------
//  stt.c — see stt.h.
//
//  Wraps the captured 16 kHz/16-bit/mono PCM in a WAV header, hand-builds
//  a multipart/form-data body, and POSTs to OpenAI's /v1/audio/transcriptions
//  endpoint. The transcript comes back as a JSON object: { "text": "..." }.
//
//  We intentionally don't go through x402.c here — the existing client only
//  speaks application/json, and the audio endpoint requires multipart. The
//  Whisper transcription is paid via OPENAI_API_KEY on the user's account
//  (the daemon's "no user keys for chat" rule is specific to the chat path).
//  When/if x402 sprouts a multipart variant we'll switch over.
// ---------------------------------------------------------------------------
#include "stt.h"
#include "secrets.h"
#include "wifi_sta.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "cJSON.h"

static const char *TAG = "stt";

#define STT_SAMPLE_HZ 16000
#define STT_BOUNDARY  "----DaemonBoundary17X9k2pQ"
#define STT_URL       "https://api.openai.com/v1/audio/transcriptions"
#define STT_MODEL     "whisper-1"

static bool has_openai_key(void) {
    const char *k = OPENAI_API_KEY;
    return k && strlen(k) >= 10 && strncmp(k, "your-", 5) != 0;
}

// Append little-endian 32-bit and 16-bit values into a byte buffer.
static void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void put_u16_le(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}

// Build a 44-byte WAV (RIFF/PCM) header in `out` for `pcm_bytes` of mono
// 16-bit @ STT_SAMPLE_HZ samples following the header.
static void wav_header(uint8_t *out, uint32_t pcm_bytes) {
    memcpy(out, "RIFF", 4);
    put_u32_le(out + 4, 36 + pcm_bytes);
    memcpy(out + 8, "WAVE", 4);
    memcpy(out + 12, "fmt ", 4);
    put_u32_le(out + 16, 16);                // fmt chunk size
    put_u16_le(out + 20, 1);                 // PCM
    put_u16_le(out + 22, 1);                 // 1 channel (mono)
    put_u32_le(out + 24, STT_SAMPLE_HZ);
    put_u32_le(out + 28, STT_SAMPLE_HZ * 2); // byte rate
    put_u16_le(out + 32, 2);                 // block align
    put_u16_le(out + 34, 16);                // bits per sample
    memcpy(out + 36, "data", 4);
    put_u32_le(out + 40, pcm_bytes);
}

// State threaded through the HTTP event handler so we can capture the
// response body (small JSON, well under 1 KB).
typedef struct {
    char  *body;
    size_t body_cap;
    size_t body_len;
} stt_resp_t;

static esp_err_t stt_http_event(esp_http_client_event_t *evt) {
    stt_resp_t *r = (stt_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r && r->body) {
        size_t avail = r->body_cap - 1 - r->body_len;
        size_t take = (size_t)evt->data_len < avail ? (size_t)evt->data_len : avail;
        if (take > 0) {
            memcpy(r->body + r->body_len, evt->data, take);
            r->body_len += take;
            r->body[r->body_len] = 0;
        }
    }
    return ESP_OK;
}

bool stt_transcribe(const int16_t *pcm, size_t frames,
                    char *out_text, size_t out_cap) {
    if (out_text && out_cap) out_text[0] = 0;
    if (!pcm || frames == 0)         return false;
    if (!out_text || out_cap < 4)    return false;
    if (!wifi_sta_is_connected())    { ESP_LOGW(TAG, "no wifi");      return false; }
    if (!has_openai_key())           { ESP_LOGW(TAG, "no OpenAI key"); return false; }

    // ---- Build multipart body in PSRAM ------------------------------------
    const char *pre =
        "--" STT_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    const char *between =
        "\r\n--" STT_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        STT_MODEL "\r\n--" STT_BOUNDARY "--\r\n";

    const size_t pre_len     = strlen(pre);
    const size_t between_len = strlen(between);
    const size_t pcm_bytes   = frames * sizeof(int16_t);
    const size_t wav_len     = 44 + pcm_bytes;
    const size_t total       = pre_len + wav_len + between_len;

    uint8_t *body = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        ESP_LOGE(TAG, "PSRAM alloc for multipart body (%u bytes) failed",
                 (unsigned)total);
        return false;
    }
    memcpy(body, pre, pre_len);
    wav_header(body + pre_len, (uint32_t)pcm_bytes);
    memcpy(body + pre_len + 44, pcm, pcm_bytes);
    memcpy(body + pre_len + wav_len, between, between_len);

    // ---- HTTP POST --------------------------------------------------------
    char respbuf[768] = {0};
    stt_resp_t resp = { .body = respbuf, .body_cap = sizeof(respbuf), .body_len = 0 };

    esp_http_client_config_t cfg = {
        .url               = STT_URL,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 25000,
        .event_handler     = stt_http_event,
        .user_data         = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = 4096,
        .buffer_size_tx    = 2048,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { heap_caps_free(body); return false; }

    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", OPENAI_API_KEY);
    esp_http_client_set_header(h, "Authorization", auth);
    esp_http_client_set_header(h, "Content-Type",
                               "multipart/form-data; boundary=" STT_BOUNDARY);
    esp_http_client_set_post_field(h, (const char *)body, (int)total);

    ESP_LOGI(TAG, "POST %s (%.2f s of audio, %u-byte body)",
             STT_URL, (float)frames / (float)STT_SAMPLE_HZ, (unsigned)total);
    esp_err_t err    = esp_http_client_perform(h);
    int       status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    heap_caps_free(body);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "perform err: %s (status=%d)", esp_err_to_name(err), status);
        return false;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d body: %.*s", status,
                 (int)(resp.body_len > 200 ? 200 : resp.body_len), resp.body);
        return false;
    }

    // ---- Parse "{ "text": "..." }" ----------------------------------------
    cJSON *root = cJSON_ParseWithLength(resp.body, resp.body_len);
    if (!root) { ESP_LOGW(TAG, "json parse failed"); return false; }
    cJSON *text = cJSON_GetObjectItem(root, "text");
    bool ok = false;
    if (cJSON_IsString(text) && text->valuestring) {
        strncpy(out_text, text->valuestring, out_cap - 1);
        out_text[out_cap - 1] = 0;
        ESP_LOGI(TAG, "transcript: %s", out_text);
        ok = true;
    } else {
        ESP_LOGW(TAG, "no .text in response: %.*s",
                 (int)(resp.body_len > 200 ? 200 : resp.body_len), resp.body);
    }
    cJSON_Delete(root);
    return ok;
}
