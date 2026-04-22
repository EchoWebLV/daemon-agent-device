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
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

#include "display.h"
#include "ui.h"

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

    // Bring the ST7789 online early so dead-panel failures show up in the
    // log before we start allocating for LVGL / WiFi / audio.
    ESP_ERROR_CHECK(display_init());

    // LVGL on top. The navy clear drawn by display_init() above is immediately
    // overdrawn by LVGL's first frame, but keeping it around means we still
    // get a "display is alive" signal even if LVGL itself fails to start.
    ESP_ERROR_CHECK(ui_init());

    uint32_t tick = 0;
    while (true) {
        size_t free_total = esp_get_free_heap_size();
        size_t free_min   = esp_get_minimum_free_heap_size();
        ESP_LOGI(TAG, "tick=%" PRIu32 "  free=%u B  low-water=%u B",
                 tick++, (unsigned)free_total, (unsigned)free_min);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
