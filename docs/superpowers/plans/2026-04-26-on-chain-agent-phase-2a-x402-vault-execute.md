# On-chain Agent — Phase 2a-x402: Route x402 USDC payments through `vault_execute` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the firmware from paying x402 facilitators directly out of the device's USDC ATA. Instead route every x402 payment through the deployed `agent_program`'s `vault_execute` so funds live in a program-owned vault PDA. The device signs the *outer* `vault_execute` with its existing keypair; the program re-signs the inner `TransferChecked` as the vault PDA via CPI seeds. From the gateway's point of view nothing changes — it still sees a valid USDC arrival.

**Architecture:** New `src/agent_pda.{h,c}` module owns the on-chain identity bits — program ID, PDA derivation (`agent_root`, `vault`), Anchor instruction discriminators, the `InnerIx` Borsh-ish encoder, and an `agent_vault_execute_ix(...)` builder that wraps an arbitrary inner instruction. `src/solana_tx.c` is refactored to compose multi-instruction v0 messages from raw `solana_ix_t` blobs (today it's hard-coded to one TransferChecked); `src/x402.c` builds the inner `TransferChecked` exactly as today, then wraps it via the new helper before signing. `src/wallet.c` exposes the vault PDA and tracks its USDC ATA balance instead of (or alongside) the device key's. Vault initialization happens off-device via a small TS script driven by the user's CLI wallet — Phase 2c (wizard) will move it on-device.

**Tech Stack:** ESP-IDF, FreeRTOS, mbedtls (sha256 + base64), orlp/ed25519, esp_http_client, Anchor 0.32.1 program at `9DJqU25ShsEXNisbzSNUPzaN6qiSbU9XiNL7eerqYPFf` on devnet, Solana JSON-RPC.

**Spec:** [`docs/superpowers/specs/2026-04-26-on-chain-agent-design.md`](../specs/2026-04-26-on-chain-agent-design.md) (sections "On-device changes → Modified" and "On-device changes → New").

---

## Scope notes

- **In:** x402 payment path. Owner = device key (single-key vault for testing). Vault initialization off-device via TS.
- **Out:** swap path (Phase 2a-swap), PIN/sealed NVS/secure boot (Phase 2b), captive-portal wizard (Phase 2c).
- **Owner-key story for v1 testing:** the user's CLI wallet (which paid for the program deploy) acts as recovery_authority. The device's `secrets.h` SOLANA_KEY acts as current_signer. They're different keypairs, but for Phase 2a the only thing that matters is that the device can sign `vault_execute` and the gateway sees the resulting USDC arrival. Phase 2c handles the proper Phantom-driven owner pairing.
- **Mainnet vs devnet:** all of Phase 2a runs on devnet against the deployed program. Mainnet flip happens after Phase 5 + audit.

---

## File structure

| File                              | Status   | Responsibility                                                            |
|-----------------------------------|----------|---------------------------------------------------------------------------|
| `src/agent_pda.h`                 | Create   | Public surface: program ID, PDA derivation, ix builders                   |
| `src/agent_pda.c`                 | Create   | Discriminators, InnerIx encoding, instruction builders                    |
| `src/solana_tx.h`                 | Modify   | Add `solana_ix_t`, `solana_tx_input_v2_t`, `solana_build_tx_v2_base64`    |
| `src/solana_tx.c`                 | Modify   | Generalize message composition to a list of `solana_ix_t`                 |
| `src/x402.c`                      | Modify   | Wrap inner `TransferChecked` via `agent_vault_execute_ix`                 |
| `src/wallet.c`                    | Modify   | Expose `wallet_vault_pda()` + `wallet_vault_usdc_ata()`                   |
| `src/wallet.h`                    | Modify   | Public declarations for the new accessors                                 |
| `src/secrets.h.example`           | Modify   | Add `OWNER_PUBKEY` (base58)                                               |
| `src/CMakeLists.txt`              | Modify   | Add `agent_pda.c` to `SRCS`                                               |
| `src/testharness.c`               | Modify   | Add `vault_status` and `vault_x402_test` verbs                            |
| `scripts/setup-test-vault.ts`     | Create   | TS off-device script: init vault + fund vault PDA + create USDC ATA       |

`src/agent_pda.c` is split internally into clearly-bounded sections (program ID + PDAs, discriminators, encoders, instruction builders). Header surface is intentionally narrow — three derivation helpers and two ix builders — so callers see the program client as opaque.

`src/solana_tx.c` keeps its existing `solana_tx_input_t` + `solana_build_signed_tx_base64` for backwards compat during the refactor; the new generic path is `solana_tx_input_v2_t` + `solana_build_tx_v2_base64`. Once `x402.c` is migrated, swap.c will follow in Phase 2a-swap and the v1 entry point can be dropped.

---

## Build / test commands (used throughout)

```bash
# build only
pio run -e waveshare_esp32s3_28

# build + flash + open monitor
pio run -e waveshare_esp32s3_28 -t upload && pio device monitor -e waveshare_esp32s3_28

# host-side TS off-device
yarn ts-node scripts/setup-test-vault.ts <device-pubkey>

# host-side test harness (after device is flashed)
python tests/run.py "vault_status"
python tests/run.py "vault_x402_test"
```

The device-side tasks all end with **build** as the verification gate (firmware compiles cleanly; new symbols link). End-to-end smoke (Task 8) is a real on-device flash + harness verb run that exercises an actual devnet `vault_execute`.

---

## Pre-flight: vault setup (run once before device tasks)

This is one-time, off-device, before any of the firmware tasks below.

- [ ] **Pre-1: Get the device pubkey**

The device's keypair lives in `src/secrets.h` as `SOLANA_KEY` (64-byte hex). Derive its base58 pubkey:

```bash
node -e '
const k = require("./src/secrets.h").match(/SOLANA_KEY\s+"([0-9a-fA-F]+)"/)[1];
const b = Buffer.from(k.slice(64), "hex");  // last 32 bytes are the pubkey
const bs58 = require("bs58").default || require("bs58");
console.log(bs58.encode(b));
'
```

Or read it from the running device (the firmware logs it on boot; or use the existing `wallet_pubkey` test-harness verb).

- [ ] **Pre-2: Run setup-test-vault.ts**

```bash
yarn ts-node scripts/setup-test-vault.ts <DEVICE_PUBKEY>
```

This script (Task 1 below) initializes the vault on devnet with `recovery_authority = your CLI wallet` and `current_signer = <DEVICE_PUBKEY>`, creates the vault's USDC ATA (devnet USDC mint), funds the vault with 0.05 SOL + 1 USDC for tests.

After this, `agent_root` and `vault` PDAs are derived and persisted to `target/test-vault.json` for the firmware to reference.

---

## Task 1: TS setup script (off-device, one-time per device)

**Files:**
- Create: `scripts/setup-test-vault.ts`

- [ ] **Step 1.1: Write `scripts/setup-test-vault.ts`**

```ts
/**
 * One-time test-vault setup, driven by the user's CLI wallet (recovery_authority).
 * Argv: <device-pubkey-base58>
 *
 *   yarn ts-node scripts/setup-test-vault.ts FNVcw2k...
 *
 * Idempotent: skips initialize_agent if the vault already exists, just tops up
 * lamports + USDC and re-creates ATAs as needed.
 */
import * as anchor from "@coral-xyz/anchor";
import { Program } from "@coral-xyz/anchor";
import {
  Connection,
  Keypair,
  PublicKey,
  SystemProgram,
  LAMPORTS_PER_SOL,
  clusterApiUrl,
  Transaction,
} from "@solana/web3.js";
import {
  createMint,
  getOrCreateAssociatedTokenAccount,
  mintTo,
  TOKEN_PROGRAM_ID,
} from "@solana/spl-token";
import { AgentProgram } from "../target/types/agent_program";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";

// Devnet USDC for testing — actually re-using a fresh mint each setup since
// real devnet USDC requires Circle's faucet. We mint our own and pretend it's
// USDC for the wire shape's sake.
async function main() {
  const argv = process.argv.slice(2);
  if (argv.length !== 1) {
    console.error("usage: ts-node scripts/setup-test-vault.ts <DEVICE_PUBKEY>");
    process.exit(1);
  }
  const devicePk = new PublicKey(argv[0]);

  const conn = new Connection(clusterApiUrl("devnet"), "confirmed");
  const owner = Keypair.fromSecretKey(
    Uint8Array.from(
      JSON.parse(fs.readFileSync(`${os.homedir()}/.config/solana/id.json`, "utf-8"))
    )
  );
  const provider = new anchor.AnchorProvider(
    conn,
    new anchor.Wallet(owner),
    { commitment: "confirmed" }
  );
  anchor.setProvider(provider);
  const program = anchor.workspace.AgentProgram as Program<AgentProgram>;

  const [agentRoot] = PublicKey.findProgramAddressSync(
    [Buffer.from("agent"), owner.publicKey.toBuffer()],
    program.programId
  );
  const [vault] = PublicKey.findProgramAddressSync(
    [Buffer.from("vault"), agentRoot.toBuffer()],
    program.programId
  );

  console.log("owner       ", owner.publicKey.toBase58());
  console.log("device      ", devicePk.toBase58());
  console.log("agent_root  ", agentRoot.toBase58());
  console.log("vault       ", vault.toBase58());

  // 1. initialize_agent (idempotent)
  const existing = await conn.getAccountInfo(vault);
  if (existing) {
    console.log("[1] vault already exists — skipping initialize_agent");
  } else {
    console.log("[1] initialize_agent");
    const sig = await program.methods
      .initializeAgent(devicePk)
      .accountsPartial({
        owner: owner.publicKey,
        agentRoot,
        vault,
        systemProgram: SystemProgram.programId,
      })
      .rpc();
    console.log("    sig:", sig);
  }

  // 2. fund vault with SOL + a tiny float for the device key (fees)
  console.log("[2] funding vault + device");
  const fundIxs: anchor.web3.TransactionInstruction[] = [];
  const vaultBal = await conn.getBalance(vault);
  if (vaultBal < 0.05 * LAMPORTS_PER_SOL) {
    fundIxs.push(
      SystemProgram.transfer({
        fromPubkey: owner.publicKey,
        toPubkey: vault,
        lamports: 0.05 * LAMPORTS_PER_SOL - vaultBal,
      })
    );
  }
  const deviceBal = await conn.getBalance(devicePk);
  if (deviceBal < 0.02 * LAMPORTS_PER_SOL) {
    fundIxs.push(
      SystemProgram.transfer({
        fromPubkey: owner.publicKey,
        toPubkey: devicePk,
        lamports: 0.02 * LAMPORTS_PER_SOL - deviceBal,
      })
    );
  }
  if (fundIxs.length) {
    await provider.sendAndConfirm(new Transaction().add(...fundIxs));
  }

  // 3. mint a fake-USDC SPL with 6 decimals, give vault an ATA, mint 1 token
  console.log("[3] minting test USDC + vault ATA");
  const mint = await createMint(conn, owner, owner.publicKey, null, 6);
  const vaultAta = await getOrCreateAssociatedTokenAccount(
    conn,
    owner,
    mint,
    vault,
    true /* allowOwnerOffCurve */
  );
  await mintTo(conn, owner, mint, vaultAta.address, owner, 1_000_000 /* 1 token */);
  console.log("    mint:", mint.toBase58());
  console.log("    vault_ata:", vaultAta.address.toBase58());

  // 4. persist for the firmware
  const out = path.resolve("target/test-vault.json");
  fs.mkdirSync(path.dirname(out), { recursive: true });
  fs.writeFileSync(
    out,
    JSON.stringify(
      {
        program_id: program.programId.toBase58(),
        owner: owner.publicKey.toBase58(),
        device: devicePk.toBase58(),
        agent_root: agentRoot.toBase58(),
        vault: vault.toBase58(),
        usdc_mint: mint.toBase58(),
        vault_usdc_ata: vaultAta.address.toBase58(),
      },
      null,
      2
    )
  );
  console.log("\nwrote", out);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
```

- [ ] **Step 1.2: Run it (verifies the script works end-to-end)**

```bash
# Use a placeholder device pubkey for now; later you'll pass the real one
yarn ts-node scripts/setup-test-vault.ts $(solana address)
```

Expected: prints all PDAs, ends with "wrote target/test-vault.json". The vault is initialized on devnet with the user's CLI pubkey as both owner AND device — this gets re-run with the real device pubkey later, which initialize_agent would reject (already exists), at which point we'll need a separate rotate flow. For Phase 2a, the simpler flow: skip `initialize_agent` if vault exists and just verify everything else.

- [ ] **Step 1.3: Commit**

```bash
git add scripts/setup-test-vault.ts
git commit -m "phase2a: TS setup script for test-vault (off-device, one-time)"
```

---

## Task 2: `agent_pda` module skeleton

**Files:**
- Create: `src/agent_pda.h`
- Create: `src/agent_pda.c`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 2.1: Write `src/agent_pda.h`**

```c
// ---------------------------------------------------------------------------
//  Agent program client (the deployed Anchor program: 9DJqU25...).
//  Derives PDAs and builds Anchor instructions in the wire format the program
//  expects. All callers stay byte-oriented — no base58 in this layer.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Devnet program ID, base58: "9DJqU25ShsEXNisbzSNUPzaN6qiSbU9XiNL7eerqYPFf"
extern const uint8_t AGENT_PROGRAM_ID[32];

// Account-meta inputs to vault_execute's inner-ix payload.
typedef struct {
    const uint8_t *pubkey;     // 32 bytes
    bool           is_signer;
    bool           is_writable;
} agent_inner_meta_t;

// Derive the agent_root PDA from the owner's pubkey.
// Returns the bump or -1 if no canonical bump is found (extremely unlikely).
int agent_pda_derive_root(const uint8_t owner[32], uint8_t out_root[32]);

// Derive the vault PDA from agent_root.
int agent_pda_derive_vault(const uint8_t agent_root[32], uint8_t out_vault[32]);

// Build the wire-format `vault_execute` instruction wrapping an inner ix.
// `inner_program_id` and `inner_metas[*].pubkey` must each point to 32-byte
// buffers; `inner_data` is the inner instruction's raw data bytes.
//
// Outputs a single instruction's bytes (program_id + accounts + data) into
// `out_ix_data`, and the account list (pubkey + signer + writable per slot)
// into `out_metas[]` / `*out_meta_count`. Caller composes these into a v0 tx.
//
// `out_ix_data` capacity: at least 256 + inner_data_len + 32 * inner_meta_count.
// `out_metas` capacity: at least inner_meta_count + 2 (vault + current_signer)
//   plus one for the inner program account.
//
// Returns the number of bytes written into out_ix_data, or -1 on error.
int agent_pda_build_vault_execute_ix(
    const uint8_t        vault_pubkey[32],
    const uint8_t        current_signer[32],
    const uint8_t        inner_program_id[32],
    const agent_inner_meta_t *inner_metas,
    size_t               inner_meta_count,
    const uint8_t       *inner_data,
    size_t               inner_data_len,
    uint8_t             *out_ix_data,
    size_t               out_ix_cap,
    agent_inner_meta_t  *out_metas,
    size_t              *out_meta_count,
    size_t               out_meta_cap);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2.2: Write `src/agent_pda.c` with stubs**

```c
#include "agent_pda.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "agent_pda";

// Decoded base58 of "9DJqU25ShsEXNisbzSNUPzaN6qiSbU9XiNL7eerqYPFf"
const uint8_t AGENT_PROGRAM_ID[32] = {
    // FILL IN AT TASK 2.4
};

int agent_pda_derive_root(const uint8_t owner[32], uint8_t out_root[32]) {
    (void)owner; (void)out_root;
    ESP_LOGE(TAG, "agent_pda_derive_root: NOT IMPLEMENTED");
    return -1;
}

int agent_pda_derive_vault(const uint8_t agent_root[32], uint8_t out_vault[32]) {
    (void)agent_root; (void)out_vault;
    ESP_LOGE(TAG, "agent_pda_derive_vault: NOT IMPLEMENTED");
    return -1;
}

int agent_pda_build_vault_execute_ix(
    const uint8_t vault_pubkey[32],
    const uint8_t current_signer[32],
    const uint8_t inner_program_id[32],
    const agent_inner_meta_t *inner_metas,
    size_t inner_meta_count,
    const uint8_t *inner_data,
    size_t inner_data_len,
    uint8_t *out_ix_data,
    size_t out_ix_cap,
    agent_inner_meta_t *out_metas,
    size_t *out_meta_count,
    size_t out_meta_cap)
{
    (void)vault_pubkey; (void)current_signer; (void)inner_program_id;
    (void)inner_metas; (void)inner_meta_count;
    (void)inner_data; (void)inner_data_len;
    (void)out_ix_data; (void)out_ix_cap;
    (void)out_metas; (void)out_meta_count; (void)out_meta_cap;
    ESP_LOGE(TAG, "agent_pda_build_vault_execute_ix: NOT IMPLEMENTED");
    return -1;
}
```

- [ ] **Step 2.3: Add to `src/CMakeLists.txt`**

Find the `idf_component_register(...)` call and append `agent_pda.c` to the SRCS list.

- [ ] **Step 2.4: Fill in the program ID bytes**

```bash
node -e '
const bs58 = require("bs58").default || require("bs58");
const b = bs58.decode("9DJqU25ShsEXNisbzSNUPzaN6qiSbU9XiNL7eerqYPFf");
console.log([...b].map(x => "0x" + x.toString(16).padStart(2, "0")).join(", "));
'
```

Replace the `// FILL IN` placeholder in `agent_pda.c` with the printed comma-separated bytes.

- [ ] **Step 2.5: Build**

```bash
pio run -e waveshare_esp32s3_28
```

Expected: clean build, the new symbols link (referenced from nowhere yet, but the compile succeeds).

- [ ] **Step 2.6: Commit**

```bash
git add src/agent_pda.h src/agent_pda.c src/CMakeLists.txt
git commit -m "phase2a: agent_pda module skeleton + program ID constant"
```

---

## Task 3: PDA derivation (Ed25519 + sha256 grind)

**Files:**
- Modify: `src/agent_pda.c`

A PDA is found by hashing seeds + a "bump" byte + the program ID + the literal "ProgramDerivedAddress" with sha256, then checking that the resulting point is *off* the Ed25519 curve. Standard Solana derivation. orlp/ed25519 doesn't expose an "is_on_curve" check directly; we'll port the small `is_on_curve` function from the wire-compatible reference.

- [ ] **Step 3.1: Replace stubs in `agent_pda.c` with real derivation**

Add at top:

```c
#include "mbedtls/sha256.h"

// Solana's "ProgramDerivedAddress" sentinel — appended after the seeds during
// hashing so off-curve points produced by find_program_address can never be
// confused with a regular keypair pubkey.
static const char PDA_MARKER[] = "ProgramDerivedAddress";

// Crude is-on-curve check: try to decode the pubkey as a valid Edwards point.
// Returns 1 if on the curve (rejected as a PDA candidate), 0 if off (accepted).
//
// We re-export ge_frombytes_negate_vartime via a small helper in
// components/ed25519/, OR inline the small decode here. Pragmatically we just
// call out to ed25519_pk_is_on_curve which we add to the components/ed25519
// module; a 30-line port of orlp/ed25519's ge_frombytes is enough.
extern int ed25519_pk_is_on_curve(const uint8_t pk[32]);

static int try_pda(const uint8_t **seeds, const size_t *seed_lens, size_t nseeds,
                   uint8_t bump, uint8_t out[32])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    for (size_t i = 0; i < nseeds; i++) {
        mbedtls_sha256_update(&c, seeds[i], seed_lens[i]);
    }
    mbedtls_sha256_update(&c, &bump, 1);
    mbedtls_sha256_update(&c, AGENT_PROGRAM_ID, 32);
    mbedtls_sha256_update(&c, (const uint8_t *)PDA_MARKER, sizeof(PDA_MARKER) - 1);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
    if (ed25519_pk_is_on_curve(out)) return 0;
    return 1;
}

static int find_pda(const uint8_t **seeds, const size_t *seed_lens, size_t nseeds,
                    uint8_t out[32])
{
    for (int bump = 255; bump >= 0; bump--) {
        if (try_pda(seeds, seed_lens, nseeds, (uint8_t)bump, out)) {
            return bump;
        }
    }
    return -1;
}

int agent_pda_derive_root(const uint8_t owner[32], uint8_t out_root[32]) {
    static const uint8_t TAG[] = "agent";
    const uint8_t *seeds[]    = { TAG,       owner };
    const size_t   seed_lens[] = { sizeof TAG - 1, 32 };
    return find_pda(seeds, seed_lens, 2, out_root);
}

int agent_pda_derive_vault(const uint8_t agent_root[32], uint8_t out_vault[32]) {
    static const uint8_t TAG[] = "vault";
    const uint8_t *seeds[]    = { TAG,       agent_root };
    const size_t   seed_lens[] = { sizeof TAG - 1, 32 };
    return find_pda(seeds, seed_lens, 2, out_vault);
}
```

- [ ] **Step 3.2: Add `ed25519_pk_is_on_curve` to the components/ed25519 module**

In `components/ed25519/`, add a `pk_is_on_curve.c` (next to existing source) that wraps orlp/ed25519's `ge_frombytes_negate_vartime`:

```c
#include "ed25519.h"
#include "ge.h"

int ed25519_pk_is_on_curve(const unsigned char pk[32]) {
    ge_p3 A;
    return ge_frombytes_negate_vartime(&A, pk) == 0;
}
```

If the orlp/ed25519 vendored copy doesn't ship `ge.h` publicly, expose `ge_frombytes_negate_vartime` via a tiny header in `components/ed25519/include/`. Build error here means edit `components/ed25519/CMakeLists.txt` to include the new file in `SRCS`.

- [ ] **Step 3.3: Sanity-check derivation in a unit-test verb**

Add to `src/testharness.c`'s verb dispatcher:

```c
} else if (!strcmp(verb, "vault_pda")) {
    extern bool wallet_can_sign(void);
    extern const uint8_t *wallet_pubkey_bytes(void);
    if (!wallet_pubkey_bytes()) { test_reply("err no_pubkey\n"); return; }

    uint8_t root[32], vault[32];
    int rb = agent_pda_derive_root(wallet_pubkey_bytes(), root);
    int vb = agent_pda_derive_vault(root, vault);
    if (rb < 0 || vb < 0) { test_reply("err derive_failed\n"); return; }

    char b58[64];
    base58_encode(root,  32, b58, sizeof b58); test_reply("agent_root %s\n", b58);
    base58_encode(vault, 32, b58, sizeof b58); test_reply("vault %s\n", b58);
}
```

- [ ] **Step 3.4: Build + flash + run**

```bash
pio run -e waveshare_esp32s3_28 -t upload
python tests/run.py "vault_pda"
```

Expected: device prints `agent_root <base58>` and `vault <base58>` matching what `setup-test-vault.ts` printed for the same device pubkey. Cross-check with the on-chain values.

- [ ] **Step 3.5: Commit**

```bash
git add src/agent_pda.c components/ed25519/ src/testharness.c
git commit -m "phase2a: PDA derivation for agent_root/vault + on-curve check"
```

---

## Task 4: Anchor instruction discriminators + InnerIx encoder

**Files:**
- Modify: `src/agent_pda.c`

Anchor encodes instructions as `[discriminator: 8 bytes][borsh-serialized args]`. The discriminator is `sha256("global:<snake_case_name>")[0..8]`. `vault_execute` takes a single argument: the `InnerIx` struct, Borsh-serialized.

Borsh `InnerIx` layout:
```
program_id:  32 bytes
accounts:    u32 little-endian count, then [pubkey:32][is_signer:1][is_writable:1] * count
data:        u32 little-endian length, then bytes
```

- [ ] **Step 4.1: Add discriminator helper**

```c
static void anchor_discriminator(const char *name, uint8_t out[8]) {
    char buf[64];
    int n = snprintf(buf, sizeof buf, "global:%s", name);
    uint8_t hash[32];
    mbedtls_sha256((const uint8_t *)buf, (size_t)n, hash, 0);
    memcpy(out, hash, 8);
}
```

- [ ] **Step 4.2: Implement `agent_pda_build_vault_execute_ix`**

Replace the stub:

```c
int agent_pda_build_vault_execute_ix(
    const uint8_t vault_pubkey[32],
    const uint8_t current_signer[32],
    const uint8_t inner_program_id[32],
    const agent_inner_meta_t *inner_metas,
    size_t inner_meta_count,
    const uint8_t *inner_data,
    size_t inner_data_len,
    uint8_t *out_ix_data,
    size_t out_ix_cap,
    agent_inner_meta_t *out_metas,
    size_t *out_meta_count,
    size_t out_meta_cap)
{
    if (inner_meta_count > 64) return -1;        // sanity
    if (inner_data_len > 1024) return -1;        // sanity

    // ---- ix data: discriminator + Borsh(InnerIx) ----
    uint8_t *p   = out_ix_data;
    uint8_t *end = out_ix_data + out_ix_cap;
    if ((size_t)(end - p) < 8) return -1;
    anchor_discriminator("vault_execute", p);
    p += 8;

    // InnerIx.program_id
    if ((size_t)(end - p) < 32) return -1;
    memcpy(p, inner_program_id, 32);
    p += 32;

    // InnerIx.accounts (u32 little-endian count, then [pk32][signer1][writable1] * count)
    if ((size_t)(end - p) < 4) return -1;
    uint32_t n = (uint32_t)inner_meta_count;
    p[0] = (uint8_t)(n);  p[1] = (uint8_t)(n>>8); p[2] = (uint8_t)(n>>16); p[3] = (uint8_t)(n>>24);
    p += 4;
    for (size_t i = 0; i < inner_meta_count; i++) {
        if ((size_t)(end - p) < 34) return -1;
        memcpy(p, inner_metas[i].pubkey, 32); p += 32;
        *p++ = inner_metas[i].is_signer  ? 1 : 0;
        *p++ = inner_metas[i].is_writable ? 1 : 0;
    }

    // InnerIx.data (u32 length + bytes)
    if ((size_t)(end - p) < 4 + inner_data_len) return -1;
    p[0] = (uint8_t)(inner_data_len);
    p[1] = (uint8_t)(inner_data_len >> 8);
    p[2] = (uint8_t)(inner_data_len >> 16);
    p[3] = (uint8_t)(inner_data_len >> 24);
    p += 4;
    memcpy(p, inner_data, inner_data_len);
    p += inner_data_len;

    int ix_data_len = (int)(p - out_ix_data);

    // ---- account metas for the OUTER vault_execute ----
    // vault (writable, NOT signer at outer level — PDA),
    // current_signer (signer),
    // then forward all inner metas (signers stripped — PDA signs them via CPI seeds),
    // then the inner program account itself.
    size_t need = 2 + inner_meta_count + 1;
    if (need > out_meta_cap) return -1;
    size_t k = 0;
    out_metas[k++] = (agent_inner_meta_t){ .pubkey = vault_pubkey,    .is_signer = false, .is_writable = true  };
    out_metas[k++] = (agent_inner_meta_t){ .pubkey = current_signer,  .is_signer = true,  .is_writable = false };
    for (size_t i = 0; i < inner_meta_count; i++) {
        out_metas[k++] = (agent_inner_meta_t){
            .pubkey      = inner_metas[i].pubkey,
            .is_signer   = false,                          // PDA path
            .is_writable = inner_metas[i].is_writable,
        };
    }
    out_metas[k++] = (agent_inner_meta_t){ .pubkey = inner_program_id, .is_signer = false, .is_writable = false };
    *out_meta_count = k;

    return ix_data_len;
}
```

- [ ] **Step 4.3: Add a unit-test verb that round-trips a known InnerIx**

In `src/testharness.c`, add:

```c
} else if (!strcmp(verb, "vault_disc")) {
    uint8_t d[8];
    extern void anchor_discriminator(const char *, uint8_t[8]); // expose static or move to header
    // Actually we can't call a static helper from another TU — instead,
    // call agent_pda_build_vault_execute_ix with empty args and read the
    // first 8 bytes of out_ix_data.
    uint8_t buf[256];
    agent_inner_meta_t metas[8];
    size_t mc;
    uint8_t empty_pubkey[32] = {0};
    int len = agent_pda_build_vault_execute_ix(
        empty_pubkey, empty_pubkey, empty_pubkey,
        NULL, 0, NULL, 0,
        buf, sizeof buf, metas, &mc, sizeof metas / sizeof metas[0]);
    if (len < 8) { test_reply("err encode\n"); return; }
    char hex[32];
    snprintf(hex, sizeof hex,
             "%02x%02x%02x%02x%02x%02x%02x%02x",
             buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
    test_reply("disc %s\n", hex);
}
```

The expected discriminator can be computed off-device:

```bash
node -e '
const c = require("crypto");
console.log(c.createHash("sha256").update("global:vault_execute").digest("hex").slice(0,16));
'
```

- [ ] **Step 4.4: Flash + run**

```bash
pio run -e waveshare_esp32s3_28 -t upload
python tests/run.py "vault_disc"
```

Expected: the device's `disc <hex>` matches the host-computed value.

- [ ] **Step 4.5: Commit**

```bash
git add src/agent_pda.c src/testharness.c
git commit -m "phase2a: anchor instruction discriminator + Borsh InnerIx encoder"
```

---

## Task 5: `solana_tx.c` v2 — multi-instruction message builder

**Files:**
- Modify: `src/solana_tx.h`
- Modify: `src/solana_tx.c`

Today `solana_build_signed_tx_base64` hardcodes the message layout for a single TransferChecked + ComputeBudget. We need a generic path that accepts a list of `solana_ix_t` blobs (program account index + account index list + data) and assembles the v0 message. The v1 path stays untouched so swap.c (Phase 2a-swap) and any other callers keep working.

- [ ] **Step 5.1: Add v2 types to `solana_tx.h`**

```c
typedef struct {
    const uint8_t *program_id;     // 32 bytes
    const uint8_t *const *account_pubkeys;  // each entry: 32 bytes
    const uint8_t *account_signer; // bool[count] — 1 if this account is a signer
    const uint8_t *account_writable; // bool[count]
    size_t         account_count;
    const uint8_t *data;
    size_t         data_len;
} solana_ix_v2_t;

typedef struct {
    const uint8_t *fee_payer;       // 32 bytes
    const uint8_t *signer_pubkey;   // 32 bytes — our wallet (signs the msg)
    const uint8_t *blockhash;       // 32 bytes
    const solana_ix_v2_t *ixs;
    size_t                ix_count;
    uint32_t              cu_limit;
    uint64_t              cu_price_micro;
} solana_tx_input_v2_t;

int solana_build_tx_v2_base64(const solana_tx_input_v2_t *in,
                              char *out, size_t out_cap);
```

- [ ] **Step 5.2: Implement `solana_build_tx_v2_base64` in `solana_tx.c`**

Skeleton (you'll need to walk the v0 message format — header, account_keys table with dedup, blockhash, instruction list with compact-u16-encoded account indices + data length, then the wallet signature in slot 1, fee-payer slot 0 left zeroed):

```c
// (Full impl — see existing solana_build_signed_tx_base64 for the wire shape;
//  generalize the instruction-emission loop instead of hardcoding TransferChecked
//  + ComputeBudget. Account-key dedup table: collect every unique pubkey across
//  all ixs (and fee_payer + signer), order them by writability + signer status
//  per the v0 spec.)
//
// ... full implementation here, ~150 lines of careful encoding ...
```

(The full implementation is mechanical — emit signature slots, message header, compact-u16 account count, account list, blockhash, compact-u16 ix count, then for each ix: program-id index, compact-u16 account index list, compact-u16 data length, data. Sign the message body with `wallet_sign`, splice into slot 1.)

- [ ] **Step 5.3: Build**

```bash
pio run -e waveshare_esp32s3_28
```

Expected: clean build.

- [ ] **Step 5.4: Add a self-test verb**

In `testharness.c` add `vault_v2_dryrun`: builds an empty-instruction-list v2 tx, base64s it, prints the size. Just confirms the new path doesn't crash.

- [ ] **Step 5.5: Flash + run**

```bash
pio run -e waveshare_esp32s3_28 -t upload
python tests/run.py "vault_v2_dryrun"
```

Expected: prints a base64 length and a non-error status.

- [ ] **Step 5.6: Commit**

```bash
git add src/solana_tx.h src/solana_tx.c src/testharness.c
git commit -m "phase2a: solana_tx v2 — multi-instruction v0-message builder"
```

---

## Task 6: x402.c migration to vault_execute

**Files:**
- Modify: `src/wallet.h`
- Modify: `src/wallet.c`
- Modify: `src/x402.c`

The x402 path today builds a single TransferChecked from the device's USDC ATA → recipient ATA, signed by the device key. After this task: same TransferChecked, but the source is the *vault PDA's* USDC ATA, the authority is the vault PDA, and the whole thing is wrapped in `vault_execute`.

- [ ] **Step 6.1: Expose vault PDA from wallet.c**

Add to `wallet.h`:

```c
// 32-byte vault PDA derived from the device's pubkey. NULL if !wallet_can_sign().
const uint8_t *wallet_vault_pda_bytes(void);
const char    *wallet_vault_pda_b58(void);
```

In `wallet.c`, derive and cache the vault PDA in `wallet_begin()` after the device pubkey is loaded.

- [ ] **Step 6.2: Refresh against the vault's USDC ATA**

Today `wallet_refresh()` queries `getTokenAccountsByOwner` for the device pubkey. For Phase 2a, change it to query the *vault PDA's* USDC ATA balance instead. (Simplest: track both; report the vault's USDC for x402 affordability checks.)

- [ ] **Step 6.3: Build the inner TransferChecked + wrap in `x402.c`**

Find the existing `solana_tx_input_t in = { ... }` block (around line 363). Replace with:

```c
// Inner: TransferChecked from VAULT's USDC ATA → recipient ATA, authority = vault PDA.
const uint8_t *vault_pk = wallet_vault_pda_bytes();
if (!vault_pk) { ESP_LOGW(TAG, "no vault pda"); goto out; }

uint8_t inner_data[10];
inner_data[0] = 12;                       // SPL token TransferChecked discriminator
memcpy(&inner_data[1], &amount_atomic, 8);
inner_data[9] = USDC_DECIMALS;

const agent_inner_meta_t inner_metas[] = {
    { source_ata,    false, true  },     // from
    { mint_key,      false, false },     // mint
    { dest_ata,      false, true  },     // to
    { vault_pk,      true,  false },     // authority (vault PDA — signed via CPI)
};
static const uint8_t TOKEN_PROGRAM_ID[32] = { /* base58 "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA" */ };

uint8_t outer_ix_data[256];
agent_inner_meta_t outer_metas[16];
size_t outer_meta_count = 0;
int outer_data_len = agent_pda_build_vault_execute_ix(
    vault_pk,
    wallet_pubkey_bytes(),                     // current_signer
    TOKEN_PROGRAM_ID,                          // inner program
    inner_metas, 4,
    inner_data, sizeof inner_data,
    outer_ix_data, sizeof outer_ix_data,
    outer_metas, &outer_meta_count, sizeof outer_metas / sizeof outer_metas[0]);
if (outer_data_len < 0) goto out;

solana_ix_v2_t ix = {
    .program_id      = AGENT_PROGRAM_ID,
    .account_pubkeys = /* array of out_metas[*].pubkey pointers */,
    .account_signer  = /* array of out_metas[*].is_signer */,
    .account_writable= /* array of out_metas[*].is_writable */,
    .account_count   = outer_meta_count,
    .data            = outer_ix_data,
    .data_len        = outer_data_len,
};

solana_tx_input_v2_t txin = {
    .fee_payer      = fee_payer_key,
    .signer_pubkey  = wallet_pubkey_bytes(),
    .blockhash      = blockhash,
    .ixs            = &ix, .ix_count = 1,
    .cu_limit       = 50000,                // bumped from 8000 — vault wrap costs ~5k extra
    .cu_price_micro = 1,
};
char tx_b64[1024];
int tx_len = solana_build_tx_v2_base64(&txin, tx_b64, sizeof tx_b64);
if (tx_len <= 0) { ESP_LOGW(TAG, "vault tx build failed"); goto out; }
```

The `source_ata` here changes meaning — it's now the *vault's* USDC ATA, not the device's. Resolve it via `wallet_vault_pda_bytes` + `findATA(mint, vault_pda)`. The recipient `dest_ata` is unchanged.

- [ ] **Step 6.4: Build**

```bash
pio run -e waveshare_esp32s3_28
```

Expected: clean build. (The token program ID constant in step 6.3 needs the actual 32 bytes — derive via the same node oneliner used for AGENT_PROGRAM_ID.)

- [ ] **Step 6.5: Commit**

```bash
git add src/wallet.h src/wallet.c src/x402.c
git commit -m "phase2a: x402 USDC payments routed through vault_execute"
```

---

## Task 7: End-to-end smoke verb

**Files:**
- Modify: `src/testharness.c`

Drives one full vault-mediated x402 payment against devnet, end to end.

- [ ] **Step 7.1: Add `vault_x402_test` verb**

Calls one of the existing x402 services with a tiny amount (or a dedicated test endpoint). Reports `ok <txid>` on success, `err <reason>` on failure.

- [ ] **Step 7.2: Run on hardware**

Pre-requirements:
- Phase 1's deployed program already on devnet (✓ done)
- Setup script run for this device's pubkey (Pre-1 + Pre-2)
- Vault funded with at least 0.01 USDC + 0.01 SOL

```bash
pio run -e waveshare_esp32s3_28 -t upload
python tests/run.py "vault_x402_test"
```

Expected: prints a base58 tx signature. Verify on `solscan.io/?cluster=devnet` that:
- The tx invoked `agent_program` (the deployed one)
- `vault_execute` was called
- Inner TransferChecked emptied USDC from the vault's ATA to the recipient

- [ ] **Step 7.3: Commit**

```bash
git add src/testharness.c
git commit -m "phase2a: vault_x402_test harness verb (devnet end-to-end)"
```

---

## Task 8: Real-flow verification (chat reply triggers x402)

**Files:** none (run-only)

- [ ] **Step 8.1: Power-on the device, ask the daemon a question**

Expected:
- Subtitle with the LLM's reply appears on screen.
- A new devnet tx hits the program, draining a small USDC amount from the vault.
- Wallet balance ticker on the device shows the vault's USDC dropping.

- [ ] **Step 8.2: Confirm on-chain**

`solana confirm <tx_sig> --url devnet` for the tx that the device just submitted (read from device logs).

If everything ✓, Phase 2a-x402 is done. Phase 2a-swap (Jupiter migration) and Phase 2b (hardware-protected signer) get their own plans.

---

## Risks and open decisions

- **Token program ID hardcoded** — fine; the SPL token program at `TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA` is canonical and never changes.
- **Vault must be pre-funded** with both SOL (for rent + tx fees) and USDC (for actual payments). Phase 2c wizard will automate this; for Phase 2a it's a manual setup-script step.
- **Compute budget bumped 8000 → 50000** to absorb the vault_execute overhead. If tx still fails for "out of compute units", bump higher.
- **Tx size** — the vault wrapper adds ~50 bytes. With one inner TransferChecked the total is well under the 1232-byte v0 limit; Jupiter swaps (Phase 2a-swap) will need to revisit.
- **First-tx delay** — the vault PDA's USDC ATA has to exist before the first payment. The setup script creates it, but if it gets deallocated somehow (rent), reads will fail until it's recreated. Worth catching with an explicit error message.
- **PDA derivation cost** — sha256 + on-curve check, looped for the bump. ~5-10 ms on ESP32-S3. Cache the result; don't re-derive per tx.
- **No PIN gating yet** — Phase 2b adds it. Until then a stolen device can spend the vault until the user rotates from Phantom.
