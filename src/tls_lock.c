// ---------------------------------------------------------------------------
//  tls_lock — see tls_lock.h.
// ---------------------------------------------------------------------------
#include "tls_lock.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "tls_lock";

static SemaphoreHandle_t s_mutex = NULL;

void tls_lock_init(void) {
    if (s_mutex) return;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "mutex create failed — TLS calls will race");
        return;
    }
    ESP_LOGI(TAG, "ready");
}

bool tls_lock_take(uint32_t timeout_ms) {
    if (!s_mutex) return true;   // not initialised → no serialisation, but
                                 // don't deadlock the caller. app_main
                                 // calls init early enough that this fallback
                                 // only matters during very-early boot.
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY
                                                  : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_mutex, ticks) == pdTRUE;
}

void tls_lock_give(void) {
    if (!s_mutex) return;
    xSemaphoreGive(s_mutex);
}
