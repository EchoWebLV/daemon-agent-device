# On-chain agent (vault + identity + memory) — Design Spec

## Goal

Move the Daemon's identity, funds, and memory from device-local NVS onto Solana, so the agent **lives on-chain** and the device becomes a swappable body. Stolen, broken, or replaced devices no longer destroy the agent — funds, persona, voice, services, and chat history all survive on-chain and can be resurrected on fresh hardware with one Phantom signature.

This is the "Option C — Full stack" path: vault + identity + memory PDAs, with the chat archive on Shadow Drive (mutable, cheap) and a content-hash anchor on-chain for integrity.

## Trust model

- **Funds** live in a vault PDA owned by a custom Anchor program. The vault has two roles: `current_signer` (the device, full day-to-day spending authority) and `recovery_authority` (the user's Phantom, can rotate the signer or sweep funds).
- **Persona, voice, services, memory** live in PDAs derived from the same `agent_root`. Updates require a tx signed by `current_signer` (or `recovery_authority` for recovery-class changes).
- **Device signing key** is hardware-protected (flash encryption + secure boot v2 + PIN-wrapped NVS + wipe-on-fail). Even with the device in hand, an attacker without the PIN can't sign anything; with enough wrong PINs the seed is wiped.
- **The recovery authority is the only single point of catastrophic loss.** If the user loses their Phantom *and* their device, funds are gone — same as any non-custodial wallet.

## Threat coverage

| Scenario | Outcome |
|---|---|
| Device pickpocketed / lost | Wipe-on-fail kicks in after N PIN tries; even before that, attacker can't authenticate to sign. User rotates the signer from Phantom; replacement device adopts the same agent. |
| Device's flash chip pulled and read off-board | Ciphertext only (flash encryption with eFuse-locked key). |
| Custom firmware flashed | Secure Boot v2 refuses to run unsigned firmware. |
| Sophisticated lab attack extracts device key | Stolen key is `current_signer` — Phantom rotates it, key becomes worthless. Phantom can also sweep before/after. |
| Phantom compromised but device safe | Attacker can rotate the signer. Mitigation: optional secondary recovery authority (out of scope for v1). |
| Both device and Phantom lost | Funds unrecoverable — accepted limit of non-custodial design. |
| User wants to migrate to a new device | Pair new device → it generates a new keypair → Phantom signs `rotate_signer(new_pk)` → done. Persona, memory, funds preserved. |

## Out of scope (v1)

- **Multi-body / parallel bodies.** v1 is solo-body: at any time, exactly one device is `current_signer`. Multi-body (e.g. phone mirror + desktop daemon driving the same agent concurrently) is v2 because it requires write-conflict handling on the memory PDA.
- **Spending limits / per-tx allowlist** in the vault. v1 trusts `current_signer` fully because the hardware protections make compromise expensive and the recovery authority is the kill switch. Limits can be added later without breaking the program's account layout.
- **External Secure Element (ATECC608A).** Pin-routed and BOM-feasible but adds ~1 week of integration. Recommended for v2 if the daemon will hold non-trivial balances.
- **Decentralized inference.** The LLM still runs at `sol.blockrun.ai` / the x402 gateway. "Agent on-chain" here means *identity + state*, not *inference*. Verifiable inference is a separable v2 question.
- **Migration of existing v0 daemons.** Assumed greenfield — current `espidf-skeleton` branch has not shipped to users with funded wallets.

## Architecture overview

```
                           ┌──────────────────────────┐
                           │  Phantom (recovery key)  │
                           └────────────┬─────────────┘
                                        │ rotate / sweep / update-identity
                                        ▼
   ┌──────────────────────────────────────────────────────────────┐
   │   Anchor program: agent_program (one deploy, all daemons)    │
   │                                                              │
   │   PDA: agent_root  (seed = ["agent", owner_pubkey])          │
   │     ├── PDA: vault     ──→ holds SOL + token ATAs            │
   │     ├── PDA: identity  ──→ name, prompt, voice, services     │
   │     └── PDA: memory    ──→ recent turns, summary, log head   │
   └──────────────┬──────────────────────────┬────────────────────┘
                  │                          │
                  │ vault.execute(ix)        │ memory.append_pointer
                  │ identity.update(...)     │ identity.update(...)
                  ▼                          ▼
            ┌─────────────────────┐    ┌────────────────────────┐
            │ ESP32-S3 device     │    │ Shadow Drive bucket    │
            │ (current_signer)    │    │ (chat log archive)     │
            └─────────────────────┘    └────────────────────────┘
```

**Day-to-day:** the device signs vault.execute / identity.update / memory.append_pointer transactions exactly the way it signs USDC TransferChecked today. No co-signer, no network round-trip, no UX change.

**Body swap:** new device generates a keypair, prints its pubkey on screen, user signs `rotate_signer(new_pk)` in Phantom, new device fetches identity + memory PDAs and resumes the agent.

## On-chain components

### Anchor program: `agent_program`

Single program deployed once by the project owner. All daemons share this program; per-daemon state is in PDAs derived from `owner_pubkey`.

#### PDA seeds

```
agent_root  : ["agent", owner_pubkey]
vault       : ["vault", agent_root]
identity    : ["identity", agent_root]
memory      : ["memory", agent_root]
```

#### Account layouts (sketch)

```rust
#[account]
pub struct AgentRoot {
    pub owner: Pubkey,             // = recovery_authority
    pub vault: Pubkey,             // PDA
    pub identity: Pubkey,          // PDA
    pub memory: Pubkey,            // PDA
    pub created_at: i64,
    pub bump: u8,
}

#[account]
pub struct Vault {
    pub agent_root: Pubkey,
    pub current_signer: Pubkey,    // device pubkey
    pub recovery_authority: Pubkey,
    pub signer_rotated_at: i64,
    pub bump: u8,
}

#[account]
pub struct Identity {
    pub agent_root: Pubkey,
    pub name: [u8; 32],            // null-padded UTF-8
    pub system_prompt: [u8; 2048], // length-prefixed; truncate at write
    pub voice_provider: u8,        // 0 = ElevenLabs, 1 = Piper, ...
    pub voice_id: [u8; 32],
    pub services_count: u8,
    pub services: [ServiceEntry; 16],
    pub updated_at: i64,
    pub bump: u8,
}

#[account]
pub struct Memory {
    pub agent_root: Pubkey,
    pub recent_count: u8,
    pub recent_turns: [TurnSlot; 10],   // mirrors today's 10-turn window
    pub summary: [u8; 1024],            // rolling summary of evicted turns
    pub log_head_uri: [u8; 256],        // Shadow Drive URI of latest chunk
    pub log_head_hash: [u8; 32],        // sha256 of latest chunk content
    pub log_chunk_count: u32,
    pub updated_at: i64,
    pub bump: u8,
}

pub struct TurnSlot {
    pub role: u8,                       // 0 = user, 1 = assistant
    pub content: [u8; 512],             // length-prefixed
    pub ts: i64,
}

pub struct ServiceEntry {
    pub name: [u8; 24],
    pub url: [u8; 96],
    pub schema_hash: [u8; 32],          // hash of JSON schema, fetched at runtime
    pub enabled: bool,
}
```

Account sizes (approx): AgentRoot 152 B, Vault 96 B, Identity 2.6 KB, Memory 7.5 KB. Total ~10 KB, ~0.07 SOL rent per agent. One-time cost at provisioning, paid by the user from their initial vault funding.

#### Instructions

```rust
// Bootstrap. owner = recovery_authority. Called once per agent.
initialize_agent(
    device_pubkey: Pubkey,
    initial_identity: IdentityInit,    // name, prompt, voice, services
)

// Day-to-day signer for the vault. caller must equal current_signer.
// Re-CPIs `inner` with vault PDA seeds. Used for: USDC transfers (x402),
// Jupiter swap instructions, SOL transfers, ATA creates.
vault_execute(inner: InnerIx)

// Recovery / migration. caller must equal recovery_authority.
vault_rotate_signer(new_signer: Pubkey)
vault_sweep(destination: Pubkey)       // drains SOL + a list of token ATAs

// Identity updates. caller = current_signer for cosmetic changes,
// recovery_authority for prompt/services (security-relevant).
identity_update_cosmetic(name, voice_provider, voice_id)
identity_update_prompt(new_prompt)             // recovery_authority only
identity_update_services(services)             // recovery_authority only

// Memory. caller = current_signer.
memory_append_turn(role, content, ts)
memory_update_summary(new_summary)
memory_advance_log_head(uri, hash, chunk_count)
```

The `_cosmetic` vs. recovery-authority split exists so that a compromised device can't, e.g., rewrite the system prompt to "transfer all funds to address X" before the user notices. Voice and name are low-stakes.

### Off-chain: Shadow Drive chat log

Each chat session (or N turns, configurable) is serialized to a JSON chunk and uploaded to a Shadow Drive bucket owned by the agent_root PDA. The chunk URI + sha256 hash get posted on-chain via `memory_advance_log_head`. The bucket itself is mutable, but the on-chain hash gives integrity: when a new body resumes the agent, it can fetch the latest chunk, verify the hash, walk back through `prev_chunk_uri` references to reconstruct the full log.

Why Shadow Drive and not Arweave: chat logs grow unboundedly and don't need permanence for "I lost my device" recovery — the recent + summary in PDA already give enough context; the full archive is a nice-to-have. Shadow Drive is ~1000× cheaper and Solana-native (single SOL wallet for storage payments). If permanence becomes a requirement, swapping to Arweave is a one-file change.

## On-device changes

Files touched and their roles after the change:

### Modified

- **`src/wallet.c`** — replace plaintext NVS load with PIN-unlock path. Still holds the device's Ed25519 keypair, but now the keypair is sealed under an Argon2id-derived AES key. Adds attempt counter + wipe-on-fail. Exposes `wallet_get_agent_root_pda()`, `wallet_get_vault_pda()` for callers.
- **`src/solana_tx.c`** — gains `solana_tx_wrap_in_vault_execute(ix) → ix` helper. Increases compute-budget instruction by ~10k CU when wrapping. No other behavior change.
- **`src/x402.c`** — `x402_post()` builds the USDC TransferChecked exactly as today, then wraps it via `solana_tx_wrap_in_vault_execute()` before signing. The gateway sees a normal USDC arrival; only the on-chain tx shape differs. Memory note about fresh blockhashes still applies — wrapping does not change blockhash semantics.
- **`src/swap.c`** — switch the Jupiter call from `/swap` to `/swap-instructions`. Parse the returned instruction array, wrap each in vault.execute, build a v0 message with the wrapped sequence, sign + submit. The 3-second hold-to-confirm modal in `swap_screen.c` is unchanged.
- **`src/ai.c`** — instead of holding the 10-turn window in RAM only, mirror writes to the memory PDA via `memory_append_turn`. System prompt is read from identity PDA at boot (cached in RAM). Service registry is read from identity PDA at boot. Updates to either invalidate the cache.
- **`src/devcfg.c`** — strips out personality/voice/services storage (these now live in identity PDA). Keeps Wi-Fi credentials only; everything else moves on-chain.
- **`partitions.csv`** — add a small `nvs_secure` partition (encrypted) for the sealed seed + attempt counter.
- **`sdkconfig.defaults`** — enable Flash Encryption (release mode), Secure Boot v2, NVS encryption.

### New

- **`src/pin.c` / `src/pin.h`** — PIN keypad LVGL UI, Argon2id KDF, attempt counter, wipe-on-fail. Called on cold boot before any wallet op.
- **`src/agent_pda.c` / `src/agent_pda.h`** — typed wrappers over the Anchor program's instructions: `agent_initialize()`, `vault_execute()`, `vault_rotate_signer()`, `identity_update_*()`, `memory_*()`. Builds instructions, fetches account data, decodes account layouts.
- **`src/memory_log.c` / `src/memory_log.h`** — Shadow Drive client: chunk packing, sha256, upload, fetch, hash verify. Used both for writing new turns and for resuming an agent on a fresh body.
- **`src/wizard/`** — first-boot captive-portal wizard. Hosts an HTTP server on the device's AP, serves an embedded HTML wizard (using the existing `scripts/embed_assets.py` pre-build hook), drives the user through Wi-Fi → personality → voice → services → PIN → backup-disclaimer → fund-vault QR.

### Unchanged

- **`src/swap_screen.c`** — same 3-second hold-to-confirm UX. Inputs are the same; only the tx that gets signed underneath is shaped differently.
- **`src/app_main.c`** — entry point unchanged except for the cold-boot path through `pin_unlock()` before anything else.
- **Voice / TTS path** — ElevenLabs voice ID is fetched from identity PDA instead of NVS, but the TTS call itself is identical.

## Hardware-protected device signer

This stack lives independently of the on-chain pieces — it protects the `current_signer` keypair on the device itself.

1. **Flash Encryption (release mode).** AES-256, key burned into eFuses with read bits blown. Attacker pulling the flash chip off the PCB sees ciphertext only.
2. **Secure Boot v2** with RSA-3072 or ECDSA-P256. Bootloader verifies firmware signature against an eFuse-burned public key. Custom firmware can't run.
3. **PIN-wrapped seed.** The Ed25519 seed is sealed in NVS encrypted with an AES key derived from the user's PIN via Argon2id (`m_cost = 64 MB` if PSRAM-backed, `t_cost = 3`, `parallelism = 1`). The unwrapped seed exists in RAM only while the device is unlocked.
4. **Wipe-on-fail.** Wrong-PIN counter in encrypted NVS. After 5 failures, the `nvs_secure` partition is erased and the seed is gone. User must restore via Phantom (rotate to a new device).
5. **JTAG / USB-Serial-JTAG disabled.** Burned via eFuse before shipping.

For a v2 hardening pass, an **ATECC608A** secure element on the I²C bus would let us store the seed in tamper-resistant silicon and perform Ed25519 signing without ever loading the seed into ESP32 RAM. ~$0.70 BOM, ~1 week of integration. Out of scope for v1 but the agent_pda.c interface is designed so the signer back-end is swappable.

## Wizard / first-run UX

On first boot the device:

1. Generates a fresh Ed25519 keypair (TRNG with radio active for full entropy).
2. Brings up a Wi-Fi AP named `daemon-setup-XXXX` (last 4 of pubkey).
3. Shows on-screen: *"Connect to `daemon-setup-XXXX` and open `daemon.local` on your phone."* + QR code with the captive URL.
4. Phone browser hits the captive portal, gets the wizard:

   - **Wi-Fi** — pick home network + password.
   - **Phantom pairing** — page links into Phantom's deep-link flow ("connect wallet"). Owner pubkey captured.
   - **Personality** — name + system-prompt textarea.
   - **Voice** — ElevenLabs voice picker with previews.
   - **Services** — toggle x402 services from a default list; user can add custom ones.
   - **PIN** — 4–6 digits, confirm twice.
   - **Confirm + fund** — wizard shows two addresses: the vault PDA (gets bulk SOL + USDC) and the device key (gets ~0.05 SOL for tx fees). Phantom deep-links pre-fill both transfers.

5. Once funds arrive (device polls both accounts), device sends `initialize_agent` with the captured fields. The instruction also creates the vault PDA's USDC associated-token account so subsequent transfers have somewhere to land. Tx fee paid from the device key's SOL.
6. Device displays *"Daemon online. Say hello."* and switches to the normal agent UI.

There is **no seed-phrase reveal step** — the device's keypair is a delegated signer, not the wallet. The user's recovery key is their existing Phantom; nothing new to back up.

## Recovery / body-swap UX

A small static web page hosted at a stable URL (e.g. `recover.<project-domain>`). Three actions:

- **Rotate signer.** User pastes the new device's pubkey (shown on the new device's screen on first boot), Phantom signs `vault_rotate_signer`. Old device's next tx fails with `WrongSigner`; new device immediately works.
- **Sweep.** User picks a destination address, Phantom signs `vault_sweep`. SOL + listed token ATAs drain. Used for "burn it down — I want my funds out".
- **Update prompt / services** (recovery-authority instructions). Useful if the user wants to retune the agent without provisioning a new body.

The recovery dApp is plain Solana web3.js; no backend, no custodial state. Agent_root PDA addresses are deterministic from `["agent", owner_pubkey]`, so the page only needs the user's Phantom — it derives everything else.

## Phased delivery (~7–8 weeks)

| Phase | Weeks | Deliverable |
|---|---|---|
| 1. Anchor program (vault only) | 2 | `agent_program` deployed to devnet. `initialize`, `vault_execute`, `rotate_signer`, `sweep` working. Unit + integration tests against a mocked daemon. |
| 2. Hardware-protected signer + minimal wizard | 2 | `pin.c`, sealed NVS, secure boot, flash encryption verified on hardware. Wizard reduced to Wi-Fi + Phantom pair + PIN. Device can sign vault.execute against the program. x402 + swap paths migrated to vault.execute. |
| 3. Identity PDA | 1 | Persona / voice / services move from NVS to PDA. Wizard fills out identity fields. Bodies fetch identity at boot. `identity_update_*` instructions wired up. |
| 4. Memory PDA + Shadow Drive | 2 | Recent-turns + summary in PDA. Shadow Drive client. Log chunks uploaded + hash-anchored. Resume-on-fresh-body verified end-to-end (wipe device → re-pair → identity + memory restored). |
| 5. Recovery dApp + audit prep | 1 | Static web page with rotate / sweep / update-prompt. Audit-readiness pass: program freeze, IDL published to a registry so Phantom shows readable instruction names. |

Audit (external, on the Anchor program) is a separate ~2-week wall-clock item that should run in parallel with phase 4–5 and gate public launch.

## Testing

- **Anchor program**: full unit tests with `solana-program-test`. Integration tests using a stub daemon that exercises every instruction permutation, including failure cases (`wrong_signer`, `not_recovery_authority`, oversized strings, replay protection).
- **Device firmware**: extend the existing e2e harness (`docs/superpowers/specs/2026-04-22-e2e-device-tests-design.md`) with a "swap with vault wrapper" case and a "fresh body resume" case. The latter wipes NVS, re-pairs, and asserts the resumed daemon has the original persona + recent turns.
- **Body-swap drill**: scripted "lose your device" simulation — kill power on device A, rotate from Phantom, boot device B with the new keypair, confirm A's next tx fails and B operates normally.
- **Wipe-on-fail**: 5 wrong PINs in succession on real hardware → confirm `nvs_secure` is erased and the device prompts for fresh provisioning.

## Performance impact

- **Chat reply latency:** unchanged. Vault wrapper adds ~5–8k CU (validator-side); device-side wrap is microseconds. Memory PDA writes happen async after the reply lands.
- **Swap latency:** unchanged. Wrapping Jupiter instructions adds ~1–2 ms of ESP32 CPU. Submit + confirm timing identical.
- **Cold boot:** +2–4 s. Argon2id PIN unlock is intentionally slow (~1–3 s) for brute-force resistance; PDA state fetch (`getMultipleAccounts` for identity + memory + vault) adds ~200–400 ms. Idle re-lock (default 30 min) keeps the device warm during normal use.
- **Body resume on fresh hardware:** identity + recent-turns ready in <1 s; full archive walk from Shadow Drive is lazy / background.
- **Recovery from theft:** *faster* — Phantom + recovery dApp completes in seconds vs. minutes for seed-import.

Implementation discipline: boot fetches must be parallelized (`getMultipleAccounts`, not sequential), the existing ATA cache in `x402.c` extends to the vault's source ATA, and memory writes batch every N turns or 30 s idle to keep tx volume low.

## Risks and open decisions

- **Tx size with Jupiter wrappers.** Solana's 1232-byte limit already pinches deep Jupiter routes; the vault wrapper adds ~50 bytes. Mitigation: probe at phase 2 and, if real routes break, route through Jupiter's "compact" mode or fall back to single-pool routes. Worst case: detect oversize, decline the swap with a clear error.
- **Compute budget on swap path.** Wrapping ≥4 inner instructions in vault_execute may approach the 1.4M CU per-tx cap. Mitigation: bump explicit compute budget; if still tight, split the swap across two transactions (rare, only for pathological routes).
- **Shadow Drive availability / ToS changes.** GenesysGo / Shadow has had operational hiccups historically. Mitigation: agent stays functional from PDA-stored recent + summary even if Shadow is unreachable; only the deep archive depends on Shadow. Swappable to Arweave or IPFS+Filecoin via the `memory_log.c` interface.
- **Phantom IDL display.** Recovery instructions will show as "Unknown" without a published IDL. Resolved by phase 5 (publish IDL to anchor's registry).
- **Program upgrade authority.** Held by the project owner during v1; eventually move to a multisig. Document the planned authority transfer at phase 5 so users see the trust path.
- **Device-key SOL exhaustion.** The device pays ~5000 lamports per tx; ~0.05 SOL covers ~10k txs (~$0.0008 each). When the device key runs low, the device emits a `vault_execute → SystemProgram.transfer(vault → device, 0.02 SOL)` to top itself up. This is a vault-signed self-refill, not a privileged instruction; cap it at, e.g., 0.05 SOL ceiling per refill to bound abuse if the device key is compromised before rotation.
- **Multi-body support.** Out of scope for v1. The memory PDA's `updated_at` field gives a foundation for optimistic-concurrency in v2 but the program does not enforce it yet.
- **PIN friction on wake.** Every cold boot prompts for PIN. Idle re-lock after N minutes is configurable; default 30. Power-cycle always re-prompts. This is the same UX as Ledger / phone unlock and is acceptable for an autonomous agent that mostly stays powered.

## Open questions for the user

1. **Wizard control surface.** Captive-portal phone-browser flow (recommended; better keyboard, more screen) or pure on-device LVGL wizard (no phone required, but typing the system prompt on a 2.8" screen is rough)? Spec assumes captive portal.
2. **Service registry shape.** Identity PDA caps services at 16 entries with fixed-size names/URLs. If users want more or longer URLs, we either (a) bump the cap (cheap, more rent), or (b) move services to a Shadow Drive pointer like memory. Spec assumes 16-entry cap.
3. **Recovery dApp hosting.** Vercel / Cloudflare Pages on a project-owned domain, or self-host on the device's captive portal as well so users without internet on their phone can still recover from the device's own AP? Spec assumes hosted.
4. **Idle re-lock timeout default.** 30 min picked arbitrarily; finalize after UX testing.
5. **Initial funding flow.** Does the wizard auto-fund the vault from the user's Phantom (Phantom signs a transfer) or just give them the address and let them send manually? Auto is smoother but requires deeper Phantom integration.
