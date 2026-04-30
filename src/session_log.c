// ---------------------------------------------------------------------------
//  session_log — see session_log.h.
// ---------------------------------------------------------------------------
#include "session_log.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static char s_buf[SESSION_LOG_CAP][SESSION_LOG_MAX_LEN];
static int  s_count = 0;
static SemaphoreHandle_t s_lock = NULL;

void session_log_init(void) {
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
}

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

void session_log_append(const char *text) {
    if (!text || !text[0]) return;
    lock();

    // If full, shift everything down by one and drop the oldest.
    if (s_count == SESSION_LOG_CAP) {
        for (int i = 1; i < SESSION_LOG_CAP; ++i) {
            memcpy(s_buf[i - 1], s_buf[i], SESSION_LOG_MAX_LEN);
        }
        s_count = SESSION_LOG_CAP - 1;
    }

    // Copy with truncation; strlcpy semantics via snprintf so the buffer
    // ends with NUL even if `text` was longer than MAX_LEN-1.
    snprintf(s_buf[s_count], SESSION_LOG_MAX_LEN, "%s", text);
    s_count++;

    unlock();
}

size_t session_log_format(char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';

    lock();
    size_t used = 0;
    for (int i = 0; i < s_count; ++i) {
        // "- " + entry + "\n"
        size_t need = 2 + strlen(s_buf[i]) + 1;
        if (used + need + 1 >= cap) break;       // leave room for NUL
        out[used++] = '-';
        out[used++] = ' ';
        size_t n = strlen(s_buf[i]);
        memcpy(out + used, s_buf[i], n);
        used += n;
        out[used++] = '\n';
    }
    out[used] = '\0';
    unlock();

    return used;
}

void session_log_clear(void) {
    lock();
    s_count = 0;
    unlock();
}
