// ---------------------------------------------------------------------------
//  Wallet screen implementation.
//
//  Layout (240x320 panel, half-and-half split):
//
//    +------------------------------------------+
//    | wallet                                 X |  status bar (26 px)
//    +------------------------------------------+
//    |                                          |
//    |            [ QR CODE 128 ]               |  top half — QR centered
//    |                                          |
//    |           abcd...wxyz                    |  truncated pubkey caption
//    +------------------------------------------+
//    |  SOL             12.3456   ~ $2440.58    |  bottom half — balances
//    |  USDC               12.34                |  + SPL tokens in a list
//    |  JUP               420.00                |
//    |  BONK         1,000,000                  |
//    +------------------------------------------+
// ---------------------------------------------------------------------------
#include "wallet_screen.h"
#include "screens_common.h"
#include "ui.h"
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
// price + usdc labels are retired on the side screens — the SOL/USDC ticker
// now lives only on the creature home. The set_price / set_usdc setters
// are kept as no-ops so ui.c can still broadcast without caring who
// listens.
static lv_obj_t *s_addr_label   = NULL;
static lv_obj_t *s_qrcode       = NULL;    // lv_qrcode; rendered once on first refresh
static bool      s_qr_set       = false;   // flips true after the pubkey is painted in
static lv_obj_t *s_list         = NULL;

// Format a base58 pubkey as "abcd...wxyz" (4 chars each side) using ASCII
// ellipsis — the default Montserrat build has no Unicode ellipsis glyph
// and prints a tofu box otherwise. 16 bytes covers it.
static void truncate_addr(const char *full, char *out, size_t cap) {
    if (!full || !full[0]) { snprintf(out, cap, "(no wallet)"); return; }
    size_t n = strlen(full);
    if (n <= 10 || cap < 14) { snprintf(out, cap, "%s", full); return; }
    snprintf(out, cap, "%c%c%c%c...%s",
             full[0], full[1], full[2], full[3],
             full + n - 4);
}

// "X" close button in the top-right of a status bar. Tapping returns to
// the creature home, which is the universal "back out of this panel"
// action on this device. Broken out so wallet/settings/wifi/etc. share
// the same 28-px hit target + style.
static void close_clicked(lv_event_t *e) {
    (void)e;
    ui_show_creature();
}

static void add_close_button(lv_obj_t *bar) {
    lv_obj_t *btn = lv_button_create(bar);
    lv_obj_set_size(btn, 28, STATUS_BAR_H - 4);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    // Transparent background so the X sits cleanly on the bar; a subtle
    // tap-state tint comes from LVGL's default pressed style.
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, close_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *x = lv_label_create(btn);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(x, SCR_COLOR_ACCENT_HI, LV_PART_MAIN);
    lv_obj_center(x);
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

    // Status bar: title on the left, close-X on the right. The SOL/USDC
    // ticker that used to live here was removed — the wallet already
    // shows its own balances below, and keeping the ticker on every
    // side screen cluttered a 26-px tall bar.
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

    add_close_button(bar);

    // --- Top half: QR code + truncated address caption --------------------
    // 128 px QR leaves a 2-module quiet zone around a 33-module code; scans
    // cleanly from 10 cm. Centered on the top half of the panel.
    s_qrcode = lv_qrcode_create(s_scr);
    lv_qrcode_set_size(s_qrcode, 128);
    lv_qrcode_set_dark_color(s_qrcode, lv_color_black());
    lv_qrcode_set_light_color(s_qrcode, lv_color_white());
    // Placeholder until wallet_screen_refresh() injects the real pubkey —
    // without this, lv_qrcode renders a red "error" square.
    lv_qrcode_set_data(s_qrcode, "daemon");
    lv_obj_align(s_qrcode, LV_ALIGN_TOP_MID, 0, STATUS_BAR_H + 6);

    s_addr_label = lv_label_create(s_scr);
    lv_label_set_text(s_addr_label, "");
    lv_obj_set_style_text_color(s_addr_label, SCR_COLOR_DIM, LV_PART_MAIN);
    lv_obj_align(s_addr_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_H + 6 + 128 + 4);

    // --- Bottom half: balance list (SOL, USDC, SPL tokens) -----------------
    // Starts at y=SCR_H/2 to split the screen cleanly; fills the rest minus
    // a small bottom margin.
    const int list_y = SCR_H / 2;
    s_list = lv_list_create(s_scr);
    lv_obj_set_size(s_list, SCR_W - 16, SCR_H - list_y - 6);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 8, list_y);
    lv_obj_set_style_bg_color(s_list, SCR_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_list, SCR_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_list, SCR_COLOR_TEXT, LV_PART_MAIN);

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
    // No-op: SOL price is creature-only now. Kept so ui.c can broadcast
    // without caring which screens listen.
    (void)s;
}

void wallet_screen_set_usdc(const char *s) {
    // Ditto — see note in wallet_screen_set_price().
    (void)s;
}

void wallet_screen_refresh(void) {
    if (!s_scr) return;

    const char *pub = wallet_pubkey();
    char addr_short[32];
    truncate_addr(pub, addr_short, sizeof(addr_short));

    double sol          = wallet_sol_balance();
    double usdc         = wallet_usdc_amount();
    double sol_usd_rate = price_sol_usd();

    char sol_amt[32], sol_line[96];
    format_amount(sol, sol_amt, sizeof(sol_amt));
    if (sol_usd_rate > 0.0) {
        char dollars[32];
        format_amount(sol * sol_usd_rate, dollars, sizeof(dollars));
        snprintf(sol_line, sizeof(sol_line), "%-6s  %-12s  ~ $%s",
                 "SOL", sol_amt, dollars);
    } else {
        snprintf(sol_line, sizeof(sol_line), "%-6s  %s", "SOL", sol_amt);
    }

    char usdc_amt[32], usdc_line[64];
    format_amount(usdc, usdc_amt, sizeof(usdc_amt));
    snprintf(usdc_line, sizeof(usdc_line), "%-6s  %s", "USDC", usdc_amt);

    const token_holding_t *tokens = NULL;
    size_t n = 0;
    wallet_tokens(&tokens, &n);

    if (!lvgl_port_lock(0)) return;

    lv_label_set_text(s_addr_label, addr_short);

    // Paint the full pubkey into the QR exactly once. wallet_pubkey() doesn't
    // change at runtime, and lv_qrcode_set_data re-renders the whole bitmap,
    // so repainting on every refresh would be wasted work. The guard also
    // protects the placeholder "daemon" value from overwriting a valid
    // render if an early refresh happens before wallet_begin() completes.
    if (s_qrcode && !s_qr_set && pub && pub[0]) {
        lv_qrcode_set_data(s_qrcode, pub);
        s_qr_set = true;
    }

    // Rebuild the balance list — SOL first, then USDC, then SPL tokens.
    // lv_list doesn't expose a clear so full rebuild is simplest.
    lv_obj_clean(s_list);

    lv_obj_t *sol_row = lv_list_add_text(s_list, sol_line);
    lv_obj_set_style_text_color(sol_row, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sol_row, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *usdc_row = lv_list_add_text(s_list, usdc_line);
    lv_obj_set_style_text_color(usdc_row, SCR_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(usdc_row, LV_OPA_TRANSP, LV_PART_MAIN);

    for (size_t i = 0; i < n; i++) {
        const token_holding_t *t = &tokens[i];
        // USDC already rendered above — wallet_tokens() returns it too.
        if (t->symbol[0] && strcmp(t->symbol, "USDC") == 0) continue;
        const char *sym = (t->symbol[0]) ? t->symbol : "SPL";
        char amt[32], line[64];
        format_amount(t->amount, amt, sizeof(amt));
        snprintf(line, sizeof(line), "%-6s  %s", sym, amt);
        lv_obj_t *row = lv_list_add_text(s_list, line);
        lv_obj_set_style_text_color(row, SCR_COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    }

    lvgl_port_unlock();
}
