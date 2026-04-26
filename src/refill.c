// ---------------------------------------------------------------------------
//  Vault → device USDC ATA auto-refill. See refill.h.
//
//  Tx shape:
//    [0]  ComputeBudget SetComputeUnitLimit
//    [1]  ComputeBudget SetComputeUnitPrice
//    [2]  agent_program::vault_execute  →  CPI:  SPL TransferChecked
//                       vault USDC ATA  →  device USDC ATA  (vault PDA signs)
// ---------------------------------------------------------------------------
#include "refill.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "agent_pda.h"
#include "base58.h"
#include "solana_tx.h"
#include "wallet.h"
#include "x402.h"

static const char *TAG = "refill";

// Real USDC mint, 6 decimals — same constant the device wallet refresh keys off.
static const char *USDC_MINT_B58 = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
#define USDC_DECIMALS 6

// One refill at a time. wallet_refresh is the only caller in production
// (plus the test harness verb), so a non-blocking flag is enough — we
// never get re-entered from multiple tasks.
static volatile bool s_in_flight = false;

// ---------------------------------------------------------------------------
//  Build + sign the refill tx. Same shape the test harness has been driving
//  on devnet/mainnet since Task 7 — extracted here so x402.c hot-path's
//  failures don't leak into the (separate-tx) refill path.
// ---------------------------------------------------------------------------
static int build_refill_tx(uint64_t amount_atomic, char *tx_b64, size_t cap)
{
    const uint8_t *device_pk    = wallet_pubkey_bytes();
    const uint8_t *vault_pk     = wallet_vault_pda_bytes();
    const uint8_t *vault_ata_pk = wallet_vault_usdc_ata_bytes();
    const uint8_t *device_ata_pk = wallet_device_usdc_ata_bytes();
    if (!device_pk || !vault_pk || !vault_ata_pk || !device_ata_pk) {
        ESP_LOGW(TAG, "wallet/vault paths not configured");
        return -1;
    }

    uint8_t mint_pk[32], blockhash[32];
    if (base58_decode(USDC_MINT_B58, mint_pk, 32) != 32) return -1;
    if (!fetch_recent_blockhash(blockhash))              return -1;

    // Inner: SPL TransferChecked, vault ATA → device ATA, authority = vault PDA.
    uint8_t inner_data[10];
    inner_data[0] = 0x0C;
    for (int i = 0; i < 8; i++) inner_data[1 + i] = (uint8_t)(amount_atomic >> (i * 8));
    inner_data[9] = USDC_DECIMALS;

    const agent_meta_t inner_metas[] = {
        { vault_ata_pk,  false, true  },
        { mint_pk,       false, false },
        { device_ata_pk, false, true  },
        { vault_pk,      true,  false },
    };

    uint8_t outer_ix_data[256];
    agent_meta_t outer_metas[16];
    size_t outer_meta_count = 0;
    int outer_data_len = agent_pda_build_vault_execute_ix(
        vault_pk, device_pk, SPL_TOKEN_PROGRAM_ID,
        inner_metas, sizeof inner_metas / sizeof inner_metas[0],
        inner_data, sizeof inner_data,
        outer_ix_data, sizeof outer_ix_data,
        outer_metas, &outer_meta_count, sizeof outer_metas / sizeof outer_metas[0]);
    if (outer_data_len < 0) {
        ESP_LOGW(TAG, "vault_execute encode failed");
        return -1;
    }

    solana_ix_account_t v2_accs[16];
    for (size_t i = 0; i < outer_meta_count; i++) {
        v2_accs[i].pubkey      = outer_metas[i].pubkey;
        v2_accs[i].is_signer   = outer_metas[i].is_signer;
        v2_accs[i].is_writable = outer_metas[i].is_writable;
    }
    solana_ix_v2_t ix = {
        .program_id    = AGENT_PROGRAM_ID,
        .accounts      = v2_accs,
        .account_count = outer_meta_count,
        .data          = outer_ix_data,
        .data_len      = (size_t)outer_data_len,
    };
    // Device pays its own gas — no facilitator on this path. fee_payer ==
    // signer_pubkey is what TEST VAULT TRANSFER's path proved on chain.
    solana_tx_input_v2_t txin = {
        .fee_payer      = device_pk,
        .signer_pubkey  = device_pk,
        .blockhash      = blockhash,
        .ixs            = &ix, .ix_count = 1,
        .cu_limit       = 50000,
        .cu_price_micro = 1,
    };
    return solana_build_tx_v2_base64(&txin, tx_b64, cap);
}

// ---------------------------------------------------------------------------
//  Submit a base64-encoded VersionedTransaction via the configured RPC.
//  Reuses x402.c's rpc_call helper so we only have one TLS-pinned client.
// ---------------------------------------------------------------------------
static bool submit_tx(const char *tx_b64, char *txid_out, size_t cap)
{
    size_t req_cap = strlen(tx_b64) + 256;
    char  *req     = malloc(req_cap);
    if (!req) return false;
    snprintf(req, req_cap,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"sendTransaction\","
        "\"params\":[\"%s\","
        "{\"encoding\":\"base64\",\"skipPreflight\":false,"
        "\"preflightCommitment\":\"processed\"}]}",
        tx_b64);

    char *rsp = malloc(4096);
    if (!rsp) { free(req); return false; }
    bool ok = rpc_call(req, rsp, 4096);
    free(req);
    if (!ok) { free(rsp); return false; }

    cJSON *root = cJSON_Parse(rsp);
    free(rsp);
    if (!root) return false;
    bool got = false;
    const cJSON *result = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsString(result) && result->valuestring && result->valuestring[0]) {
        strlcpy(txid_out, result->valuestring, cap);
        got = true;
    } else {
        const cJSON *err = cJSON_GetObjectItem(root, "error");
        if (cJSON_IsObject(err)) {
            const cJSON *msg = cJSON_GetObjectItem(err, "message");
            if (cJSON_IsString(msg)) ESP_LOGW(TAG, "rpc err: %s", msg->valuestring);
        }
    }
    cJSON_Delete(root);
    return got;
}

bool refill_run_amount(uint64_t amount_atomic, char *out_txid, size_t txid_cap)
{
    if (!out_txid || txid_cap == 0)  return false;
    if (amount_atomic == 0)          return false;
    if (s_in_flight) {
        ESP_LOGI(TAG, "refill already in flight — skipping");
        return false;
    }
    s_in_flight = true;
    out_txid[0] = '\0';

    char tx_b64[1024];
    int n = build_refill_tx(amount_atomic, tx_b64, sizeof tx_b64);
    if (n <= 0) { s_in_flight = false; return false; }

    bool ok = submit_tx(tx_b64, out_txid, txid_cap);
    if (ok) ESP_LOGI(TAG, "refill submitted: %llu micro-USDC, txid %s",
                     (unsigned long long)amount_atomic, out_txid);
    else    ESP_LOGW(TAG, "refill submit failed");

    s_in_flight = false;
    return ok;
}

bool refill_check_and_maybe_run(char *out_txid, size_t txid_cap)
{
    if (!out_txid || txid_cap == 0) return false;
    out_txid[0] = '\0';

    // wallet_usdc_amount returns the device's USDC ATA balance from the most
    // recent wallet_refresh — exactly the cache we want to gate on.
    double usdc = wallet_usdc_amount();
    uint64_t micro = (uint64_t)(usdc * 1e6 + 0.5);
    if (micro >= REFILL_THRESHOLD_USDC_ATOMIC) return false;

    uint64_t need = REFILL_TARGET_USDC_ATOMIC - micro;
    ESP_LOGI(TAG, "device USDC %.6f below threshold; refilling %llu micro-USDC",
             usdc, (unsigned long long)need);
    return refill_run_amount(need, out_txid, txid_cap);
}

// ---------------------------------------------------------------------------
//  Synchronous wait — getSignatureStatuses poll, ~30 s ceiling.
//  Mirrors swap.c::rpc_wait_for_confirm but uses the public rpc_call helper.
// ---------------------------------------------------------------------------
static bool wait_for_confirm(const char *txid) {
    if (!txid || !txid[0]) return false;
    for (int i = 0; i < 38; i++) {
        char req[256];
        snprintf(req, sizeof req,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getSignatureStatuses\","
            "\"params\":[[\"%s\"],{\"searchTransactionHistory\":false}]}",
            txid);
        char rsp[1024];
        if (rpc_call(req, rsp, sizeof rsp)) {
            cJSON *root = cJSON_Parse(rsp);
            if (root) {
                const cJSON *result = cJSON_GetObjectItem(root, "result");
                const cJSON *value  = result ? cJSON_GetObjectItem(result, "value") : NULL;
                bool landed = false;
                if (cJSON_IsArray(value) && cJSON_GetArraySize(value) > 0) {
                    const cJSON *st = cJSON_GetArrayItem(value, 0);
                    if (cJSON_IsObject(st)) landed = true;
                }
                cJSON_Delete(root);
                if (landed) return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    ESP_LOGW(TAG, "wait confirm timeout %.16s...", txid);
    return false;
}

bool refill_run_and_wait(uint64_t amount_atomic, char *out_txid, size_t txid_cap)
{
    if (!refill_run_amount(amount_atomic, out_txid, txid_cap)) return false;
    if (!wait_for_confirm(out_txid)) return false;
    // Refresh the cached balances so callers can immediately re-check.
    wallet_refresh();
    return true;
}
