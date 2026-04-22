// ---------------------------------------------------------------------------
//  Host-driven E2E test harness. Protocol + case list:
//      docs/superpowers/specs/2026-04-22-e2e-device-tests-design.md
//
//  Task-based design: one dedicated FreeRTOS task blocks on fgets(stdin) and
//  dispatches each line. BEGIN sets s_in_test_mode; the flag itself isn't
//  required for correctness (verbs work whenever they're called) but it
//  mirrors the spec's lifecycle and gives external modules something to
//  consult if they ever want to pause background work during a run.
//
//  Every response line starts with "TEST OK " or "TEST ERR " and ends with
//  a single \n. Any line NOT starting with "TEST " is ignored — the host
//  filters the same way. That lets regular ESP_LOGx output flow through
//  the same USB-CDC link without confusing the parser on either side.
// ---------------------------------------------------------------------------
#include "testharness.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "testharness";

// ---------- state ----------------------------------------------------------

// Single static flag flipped by BEGIN/END. Read atomically via test_harness_
// in_test_mode(); writes happen only from the harness task.
static volatile bool s_in_test_mode = false;

// ---------- small response helpers -----------------------------------------

static void resp_ok(const char *rest) {
    // Single printf to keep the full line atomic relative to any ESP_LOGx
    // output racing for the UART — stdio is line-buffered under the console
    // VFS, so one printf call maps to one write.
    printf("TEST OK %s\n", rest);
    fflush(stdout);
}

static void resp_err(const char *rest) {
    printf("TEST ERR %s\n", rest);
    fflush(stdout);
}

// ---------- line parsing ---------------------------------------------------

// Strip trailing CR / LF / spaces in place. fgets keeps the \n and the host
// sometimes ships \r\n; we don't want "BEGIN\r" to mismatch "BEGIN".
static void rtrim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

// Case-insensitive match of the first token. Returns pointer to rest-of-line
// (after the matched token and any intermediate whitespace), or NULL if no
// match. Empty `token` is a programming error and asserts via NULL return.
static const char *match_token(const char *line, const char *token) {
    if (!line || !token || !*token) return NULL;
    size_t tl = strlen(token);
    for (size_t i = 0; i < tl; i++) {
        if (toupper((unsigned char)line[i]) != toupper((unsigned char)token[i])) {
            return NULL;
        }
    }
    // Must be followed by whitespace or end-of-line so "BEGIN" doesn't match
    // a longer word like "BEGINNER".
    char after = line[tl];
    if (after != '\0' && after != ' ' && after != '\t') return NULL;
    const char *p = line + tl;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// ---------- verb implementations (MVP) -------------------------------------

static void handle_begin(void) {
    s_in_test_mode = true;
    resp_ok("begin");
}

static void handle_end(void) {
    s_in_test_mode = false;
    resp_ok("end");
}

static void handle_ping(void) {
    // Uptime in ms — esp_timer_get_time returns microseconds.
    uint64_t ms = (uint64_t)(esp_timer_get_time() / 1000);
    char buf[48];
    snprintf(buf, sizeof(buf), "ping %" PRIu64, ms);
    resp_ok(buf);
}

static void handle_heap(void) {
    size_t internal = esp_get_free_heap_size();
    size_t psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    char buf[64];
    snprintf(buf, sizeof(buf), "heap %u %u",
             (unsigned)internal, (unsigned)psram);
    resp_ok(buf);
}

static void handle_version(void) {
    // IDF version is a compile-time string macro. __DATE__ / __TIME__ come
    // from preprocessor and reflect when this file was last compiled, which
    // is what the host wants ("did we actually flash the new build?").
    char buf[96];
    snprintf(buf, sizeof(buf), "version %s %s %s",
             IDF_VER, __DATE__, __TIME__);
    resp_ok(buf);
}

// ---------- dispatcher -----------------------------------------------------

static void dispatch_line(const char *line) {
    // All of our verbs start with "TEST ". Anything else is someone else's
    // output and we silently ignore it — keeps regular logs from triggering
    // "unknown verb" noise.
    const char *after_test = match_token(line, "TEST");
    if (!after_test) return;

    const char *rest;

    if ((rest = match_token(after_test, "BEGIN")))    { handle_begin();   return; }
    if ((rest = match_token(after_test, "END")))      { handle_end();     return; }
    if ((rest = match_token(after_test, "PING")))     { handle_ping();    return; }
    if ((rest = match_token(after_test, "HEAP")))     { handle_heap();    return; }
    if ((rest = match_token(after_test, "VERSION")))  { handle_version(); return; }

    // Phase-2 verbs will slot in below. Until they exist, stub out so the
    // host sees a clear error instead of a timeout.
    if (match_token(after_test, "SCREEN")) { resp_err("not_implemented screen"); return; }
    if (match_token(after_test, "TAP"))    { resp_err("not_implemented tap");    return; }
    if (match_token(after_test, "SWIPE"))  { resp_err("not_implemented swipe");  return; }
    if (match_token(after_test, "WIFI"))   { resp_err("not_implemented wifi");   return; }
    if (match_token(after_test, "WALLET")) { resp_err("not_implemented wallet"); return; }
    if (match_token(after_test, "AI"))     { resp_err("not_implemented ai");     return; }
    if (match_token(after_test, "X402"))   { resp_err("not_implemented x402");   return; }

    // Silence `rest is unused` when none of the typed verbs matched.
    (void)rest;

    // Echo enough of the bad line to be useful but cap it so a giant paste
    // can't blow the output buffer.
    char msg[64];
    snprintf(msg, sizeof(msg), "unknown %.48s", after_test);
    resp_err(msg);
}

// ---------- harness task ---------------------------------------------------

// Reads one line at a time from stdin and dispatches it. Blocks on I/O —
// that's fine because this task is dedicated. Line buffer is sized to fit
// the longest verb we expect (a TEST X402 CALL <url> line, ~200 bytes).
static void harness_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "test harness task online (core=%d, prio=%d)",
             xPortGetCoreID(), uxTaskPriorityGet(NULL));

    // fgets + stdin works because PlatformIO's default ESP-IDF sdkconfig
    // routes stdio to the USB-CDC console. If a future build disables CDC
    // we'd need to fall back to uart_read_bytes on UART0.
    char line[256];

    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            // EOF or error — the console driver may deliver this briefly
            // during enumeration. Back off and retry rather than spin.
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        rtrim(line);
        if (line[0] == '\0') continue;
        dispatch_line(line);
    }
}

// ---------- public API -----------------------------------------------------

bool test_harness_begin(void) {
    BaseType_t ok = xTaskCreatePinnedToCore(harness_task,
                                            "testharness",
                                            6 * 1024,   // stack
                                            NULL,
                                            4,          // priority
                                            NULL,
                                            0);         // core 0
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn harness task");
        return false;
    }
    return true;
}

bool test_harness_in_test_mode(void) {
    return s_in_test_mode;
}
