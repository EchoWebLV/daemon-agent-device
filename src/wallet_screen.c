// ---------------------------------------------------------------------------
//  Wallet screen implementation.
//
//  Layout:
//
//    +--------------------------------------------------+
//    | status                       SOL $198.42         |
//    |                              USDC 12.34          |
//    +--------------------------------------------------+
//    | 2pRk…C9mj                                        |
//    |                                                  |
//    |        12.3456 SOL                               |
//    |        ≈ $2440.58                                |
//    |                                                  |
//    | ── HOLDINGS ─────────────────────────────────    |
//    |  USDC              12.34                         |
//    |  JUP              420.00                         |
//    |  BONK         1,000,000                          |
//    |  ...                                             |
//    |                                                  |
//    |                     swipe →                      |
//    +--------------------------------------------------+
// ---------------------------------------------------------------------------
#include "wallet_screen.h"
#include "screens_common.h"
#include "wallet.h"
#include "price.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "wallet_screen";

static lv_obj_t *s_scr          = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_price_label  = NULL;
static lv_obj_t *s_usdc_label   = NULL;
static lv_obj_t *s_addr_label   = NULL;
static lv_obj_t *s_sol_label    = NULL;
static lv_obj_t *s_usd_label    = NULL;
static lv_obj_t *s_list         = NULL;
static lv_obj_t *s_footer       = NULL;

// Format a base58 pubkey as "abcd…wxyz" (4 chars each side). Pass a 12-byte
// output buffer minimum; 16 bytes is plenty.
static void truncate_addr(const char *full, char *out, size_t cap) {
    if (!full || !full[0]) { snprintf(out, cap, "(no wallet)"); return; }
    size_t n = strlen(full);
    if (n <= 10 || cap < 12) { snprintf(out, cap, "%s", full); return; }
    // U+2026 HORIZONTAL ELLIPSIS is 3 UTF-8 bytes. Hardcoded for bravity.
    snprintf(out, cap, "%c%c%c%c\xE2\x80\xA6%s",
             full[0], full[1], full[2], full[3],
             full + n - 4);
}

// Format "1234567.89" with comma separators into `out`. Truncates to cap-1.
// Drops trailing zeros after decimal if any. Good enough for BONK-sized
// holdings; not trying to be libc-locale-correct.
static void format_amount(double v, char *out, size_t cap) {
    if (v == 0.0) { snprintf(out, cap, "0"); return; }
    char raw[48];
    // Up to 4 decimals — looks fine for both 0.0042 and 1234567.0.
    snprintf(raw, sizeof(raw), "%.4f", v);
    // Trim trailing zeros (and a dangling '.' if we removed them all).
    size_t rn = strlen(raw);
    while (rn > 0 && raw[rn - 1] == '0') raw[--rn] = 0;
    if (rn > 0 && raw[rn - 1] == '.')    raw[--rn] = 0;

    // Split int and frac at the dot (if any) and add comma groups to int.
    char *dot = strchr(raw, '.');
    size_t int_len = dot ? (size_t)(dot - raw) : rn;
    const char *frac = dot ? dot : "";

    // Build grouped int portion right-to-left.
    char grp[48];
    size_t gn = 0;
    for (size_t i = 0; i < int_len && gn + 2 < sizeof(grp); i++) {
        if (i && ((int_len - i) % 3) == 0) grp[gn++] = ',';
        grp[gn++] = raw[i];
    }
    grp[gn] = 0;
    snprintf(out, cap, "%s%s", grp, frac);
}

bool wallet_screen_init(void) {
    if (s_scr) return true;
    if (!lvgl_port_lock(0)) { ESP_LOGE(TAG, "lvgl_port_lock failed"); return false; }

    s_scr = lv_obj_create(NULL);
    scr_apply_bg(s_scr);

    // Status bar with left status + right price + right-below USDC.
    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_set_size(bar, SCR_W, STATUS_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, SCR_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 4, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_status_label = lv_label_create(bar);
    lv_label_set_text(s_status_label, "wallet");
    lv_obj_set_style_text_color(s_status_label, SCR_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 0, 0);

    s_price_label = lv_label_create(bar);
    lv_label_set_text(s_price_label, "");
    lv_obj_set_style_text_color(s_price_label, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_align(s_price_label, LV_ALIGN_TOP_RIGHT, 0, -2);

    s_usdc_label = lv_label_create(bar);
    lv_label_set_text(s_usdc_label, "");
    lv_obj_set_style_text_color(s_usdc_label, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(s_usdc_label, LV_ALIGN_BOTTOM_RIGHT, 0, 2);

    // Truncated address.
    s_addr_label = lv_label_create(s_scr);
    lv_label_set_text(s_addr_label, "—");
    lv_obj_set_style_text_color(s_addr_label, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(s_addr_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_H + 6);

    // Big SOL balance.
    s_sol_label = lv_label_create(s_scr);
    lv_label_set_text(s_sol_label, "—");
    lv_obj_set_style_text_color(s_sol_label, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(s_sol_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_H + 30);

    // USD equivalent.
    s_usd_label = lv_label_create(s_scr);
    lv_label_set_text(s_usd_label, "");
    lv_obj_set_style_text_color(s_usd_label, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(s_usd_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_H + 52);

    // HOLDINGS divider label. Kept separate from the list so the list's
    // own scrolling doesn't drag the header away.
    lv_obj_t *div = lv_label_create(s_scr);
    lv_label_set_text(div, "HOLDINGS");
    lv_obj_set_style_text_color(div, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(div, LV_ALIGN_TOP_LEFT, 12, STATUS_BAR_H + 80);

    // Scrollable list. lv_list has its own visual chrome; we bolt the
    // Daemon palette on top.
    s_list = lv_list_create(s_scr);
    lv_obj_set_size(s_list, SCR_W - 20, SCR_H - (STATUS_BAR_H + 104) - 24);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 10, STATUS_BAR_H + 100);
    lv_obj_set_style_bg_color(s_list, SCR_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_list, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_list, SCR_COLOR_TEXT, LV_PART_MAIN);

    // Footer hint.
    s_footer = lv_label_create(s_scr);
    lv_label_set_text(s_footer, "swipe for menu");
    lv_obj_set_style_text_color(s_footer, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_MID, 0, -6);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "wallet screen built");
    return true;
}

lv_obj_t *wallet_screen(void) { return s_scr; }

void wallet_screen_set_status(const char *s) {
    if (!s_status_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_status_label, s ? s : "");
    lvgl_port_unlock();
}

void wallet_screen_set_price(const char *s) {
    if (!s_price_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_price_label, s ? s : "");
    lvgl_port_unlock();
}

void wallet_screen_set_usdc(const char *s) {
    if (!s_usdc_label) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_usdc_label, s ? s : "");
    lvgl_port_unlock();
}

void wallet_screen_refresh(void) {
    if (!s_scr) return;

    const char *pub = wallet_pubkey();
    char addr_short[32];
    truncate_addr(pub, addr_short, sizeof(addr_short));

    double sol = wallet_sol_balance();
    double sol_usd_rate = price_sol_usd();

    char sol_str[32];
    char usd_str[56];
    format_amount(sol, sol_str, sizeof(sol_str));

    if (sol_usd_rate > 0.0) {
        char dollars[32];
        format_amount(sol * sol_usd_rate, dollars, sizeof(dollars));
        // Prefix is U+2248 ("≈") + " $" = 5 raw bytes, so 56 comfortably
        // covers a 31-byte `dollars` plus the prefix plus NUL. GCC's
        // format-truncation warning was for the 32-byte case.
        snprintf(usd_str, sizeof(usd_str), "\xE2\x89\x88 $%s", dollars);
    } else {
        usd_str[0] = 0;
    }

    const token_holding_t *tokens = NULL;
    size_t n = 0;
    wallet_tokens(&tokens, &n);

    if (!lvgl_port_lock(0)) return;

    lv_label_set_text(s_addr_label, addr_short);
    lv_label_set_text_fmt(s_sol_label, "%s SOL", sol_str);
    lv_label_set_text(s_usd_label, usd_str);

    // Replace the list contents. lv_list_add_text / lv_list_add_button
    // don't expose a clear; rebuilding is simplest.
    lv_obj_clean(s_list);
    if (n == 0) {
        lv_obj_t *empty = lv_list_add_text(s_list, "No SPL holdings yet");
        lv_obj_set_style_text_color(empty, SCR_COLOR_DIM, LV_PART_MAIN);
    } else {
        for (size_t i = 0; i < n; i++) {
            const token_holding_t *t = &tokens[i];
            const char *sym = (t->symbol[0]) ? t->symbol : "SPL";
            char amt[32];
            format_amount(t->amount, amt, sizeof(amt));
            char line[64];
            // Pad symbol column so amounts right-align-ish in the default
            // font. 10 columns keeps typical 4-letter tickers aligned.
            snprintf(line, sizeof(line), "%-6s  %s", sym, amt);
            lv_obj_t *row = lv_list_add_text(s_list, line);
            lv_obj_set_style_text_color(row, SCR_COLOR_TEXT, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        }
    }

    lvgl_port_unlock();
}
