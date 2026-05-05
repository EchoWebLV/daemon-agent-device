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
#include "mdns.h"

#include "ai.h"
#include "buttons.h"
#include "creature_screen.h"
#include "devcfg.h"
#include "display.h"
#include "payapi.h"
#include "price.h"
#include "server.h"
#include "testharness.h"
#include "touch.h"
#include "ui.h"
#include "voice.h"
#include "mic.h"
#include "wallet.h"
#include "wifi_sta.h"

static const char *TAG = "daemon";

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

    // Bring the ILI9342C online early so dead-panel failures show up in
    // the log before we start allocating for LVGL / WiFi / audio.
    ESP_ERROR_CHECK(display_init());

    // LVGL on top. The navy clear drawn by display_init() above is immediately
    // overdrawn by LVGL's first frame, but keeping it around means we still
    // get a "display is alive" signal even if LVGL itself fails to start.
    ESP_ERROR_CHECK(ui_init());

    // GT911 touch → LVGL pointer input. Must come after ui_init() because
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

        // mDNS so Claude Code's MCP server and curl can reach us by name.
        // The hostname becomes "daemon.local" on Bonjour-aware hosts. We
        // also advertise the HTTP service so network browsers list us.
        // Failure is non-fatal — the IP-based path keeps working.
        if (mdns_init() == ESP_OK) {
            mdns_hostname_set("daemon");
            mdns_instance_name_set("Daemon");
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
            ESP_LOGI(TAG, "mDNS up — http://daemon.local/");
        } else {
            ESP_LOGW(TAG, "mdns_init failed; reach by IP only");
        }

        // Pay.sh dispatcher: fetch catalog + enabled providers' openapi specs,
        // slim into LLM tools, register with ai.c. Spawns a background task
        // that doesn't block. payapi_init is idempotent so reconnect events
        // don't re-spawn the task.
        payapi_init();
    }

    // AI client + /say handler. ai_begin() resets conversation history and
    // wires the model id. server_set_say_handler() routes POST /say from
    // the web UI through say_with_ui() at the top of this file, which calls
    // ai_handle_say() (x402 chat round-trip) then ui_deliver_reply() to
    // forward the reply to the creature subtitle + voice path.
    ai_begin();
    server_set_say_handler(say_with_ui);

    // Voice: ES8311 codec on the shared I2C bus + I2S TX. Plays the
    // boot-beep probe so a dead speaker / unseated codec is obvious in
    // the first second of boot. Failure is non-fatal; voice_speak's
    // NULL guards make replies fall through silently if codec init didn't
    // take.
    if (!voice_begin()) {
        ESP_LOGW(TAG, "voice_begin failed; continuing muted");
    }

    // Mic capture for push-to-talk. Must run AFTER voice_begin since both
    // codecs share the BSP-managed I2S port. Failure is non-fatal — the
    // PTT button just becomes a no-op when nothing can record.
    if (mic_init() != ESP_OK) {
        ESP_LOGW(TAG, "mic_init failed; push-to-talk disabled");
    }

    // Front-face hardware button → PTT. Must run AFTER touch_init so the
    // GT911 handle is live (the BSP button bit comes through the touch IC,
    // not a dedicated GPIO — see buttons.c). Non-fatal on failure.
    if (buttons_init() != ESP_OK) {
        ESP_LOGW(TAG, "buttons_init failed; hardware PTT disabled");
    }

    // Wallet + price. Both are safe to initialise before Wi-Fi is up: the
    // wallet just decodes the static key, and price_begin() is a no-op.
    // Refreshes only attempt network traffic when wifi_sta_is_connected().
    wallet_begin();
    // React when incoming SOL / USDC / SPL crosses the announcement
    // threshold. Chains into ui_deliver_reply (subtitle + TTS).
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

    // Boot greeting — exercises the full chat pipeline (subtitle + TTS) on
    // every cold boot so a regression on either side surfaces immediately.
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
