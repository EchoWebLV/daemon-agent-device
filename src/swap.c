// ---------------------------------------------------------------------------
//  swap.c — see swap.h.
//
//  Pipeline (one swap):
//    1. resolve symbols → mints + decimals
//    2. balance check against wallet snapshot
//    3. Jupiter /quote → outAmount + min_out + verbatim quote-response JSON
//    4. open approval modal, block on confirm/cancel
//    5. Jupiter /swap (sends quote-response back) → unsigned v0 tx
//    6. parse v0 wire format, locate our signature slot, sign + splice
//    7. RPC sendTransaction
//    8. RPC getSignatureStatuses, poll up to 30s
//    9. wallet_request_refresh() → creature speaks via incoming-payment cb
// ---------------------------------------------------------------------------
#include "swap.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "swap";

bool swap_request(const char *from_sym,
                  const char *to_sym,
                  double      amount_ui,
                  uint16_t    slippage_bps,
                  swap_result_t *out) {
    (void)from_sym; (void)to_sym; (void)amount_ui; (void)slippage_bps;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->status = SWAP_ERR_QUOTE;
    strlcpy(out->error_msg, "not_implemented", sizeof(out->error_msg));
    ESP_LOGW(TAG, "swap_request stub — not implemented yet");
    return false;
}
