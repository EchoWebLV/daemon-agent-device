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

#include "devcfg.h"
#include "display.h"
#include "price.h"
#include "server.h"
#include "touch.h"
#include "ui.h"
#include "wallet.h"
#include "wifi_sta.h"

static const char *TAG = "daemon";

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

    // Wallet + price. Both are safe to initialise before Wi-Fi is up: the
    // wallet just decodes the static key, and price_begin() is a no-op.
    // Refreshes only attempt network traffic when wifi_sta_is_connected().
    wallet_begin();
    price_begin();
    if (wifi_err == ESP_OK) {
        wallet_request_refresh();
        price_request_refresh();
    }

    // Cadence for periodic pulls. 30 s matches the Arduino build.
    const uint32_t WALLET_REFRESH_EVERY_MS = 60000;
    const uint32_t PRICE_REFRESH_EVERY_MS  = 30000;
    uint32_t last_wallet = 0, last_price = 0;

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

        char price_str[16];
        char usdc_str[16];
        price_display_string(price_str, sizeof(price_str));
        wallet_usdc_display_string(usdc_str, sizeof(usdc_str));

        ESP_LOGI(TAG, "tick=%" PRIu32 "  free=%u  lw=%u  ip=%s  %s  %s",
                 tick++, (unsigned)free_total, (unsigned)free_min,
                 ip[0] ? ip : "(none)", price_str, usdc_str);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
