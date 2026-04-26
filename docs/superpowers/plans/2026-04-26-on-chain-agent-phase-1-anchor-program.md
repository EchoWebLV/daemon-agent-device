# On-chain Agent — Phase 1: Anchor Program (vault) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a deployed-to-devnet Anchor program (`agent_program`) that exposes the vault primitives the on-chain agent design depends on: `initialize_agent`, `vault_execute`, `vault_rotate_signer`, `vault_sweep`. Identity and memory PDAs are out of scope for this phase — they get their own phases.

**Architecture:** Single-program Anchor workspace nested inside the existing PlatformIO repo at `programs/agent_program/`. Per-daemon state lives in PDAs derived from `["agent", owner_pubkey]` (root) and `["vault", agent_root]` (vault). `vault_execute` re-CPIs the inner instruction with vault PDA seeds, so the device's day-to-day USDC transfers and Jupiter swaps route through one trust boundary. Recovery authority (the user's Phantom) holds the kill switch via `rotate_signer` and `sweep`.

**Tech Stack:** Rust 1.75+, Anchor 0.30.1, Solana 1.18 / Agave 2.0, TypeScript + Mocha + Chai (Anchor's default test harness), `@coral-xyz/anchor` 0.30.1, `@solana/spl-token` 0.4 for the SPL test cases.

**Spec:** [`docs/superpowers/specs/2026-04-26-on-chain-agent-design.md`](../specs/2026-04-26-on-chain-agent-design.md) (sections "On-chain components → Anchor program" and "Trust model" are the authoritative source).

---

## Scope notes

- **Identity / memory PDAs:** account structs are NOT defined in this phase. They land in Phase 3 / Phase 4. Phase 1's `initialize_agent` only creates `agent_root` + `vault`.
- **Device firmware:** untouched in this phase. Phase 2 picks that up. All Phase 1 testing happens against TypeScript clients on a local validator + devnet.
- **Audit:** out of scope; the program is "test deployed" only. Public mainnet launch happens after Phase 5 + an external audit.

---

## File structure

| File                                        | Status   | Responsibility                                                                 |
|---------------------------------------------|----------|--------------------------------------------------------------------------------|
| `Anchor.toml`                               | Create   | Workspace config (cluster, wallet, scripts, programs)                          |
| `Cargo.toml`                                | Create   | Workspace root for the program crate                                           |
| `package.json`                              | Create   | TypeScript test deps (`@coral-xyz/anchor`, `@solana/spl-token`, mocha, chai)   |
| `tsconfig.json`                             | Create   | TS compiler config for tests                                                   |
| `programs/agent_program/Cargo.toml`         | Create   | Program crate manifest                                                         |
| `programs/agent_program/Xargo.toml`         | Create   | sbf target stub                                                                |
| `programs/agent_program/src/lib.rs`         | Create   | Program entrypoint, instruction handlers                                       |
| `programs/agent_program/src/state.rs`       | Create   | `AgentRoot`, `Vault` account structs                                           |
| `programs/agent_program/src/error.rs`       | Create   | Custom error codes                                                             |
| `tests/agent_program.ts`                    | Create   | All TypeScript tests for the program                                           |
| `tests/helpers.ts`                          | Create   | Shared test helpers (fund, derive PDAs, build inner ixs)                       |
| `scripts/deploy-agent-program.sh`           | Create   | Build + deploy to devnet, write program id back into Anchor.toml               |
| `scripts/devnet-smoke.ts`                   | Create   | End-to-end smoke against devnet                                                |
| `programs/agent_program/README.md`          | Create   | Build / test / deploy instructions                                             |
| `.gitignore`                                | Modify   | Add `target/`, `.anchor/`, `node_modules/`, `test-ledger/`                     |

The Rust program is split into `lib.rs` (instructions), `state.rs` (account layouts), and `error.rs` (typed errors) so each file holds one responsibility. As more instruction families land in later phases (identity, memory), each will get its own `<area>.rs` module imported into `lib.rs`.

---

## Build / test commands (used throughout)

```bash
# build only (validates Rust compiles for the sbf target)
anchor build

# build + spin up local validator + run all tests
anchor test

# run one specific test by name (mocha grep)
anchor test -- --grep "vault_execute happy path"

# devnet deploy (after Task 13 lands the script)
./scripts/deploy-agent-program.sh

# devnet smoke (after Task 14 lands the script)
ts-node scripts/devnet-smoke.ts
```

Most tasks end with `anchor test -- --grep "<task name>"` as the verification gate. The devnet smoke at Task 14 needs network + airdropped devnet SOL.

---

## Task 0: Tooling check

**Files:** none (one-time environment verification).

- [ ] **Step 0.1: Check Rust toolchain**

Run:
```bash
rustc --version
```
Expected: `rustc 1.75.0` or newer. If missing: install via `curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh`.

- [ ] **Step 0.2: Check Solana CLI**

Run:
```bash
solana --version
```
Expected: `solana-cli 1.18.x` or `agave-cli 2.0.x` or newer. If missing: install via `sh -c "$(curl -sSfL https://release.anza.xyz/stable/install)"` then `solana config set --url devnet`.

- [ ] **Step 0.3: Check Anchor**

Run:
```bash
anchor --version
```
Expected: `anchor-cli 0.30.1`. If missing: `cargo install --git https://github.com/coral-xyz/anchor avm --locked && avm install 0.30.1 && avm use 0.30.1`.

- [ ] **Step 0.4: Check Node + ts-node**

Run:
```bash
node --version && npx ts-node --version
```
Expected: Node 20+; `npx ts-node` available (will be installed via `package.json` in Task 2).

- [ ] **Step 0.5: Devnet wallet ready**

Run:
```bash
solana config get
solana balance
```
Expected: `RPC URL: https://api.devnet.solana.com`, balance ≥ 2 SOL. If balance < 2: `solana airdrop 2`.

---

## Task 1: Anchor workspace skeleton

**Files:**
- Create: `Anchor.toml`
- Create: `Cargo.toml`
- Create: `programs/agent_program/Cargo.toml`
- Create: `programs/agent_program/Xargo.toml`
- Create: `programs/agent_program/src/lib.rs`
- Modify: `.gitignore`

- [ ] **Step 1.1: Write `Anchor.toml`**

```toml
[toolchain]
anchor_version = "0.30.1"

[features]
resolution = true
skip-lint = false

[programs.localnet]
agent_program = "AgentProgram1111111111111111111111111111111"

[programs.devnet]
agent_program = "AgentProgram1111111111111111111111111111111"

[registry]
url = "https://api.apr.dev"

[provider]
cluster = "Localnet"
wallet  = "~/.config/solana/id.json"

[scripts]
test = "yarn run ts-mocha -p ./tsconfig.json -t 1000000 tests/**/*.ts"
```

The `AgentProgram1111…` placeholder gets replaced by `anchor build`'s real keypair-derived ID after Task 1.6.

- [ ] **Step 1.2: Write `Cargo.toml` (workspace root)**

```toml
[workspace]
resolver = "2"
members = ["programs/*"]

[profile.release]
overflow-checks = true
lto = "fat"
codegen-units = 1
[profile.release.build-override]
opt-level = 3
incremental = false
codegen-units = 1
```

- [ ] **Step 1.3: Write `programs/agent_program/Cargo.toml`**

```toml
[package]
name = "agent_program"
version = "0.1.0"
description = "On-chain vault for the daemon device"
edition = "2021"

[lib]
crate-type = ["cdylib", "lib"]
name = "agent_program"

[features]
no-entrypoint = []
no-idl = []
no-log-ix-name = []
cpi = ["no-entrypoint"]
default = []
idl-build = ["anchor-lang/idl-build"]

[dependencies]
anchor-lang = { version = "0.30.1", features = ["init-if-needed"] }
anchor-spl  = { version = "0.30.1", features = ["token"] }
```

- [ ] **Step 1.4: Write `programs/agent_program/Xargo.toml`**

```toml
[target.bpfel-unknown-unknown.dependencies.std]
features = []
```

- [ ] **Step 1.5: Write minimal `programs/agent_program/src/lib.rs`**

```rust
use anchor_lang::prelude::*;

declare_id!("AgentProgram1111111111111111111111111111111");

#[program]
pub mod agent_program {
    use super::*;
}
```

- [ ] **Step 1.6: Generate program keypair + run `anchor build`**

Run:
```bash
anchor build
```
Expected output ends with `To deploy this program:` and a path to a `.so` artifact under `target/deploy/agent_program.so`. The build also emits `target/deploy/agent_program-keypair.json` — the program's deploy keypair.

- [ ] **Step 1.7: Sync `declare_id!` with the generated keypair**

Run:
```bash
anchor keys sync
```
Then verify the same pubkey now appears in three places: `declare_id!(...)` in `lib.rs`, `[programs.localnet]` in `Anchor.toml`, and `solana-keygen pubkey target/deploy/agent_program-keypair.json`.

- [ ] **Step 1.8: Append to `.gitignore`**

Append to existing `.gitignore`:

```
# Anchor / Solana
target/
.anchor/
test-ledger/
node_modules/
.yarn/
.idea/
```

(If `.gitignore` does not exist, create it with this content.)

- [ ] **Step 1.9: Commit**

```bash
git add Anchor.toml Cargo.toml programs/agent_program/Cargo.toml programs/agent_program/Xargo.toml programs/agent_program/src/lib.rs .gitignore
git commit -m "agent-program: anchor workspace skeleton"
```

---

## Task 2: TypeScript test harness

**Files:**
- Create: `package.json`
- Create: `tsconfig.json`
- Create: `tests/agent_program.ts`
- Create: `tests/helpers.ts`

- [ ] **Step 2.1: Write `package.json`**

```json
{
  "name": "board-game-onchain",
  "version": "0.1.0",
  "private": true,
  "scripts": {
    "lint:fix": "prettier */*.js \"*/**/*{.js,.ts}\" -w",
    "lint": "prettier */*.js \"*/**/*{.js,.ts}\" --check"
  },
  "dependencies": {
    "@coral-xyz/anchor": "^0.30.1",
    "@solana/spl-token": "^0.4.6",
    "@solana/web3.js": "^1.95.0"
  },
  "devDependencies": {
    "@types/bn.js": "^5.1.0",
    "@types/chai": "^4.3.0",
    "@types/mocha": "^9.0.0",
    "chai": "^4.3.4",
    "mocha": "^9.0.3",
    "prettier": "^2.6.2",
    "ts-mocha": "^10.0.0",
    "typescript": "^4.3.5"
  }
}
```

- [ ] **Step 2.2: Write `tsconfig.json`**

```json
{
  "compilerOptions": {
    "types": ["mocha", "chai"],
    "typeRoots": ["./node_modules/@types"],
    "lib": ["es2015"],
    "module": "commonjs",
    "target": "es6",
    "esModuleInterop": true
  }
}
```

- [ ] **Step 2.3: Install Node deps**

Run:
```bash
yarn install
```
(Or `npm install` — both work; the rest of the plan assumes `yarn` since `Anchor.toml` references it.)

Expected: `node_modules/` populated; no errors.

- [ ] **Step 2.4: Write `tests/helpers.ts`**

```ts
import * as anchor from "@coral-xyz/anchor";
import { PublicKey, Keypair, SystemProgram } from "@solana/web3.js";

export const PROGRAM_ID = new PublicKey(
  // overwritten by anchor.workspace.AgentProgram.programId at runtime;
  // this constant is only used for direct PDA derivation outside tests.
  "AgentProgram1111111111111111111111111111111"
);

export function deriveAgentRoot(owner: PublicKey, programId: PublicKey) {
  return PublicKey.findProgramAddressSync(
    [Buffer.from("agent"), owner.toBuffer()],
    programId
  );
}

export function deriveVault(agentRoot: PublicKey, programId: PublicKey) {
  return PublicKey.findProgramAddressSync(
    [Buffer.from("vault"), agentRoot.toBuffer()],
    programId
  );
}

export async function fundLamports(
  conn: anchor.web3.Connection,
  to: PublicKey,
  lamports: number
) {
  const sig = await conn.requestAirdrop(to, lamports);
  await conn.confirmTransaction(sig, "confirmed");
}
```

- [ ] **Step 2.5: Write `tests/agent_program.ts` skeleton**

```ts
import * as anchor from "@coral-xyz/anchor";
import { Program } from "@coral-xyz/anchor";
import { Keypair, PublicKey, SystemProgram, LAMPORTS_PER_SOL } from "@solana/web3.js";
import { expect } from "chai";
import { AgentProgram } from "../target/types/agent_program";
import { deriveAgentRoot, deriveVault, fundLamports } from "./helpers";

describe("agent_program", () => {
  const provider = anchor.AnchorProvider.env();
  anchor.setProvider(provider);

  const program = anchor.workspace.AgentProgram as Program<AgentProgram>;

  it("workspace loads", () => {
    expect(program.programId.toBase58()).to.have.length.greaterThan(32);
  });
});
```

- [ ] **Step 2.6: Run baseline test**

Run:
```bash
anchor test
```
Expected: 1 passing test (`workspace loads`). The build runs first; tests run on a freshly spun-up local validator.

- [ ] **Step 2.7: Commit**

```bash
git add package.json tsconfig.json tests/agent_program.ts tests/helpers.ts yarn.lock
git commit -m "agent-program: typescript test harness scaffold"
```

---

## Task 3: AgentRoot account + bare initialize_agent

**Files:**
- Create: `programs/agent_program/src/state.rs`
- Modify: `programs/agent_program/src/lib.rs`
- Modify: `tests/agent_program.ts`

- [ ] **Step 3.1: Write the failing test**

Add to `tests/agent_program.ts` inside the `describe` block:

```ts
it("initialize_agent: creates agent_root PDA", async () => {
  const owner = Keypair.generate();
  const device = Keypair.generate();
  await fundLamports(provider.connection, owner.publicKey, 2 * LAMPORTS_PER_SOL);

  const [agentRoot] = deriveAgentRoot(owner.publicKey, program.programId);

  await program.methods
    .initializeAgent(device.publicKey)
    .accounts({
      owner: owner.publicKey,
      agentRoot,
      systemProgram: SystemProgram.programId,
    })
    .signers([owner])
    .rpc();

  const acct = await program.account.agentRoot.fetch(agentRoot);
  expect(acct.owner.toBase58()).to.equal(owner.publicKey.toBase58());
  expect(acct.createdAt.toNumber()).to.be.greaterThan(0);
});
```

- [ ] **Step 3.2: Run the test (expect fail)**

Run:
```bash
anchor test -- --grep "creates agent_root PDA"
```
Expected: build error — `initializeAgent` and `agentRoot` types don't exist yet.

- [ ] **Step 3.3: Write `programs/agent_program/src/state.rs`**

```rust
use anchor_lang::prelude::*;

#[account]
pub struct AgentRoot {
    pub owner: Pubkey,
    pub created_at: i64,
    pub bump: u8,
}

impl AgentRoot {
    pub const LEN: usize = 8 + 32 + 8 + 1;
}
```

- [ ] **Step 3.4: Update `programs/agent_program/src/lib.rs`**

```rust
use anchor_lang::prelude::*;

pub mod state;

use state::*;

declare_id!("AgentProgram1111111111111111111111111111111"); // actual id from anchor keys sync

#[program]
pub mod agent_program {
    use super::*;

    pub fn initialize_agent(ctx: Context<InitializeAgent>, device: Pubkey) -> Result<()> {
        let root = &mut ctx.accounts.agent_root;
        root.owner = ctx.accounts.owner.key();
        root.created_at = Clock::get()?.unix_timestamp;
        root.bump = ctx.bumps.agent_root;
        msg!("agent_root initialized; device={}", device);
        Ok(())
    }
}

#[derive(Accounts)]
pub struct InitializeAgent<'info> {
    #[account(mut)]
    pub owner: Signer<'info>,

    #[account(
        init,
        payer = owner,
        space = AgentRoot::LEN,
        seeds = [b"agent", owner.key().as_ref()],
        bump
    )]
    pub agent_root: Account<'info, AgentRoot>,

    pub system_program: Program<'info, System>,
}
```

(Replace the placeholder `declare_id!` with the actual program ID from `anchor keys list` if not already synced.)

- [ ] **Step 3.5: Run the test (expect pass)**

Run:
```bash
anchor test -- --grep "creates agent_root PDA"
```
Expected: 1 passing.

- [ ] **Step 3.6: Commit**

```bash
git add programs/agent_program/src/state.rs programs/agent_program/src/lib.rs tests/agent_program.ts
git commit -m "agent-program: initialize_agent creates agent_root PDA"
```

---

## Task 4: Vault account + extend initialize_agent

**Files:**
- Modify: `programs/agent_program/src/state.rs`
- Modify: `programs/agent_program/src/lib.rs`
- Modify: `tests/agent_program.ts`

- [ ] **Step 4.1: Write the failing test**

Append to `tests/agent_program.ts`:

```ts
it("initialize_agent: also creates vault with current_signer + recovery_authority", async () => {
  const owner = Keypair.generate();
  const device = Keypair.generate();
  await fundLamports(provider.connection, owner.publicKey, 2 * LAMPORTS_PER_SOL);

  const [agentRoot] = deriveAgentRoot(owner.publicKey, program.programId);
  const [vault] = deriveVault(agentRoot, program.programId);

  await program.methods
    .initializeAgent(device.publicKey)
    .accounts({
      owner: owner.publicKey,
      agentRoot,
      vault,
      systemProgram: SystemProgram.programId,
    })
    .signers([owner])
    .rpc();

  const v = await program.account.vault.fetch(vault);
  expect(v.agentRoot.toBase58()).to.equal(agentRoot.toBase58());
  expect(v.currentSigner.toBase58()).to.equal(device.publicKey.toBase58());
  expect(v.recoveryAuthority.toBase58()).to.equal(owner.publicKey.toBase58());
});
```

- [ ] **Step 4.2: Run the test (expect fail)**

Run:
```bash
anchor test -- --grep "creates vault"
```
Expected: build error — `Vault` not defined.

- [ ] **Step 4.3: Append `Vault` to `state.rs`**

```rust
#[account]
pub struct Vault {
    pub agent_root: Pubkey,
    pub current_signer: Pubkey,
    pub recovery_authority: Pubkey,
    pub signer_rotated_at: i64,
    pub bump: u8,
}

impl Vault {
    pub const LEN: usize = 8 + 32 + 32 + 32 + 8 + 1;
}
```

- [ ] **Step 4.4: Extend `InitializeAgent` context + handler**

Replace `InitializeAgent` and the `initialize_agent` body in `lib.rs`:

```rust
#[derive(Accounts)]
pub struct InitializeAgent<'info> {
    #[account(mut)]
    pub owner: Signer<'info>,

    #[account(
        init,
        payer = owner,
        space = AgentRoot::LEN,
        seeds = [b"agent", owner.key().as_ref()],
        bump
    )]
    pub agent_root: Account<'info, AgentRoot>,

    #[account(
        init,
        payer = owner,
        space = Vault::LEN,
        seeds = [b"vault", agent_root.key().as_ref()],
        bump
    )]
    pub vault: Account<'info, Vault>,

    pub system_program: Program<'info, System>,
}
```

```rust
pub fn initialize_agent(ctx: Context<InitializeAgent>, device: Pubkey) -> Result<()> {
    let root = &mut ctx.accounts.agent_root;
    root.owner = ctx.accounts.owner.key();
    root.created_at = Clock::get()?.unix_timestamp;
    root.bump = ctx.bumps.agent_root;

    let vault = &mut ctx.accounts.vault;
    vault.agent_root = root.key();
    vault.current_signer = device;
    vault.recovery_authority = ctx.accounts.owner.key();
    vault.signer_rotated_at = root.created_at;
    vault.bump = ctx.bumps.vault;

    msg!("agent + vault initialized; device={}", device);
    Ok(())
}
```

- [ ] **Step 4.5: Run all tests**

Run:
```bash
anchor test
```
Expected: 3 passing (`workspace loads`, `creates agent_root PDA`, `creates vault…`).

- [ ] **Step 4.6: Commit**

```bash
git add programs/agent_program/src/state.rs programs/agent_program/src/lib.rs tests/agent_program.ts
git commit -m "agent-program: initialize creates vault with current_signer + recovery"
```

---

## Task 5: vault_execute happy path (System.transfer inner)

**Files:**
- Create: `programs/agent_program/src/error.rs`
- Modify: `programs/agent_program/src/lib.rs`
- Modify: `tests/helpers.ts`
- Modify: `tests/agent_program.ts`

- [ ] **Step 5.1: Write the failing test**

Add a helper to `tests/helpers.ts`:

```ts
import { TransactionInstruction } from "@solana/web3.js";

export type InnerIx = {
  programId: PublicKey;
  accounts: { pubkey: PublicKey; isSigner: boolean; isWritable: boolean }[];
  data: Buffer;
};

export function ixToInner(ix: TransactionInstruction): InnerIx {
  return {
    programId: ix.programId,
    accounts: ix.keys.map((k) => ({
      pubkey: k.pubkey,
      isSigner: k.isSigner,
      isWritable: k.isWritable,
    })),
    data: ix.data,
  };
}
```

Append to `tests/agent_program.ts`:

```ts
it("vault_execute: device signer can transfer SOL out via system program", async () => {
  const owner = Keypair.generate();
  const device = Keypair.generate();
  const dest = Keypair.generate();
  await fundLamports(provider.connection, owner.publicKey, 2 * LAMPORTS_PER_SOL);
  await fundLamports(provider.connection, device.publicKey, 0.05 * LAMPORTS_PER_SOL);

  const [agentRoot] = deriveAgentRoot(owner.publicKey, program.programId);
  const [vault] = deriveVault(agentRoot, program.programId);

  await program.methods
    .initializeAgent(device.publicKey)
    .accounts({ owner: owner.publicKey, agentRoot, vault, systemProgram: SystemProgram.programId })
    .signers([owner])
    .rpc();

  // fund the vault PDA with 0.5 SOL so it can transfer
  await fundLamports(provider.connection, vault, 0.5 * LAMPORTS_PER_SOL);

  // build the inner: SystemProgram.transfer(vault → dest, 0.1 SOL)
  const inner = SystemProgram.transfer({
    fromPubkey: vault,
    toPubkey: dest.publicKey,
    lamports: 0.1 * LAMPORTS_PER_SOL,
  });

  const innerData = {
    programId: inner.programId,
    accounts: inner.keys.map((k) => ({ pubkey: k.pubkey, isSigner: k.isSigner, isWritable: k.isWritable })),
    data: inner.data,
  };

  const before = await provider.connection.getBalance(dest.publicKey);

  await program.methods
    .vaultExecute(innerData as any)
    .accounts({ vault, currentSigner: device.publicKey })
    .remainingAccounts([
      { pubkey: vault, isSigner: false, isWritable: true },
      { pubkey: dest.publicKey, isSigner: false, isWritable: true },
      { pubkey: SystemProgram.programId, isSigner: false, isWritable: false },
    ])
    .signers([device])
    .rpc();

  const after = await provider.connection.getBalance(dest.publicKey);
  expect(after - before).to.equal(0.1 * LAMPORTS_PER_SOL);
});
```

- [ ] **Step 5.2: Run the test (expect fail)**

Run:
```bash
anchor test -- --grep "transfer SOL out"
```
Expected: build error — `vaultExecute` undefined.

- [ ] **Step 5.3: Write `programs/agent_program/src/error.rs`**

```rust
use anchor_lang::prelude::*;

#[error_code]
pub enum AgentError {
    #[msg("signer is not the vault's current_signer")]
    WrongSigner,
    #[msg("signer is not the recovery_authority")]
    NotRecoveryAuthority,
    #[msg("nothing to sweep above rent-min")]
    NothingToSweep,
}
```

- [ ] **Step 5.4: Add the `vault_execute` instruction to `lib.rs`**

At top of `lib.rs`:

```rust
pub mod error;
use error::*;
use anchor_lang::solana_program::instruction::{AccountMeta, Instruction};
use anchor_lang::solana_program::program::invoke_signed;
```

Add to the `#[program]` mod:

```rust
pub fn vault_execute(ctx: Context<VaultExecute>, inner: InnerIx) -> Result<()> {
    let vault = &ctx.accounts.vault;
    require_keys_eq!(
        ctx.accounts.current_signer.key(),
        vault.current_signer,
        AgentError::WrongSigner
    );

    // remaining_accounts must contain all AccountInfos used by the inner ix
    // PLUS the inner ix's program account. We pass it through to invoke_signed
    // unchanged — Solana's runtime matches metas to AccountInfos by pubkey.
    let metas: Vec<AccountMeta> = inner
        .accounts
        .iter()
        .map(|a| {
            if a.is_writable {
                AccountMeta::new(a.pubkey, a.is_signer)
            } else {
                AccountMeta::new_readonly(a.pubkey, a.is_signer)
            }
        })
        .collect();

    let ix = Instruction {
        program_id: inner.program_id,
        accounts: metas,
        data: inner.data,
    };

    let agent_root = vault.agent_root;
    let signer_seeds: &[&[u8]] = &[b"vault", agent_root.as_ref(), &[vault.bump]];

    invoke_signed(&ix, ctx.remaining_accounts, &[signer_seeds])?;
    Ok(())
}
```

Add the context + payload structs:

```rust
#[derive(AnchorSerialize, AnchorDeserialize, Clone)]
pub struct InnerIxAccount {
    pub pubkey: Pubkey,
    pub is_signer: bool,
    pub is_writable: bool,
}

#[derive(AnchorSerialize, AnchorDeserialize, Clone)]
pub struct InnerIx {
    pub program_id: Pubkey,
    pub accounts: Vec<InnerIxAccount>,
    pub data: Vec<u8>,
}

#[derive(Accounts)]
pub struct VaultExecute<'info> {
    #[account(
        mut,
        seeds = [b"vault", vault.agent_root.as_ref()],
        bump = vault.bump,
    )]
    pub vault: Account<'info, Vault>,

    pub current_signer: Signer<'info>,
}
```

- [ ] **Step 5.5: Run the test (expect pass)**

Run:
```bash
anchor test -- --grep "transfer SOL out"
```
Expected: 1 passing.

- [ ] **Step 5.6: Commit**

```bash
git add programs/agent_program/src/error.rs programs/agent_program/src/lib.rs tests/agent_program.ts tests/helpers.ts
git commit -m "agent-program: vault_execute happy path (system transfer)"
```

---

## Task 6: vault_execute auth check (wrong signer fails)

**Files:**
- Modify: `tests/agent_program.ts`

- [ ] **Step 6.1: Write the failing test**

Append:

```ts
it("vault_execute: wrong signer is rejected with WrongSigner", async () => {
  const owner = Keypair.generate();
  const device = Keypair.generate();
  const evil = Keypair.generate();
  await fundLamports(provider.connection, owner.publicKey, 2 * LAMPORTS_PER_SOL);
  await fundLamports(provider.connection, evil.publicKey, 0.05 * LAMPORTS_PER_SOL);

  const [agentRoot] = deriveAgentRoot(owner.publicKey, program.programId);
  const [vault] = deriveVault(agentRoot, program.programId);

  await program.methods
    .initializeAgent(device.publicKey)
    .accounts({ owner: owner.publicKey, agentRoot, vault, systemProgram: SystemProgram.programId })
    .signers([owner])
    .rpc();

  await fundLamports(provider.connection, vault, 0.2 * LAMPORTS_PER_SOL);

  const inner = SystemProgram.transfer({
    fromPubkey: vault,
    toPubkey: evil.publicKey,
    lamports: 0.05 * LAMPORTS_PER_SOL,
  });
  const innerData = {
    programId: inner.programId,
    accounts: inner.keys.map((k) => ({ pubkey: k.pubkey, isSigner: k.isSigner, isWritable: k.isWritable })),
    data: inner.data,
  };

  let threw = false;
  try {
    await program.methods
      .vaultExecute(innerData as any)
      .accounts({ vault, currentSigner: evil.publicKey })
      .remainingAccounts([
        { pubkey: vault, isSigner: false, isWritable: true },
        { pubkey: evil.publicKey, isSigner: false, isWritable: true },
        { pubkey: SystemProgram.programId, isSigner: false, isWritable: false },
      ])
      .signers([evil])
      .rpc();
  } catch (e: any) {
    threw = true;
    expect(e.toString()).to.match(/WrongSigner/);
  }
  expect(threw).to.equal(true);
});
```

- [ ] **Step 6.2: Run the test (expect pass — already enforced in Task 5)**

Run:
```bash
anchor test -- --grep "wrong signer"
```
Expected: 1 passing. (The `require_keys_eq!` was added in Task 5.4; this task is purely a regression test that locks the behavior.)

- [ ] **Step 6.3: Commit**

```bash
git add tests/agent_program.ts
git commit -m "agent-program: regression test for vault_execute auth check"
```

---

## Task 7: vault_execute with USDC TransferChecked CPI

**Files:**
- Modify: `tests/helpers.ts`
- Modify: `tests/agent_program.ts`

- [ ] **Step 7.1: Add SPL helpers**

Append to `tests/helpers.ts`:

```ts
import {
  createMint,
  createAssociatedTokenAccount,
  mintTo,
  TOKEN_PROGRAM_ID,
  getAssociatedTokenAddressSync,
  createTransferCheckedInstruction,
} from "@solana/spl-token";

export async function setupMintAndAtas(
  conn: anchor.web3.Connection,
  payer: Keypair,
  owners: PublicKey[],
  decimals = 6,
  mintAmountToFirst = 1_000_000n
) {
  const mint = await createMint(conn, payer, payer.publicKey, null, decimals);
  const atas: PublicKey[] = [];
  for (const o of owners) {
    const ata = await createAssociatedTokenAccount(conn, payer, mint, o, undefined, undefined, undefined, true);
    atas.push(ata);
  }
  await mintTo(conn, payer, mint, atas[0], payer, Number(mintAmountToFirst));
  return { mint, atas };
}

export {
  TOKEN_PROGRAM_ID,
  getAssociatedTokenAddressSync,
  createTransferCheckedInstruction,
};
```

- [ ] **Step 7.2: Write the failing test**

Append to `tests/agent_program.ts`:

```ts
it("vault_execute: device transfers a token via SPL TransferChecked CPI", async () => {
  const owner = Keypair.generate();
  const device = Keypair.generate();
  const dest = Keypair.generate();
  const payer = (provider.wallet as anchor.Wallet).payer;

  await fundLamports(provider.connection, owner.publicKey, 2 * LAMPORTS_PER_SOL);
  await fundLamports(provider.connection, device.publicKey, 0.05 * LAMPORTS_PER_SOL);

  const [agentRoot] = deriveAgentRoot(owner.publicKey, program.programId);
  const [vault] = deriveVault(agentRoot, program.programId);

  await program.methods
    .initializeAgent(device.publicKey)
    .accounts({ owner: owner.publicKey, agentRoot, vault, systemProgram: SystemProgram.programId })
    .signers([owner])
    .rpc();

  // create a mint and ATAs for vault + dest, mint 1_000_000 to vault
  const { mint, atas } = await setupMintAndAtas(provider.connection, payer, [vault, dest.publicKey]);
  const [vaultAta, destAta] = atas;

  const inner = createTransferCheckedInstruction(vaultAta, mint, destAta, vault, 100_000n, 6);
  const innerData = {
    programId: inner.programId,
    accounts: inner.keys.map((k) => ({ pubkey: k.pubkey, isSigner: k.isSigner, isWritable: k.isWritable })),
    data: inner.data,
  };

  await program.methods
    .vaultExecute(innerData as any)
    .accounts({ vault, currentSigner: device.publicKey })
    .remainingAccounts([
      ...inner.keys.map((k) => ({ pubkey: k.pubkey, isSigner: false, isWritable: k.isWritable })),
      { pubkey: TOKEN_PROGRAM_ID, isSigner: false, isWritable: false },
    ])
    .signers([device])
    .rpc();

  const destAcct = await provider.connection.getTokenAccountBalance(destAta);
  expect(destAcct.value.amount).to.equal("100000");
});
```

Note: the inner instruction's `vault` key is `isSigner=true` from spl-token's perspective, but in `remainingAccounts` we set `isSigner=false` because the CPI re-attaches the signer authority via `invoke_signed` with the vault's seeds. Anchor's account meta builder forwards `is_signer` from the inner payload, so the vault is treated as a signer at the CPI boundary even though the wire-level account isn't a top-level signer.

- [ ] **Step 7.3: Run the test (expect pass)**

Run:
```bash
anchor test -- --grep "TransferChecked"
```
Expected: 1 passing.

- [ ] **Step 7.4: Commit**

```bash
git add tests/agent_program.ts tests/helpers.ts
git commit -m "agent-program: vault_execute drives SPL TransferChecked"
```

---

## Task 8: vault_rotate_signer

**Files:**
- Modify: `programs/agent_program/src/lib.rs`
- Modify: `tests/agent_program.ts`

- [ ] **Step 8.1: Write the failing test**

Append:

```ts
it("vault_rotate_signer: recovery_authority can rotate current_signer", async () => {
  const owner = Keypair.generate();
  const device = Keypair.generate();
  const newDevice = Keypair.generate();
  await fundLamports(provider.connection, owner.publicKey, 2 * LAMPORTS_PER_SOL);

  const [agentRoot] = deriveAgentRoot(owner.publicKey, program.programId);
  const [vault] = deriveVault(agentRoot, program.programId);

  await program.methods
    .initializeAgent(device.publicKey)
    .accounts({ owner: owner.publicKey, agentRoot, vault, systemProgram: SystemProgram.programId })
    .signers([owner])
    .rpc();

  await program.methods
    .vaultRotateSigner(newDevice.publicKey)
    .accounts({ vault, recoveryAuthority: owner.publicKey })
    .signers([owner])
    .rpc();

  const v = await program.account.vault.fetch(vault);
  expect(v.currentSigner.toBase58()).to.equal(newDevice.publicKey.toBase58());
  expect(v.signerRotatedAt.toNumber()).to.be.greaterThan(0);
});
```

- [ ] **Step 8.2: Run the test (expect fail)**

Run:
```bash
anchor test -- --grep "rotate current_signer"
```
Expected: build error — `vaultRotateSigner` undefined.

- [ ] **Step 8.3: Add the instruction to `lib.rs`**

In the `#[program]` mod:

```rust
pub fn vault_rotate_signer(ctx: Context<VaultRotateSigner>, new_signer: Pubkey) -> Result<()> {
    let vault = &mut ctx.accounts.vault;
    require_keys_eq!(
        ctx.accounts.recovery_authority.key(),
        vault.recovery_authority,
        AgentError::NotRecoveryAuthority
    );
    vault.current_signer = new_signer;
    vault.signer_rotated_at = Clock::get()?.unix_timestamp;
    msg!("vault rotated; new_signer={}", new_signer);
    Ok(())
}
```

Add the context after `VaultExecute`:

```rust
#[derive(Accounts)]
pub struct VaultRotateSigner<'info> {
    #[account(
        mut,
        seeds = [b"vault", vault.agent_root.as_ref()],
        bump = vault.bump,
    )]
    pub vault: Account<'info, Vault>,

    pub recovery_authority: Signer<'info>,
}
```

- [ ] **Step 8.4: Run the test (expect pass)**

Run:
```bash
anchor test -- --grep "rotate current_signer"
```
Expected: 1 passing.

- [ ] **Step 8.5: Commit**

```bash
git add programs/agent_program/src/lib.rs tests/agent_program.ts
git commit -m "agent-program: vault_rotate_signer"
```

---

## Task 9: rotate auth check + post-rotate failure

**Files:**
- Modify: `tests/agent_program.ts`

- [ ] **Step 9.1: Write both failing tests**

Append:

```ts
it("vault_rotate_signer: non-recovery_authority is rejected", async () => {
  const owner = Keypair.generate();
  const device = Keypair.generate();
  const evil = Keypair.generate();
  await fundLamports(provider.connection, owner.publicKey, 2 * LAMPORTS_PER_SOL);
  await fundLamports(provider.connection, evil.publicKey, 0.05 * LAMPORTS_PER_SOL);

  const [agentRoot] = deriveAgentRoot(owner.publicKey, program.programId);
  const [vault] = deriveVault(agentRoot, program.programId);

  await program.methods
    .initializeAgent(device.publicKey)
    .accounts({ owner: owner.publicKey, agentRoot, vault, systemProgram: SystemProgram.programId })
    .signers([owner])
    .rpc();

  let threw = false;
  try {
    await program.methods
      .vaultRotateSigner(evil.publicKey)
      .accounts({ vault, recoveryAuthority: evil.publicKey })
      .signers([evil])
      .rpc();
  } catch (e: any) {
    threw = true;
    expect(e.toString()).to.match(/NotRecoveryAuthority/);
  }
  expect(threw).to.equal(true);
});

it("after rotate: old signer's vault_execute is rejected", async () => {
  const owner = Keypair.generate();
  const device = Keypair.generate();
  const newDevice = Keypair.generate();
  const dest = Keypair.generate();
  await fundLamports(provider.connection, owner.publicKey, 2 * LAMPORTS_PER_SOL);
  await fundLamports(provider.connection, device.publicKey, 0.05 * LAMPORTS_PER_SOL);

  const [agentRoot] = deriveAgentRoot(owner.publicKey, program.programId);
  const [vault] = deriveVault(agentRoot, program.programId);

  await program.methods
    .initializeAgent(device.publicKey)
    .accounts({ owner: owner.publicKey, agentRoot, vault, systemProgram: SystemProgram.programId })
    .signers([owner])
    .rpc();
  await fundLamports(provider.connection, vault, 0.2 * LAMPORTS_PER_SOL);

  await program.methods
    .vaultRotateSigner(newDevice.publicKey)
    .accounts({ vault, recoveryAuthority: owner.publicKey })
    .signers([owner])
    .rpc();

  const inner = SystemProgram.transfer({
    fromPubkey: vault,
    toPubkey: dest.publicKey,
    lamports: 0.05 * LAMPORTS_PER_SOL,
  });
  const innerData = {
    programId: inner.programId,
    accounts: inner.keys.map((k) => ({ pubkey: k.pubkey, isSigner: k.isSigner, isWritable: k.isWritable })),
    data: inner.data,
  };

  let threw = false;
  try {
    await program.methods
      .vaultExecute(innerData as any)
      .accounts({ vault, currentSigner: device.publicKey })
      .remainingAccounts([
        { pubkey: vault, isSigner: false, isWritable: true },
        { pubkey: dest.publicKey, isSigner: false, isWritable: true },
        { pubkey: SystemProgram.programId, isSigner: false, isWritable: false },
      ])
      .signers([device])
      .rpc();
  } catch (e: any) {
    threw = true;
    expect(e.toString()).to.match(/WrongSigner/);
  }
  expect(threw).to.equal(true);
});
```

- [ ] **Step 9.2: Run the tests (expect pass — behavior already enforced)**

Run:
```bash
anchor test -- --grep "non-recovery|old signer"
```
Expected: 2 passing.

- [ ] **Step 9.3: Commit**

```bash
git add tests/agent_program.ts
git commit -m "agent-program: rotate auth + post-rotate signer regression tests"
```

---

## Task 10: vault_sweep SOL

**Files:**
- Modify: `programs/agent_program/src/lib.rs`
- Modify: `tests/agent_program.ts`

- [ ] **Step 10.1: Write the failing test**

Append:

```ts
it("vault_sweep: recovery_authority drains SOL minus rent", async () => {
  const owner = Keypair.generate();
  const device = Keypair.generate();
  const dest = Keypair.generate();
  await fundLamports(provider.connection, owner.publicKey, 2 * LAMPORTS_PER_SOL);

  const [agentRoot] = deriveAgentRoot(owner.publicKey, program.programId);
  const [vault] = deriveVault(agentRoot, program.programId);

  await program.methods
    .initializeAgent(device.publicKey)
    .accounts({ owner: owner.publicKey, agentRoot, vault, systemProgram: SystemProgram.programId })
    .signers([owner])
    .rpc();

  await fundLamports(provider.connection, vault, 1.0 * LAMPORTS_PER_SOL);

  const beforeDest = await provider.connection.getBalance(dest.publicKey);
  await program.methods
    .vaultSweep()
    .accounts({ vault, destination: dest.publicKey, recoveryAuthority: owner.publicKey })
    .signers([owner])
    .rpc();
  const afterDest = await provider.connection.getBalance(dest.publicKey);

  // sweep moves all lamports above the rent-exempt minimum for Vault account
  const rentMin = await provider.connection.getMinimumBalanceForRentExemption(120 /* > Vault::LEN */);
  const moved = afterDest - beforeDest;
  expect(moved).to.be.greaterThan(0.9 * LAMPORTS_PER_SOL);
  // vault balance should be near rent-min, not zero (account stays alive)
  const vaultBal = await provider.connection.getBalance(vault);
  expect(vaultBal).to.be.greaterThanOrEqual(rentMin);
});
```

- [ ] **Step 10.2: Run the test (expect fail)**

Run:
```bash
anchor test -- --grep "drains SOL"
```
Expected: build error — `vaultSweep` undefined.

- [ ] **Step 10.3: Add `vault_sweep` to `lib.rs`**

In the `#[program]` mod:

```rust
pub fn vault_sweep(ctx: Context<VaultSweep>) -> Result<()> {
    let vault_info = ctx.accounts.vault.to_account_info();
    let dest_info = ctx.accounts.destination.to_account_info();

    require_keys_eq!(
        ctx.accounts.recovery_authority.key(),
        ctx.accounts.vault.recovery_authority,
        AgentError::NotRecoveryAuthority
    );

    let rent_min = Rent::get()?.minimum_balance(vault_info.data_len());
    let movable = vault_info.lamports().saturating_sub(rent_min);
    require!(movable > 0, AgentError::NothingToSweep);

    **vault_info.try_borrow_mut_lamports()? -= movable;
    **dest_info.try_borrow_mut_lamports()? += movable;

    msg!("swept {} lamports → {}", movable, ctx.accounts.destination.key());
    Ok(())
}
```

Add the context:

```rust
#[derive(Accounts)]
pub struct VaultSweep<'info> {
    #[account(
        mut,
        seeds = [b"vault", vault.agent_root.as_ref()],
        bump = vault.bump,
    )]
    pub vault: Account<'info, Vault>,

    /// CHECK: arbitrary destination
    #[account(mut)]
    pub destination: AccountInfo<'info>,

    pub recovery_authority: Signer<'info>,
}
```

- [ ] **Step 10.4: Run the test (expect pass)**

Run:
```bash
anchor test -- --grep "drains SOL"
```
Expected: 1 passing.

- [ ] **Step 10.5: Commit**

```bash
git add programs/agent_program/src/lib.rs tests/agent_program.ts
git commit -m "agent-program: vault_sweep moves SOL above rent-min"
```

---

## Task 11: vault_sweep_token (extension)

**Files:**
- Modify: `programs/agent_program/src/lib.rs`
- Modify: `tests/agent_program.ts`

- [ ] **Step 11.1: Write the failing test**

Append:

```ts
it("vault_sweep_token: drains an SPL ATA owned by vault", async () => {
  const owner = Keypair.generate();
  const device = Keypair.generate();
  const dest = Keypair.generate();
  const payer = (provider.wallet as anchor.Wallet).payer;

  await fundLamports(provider.connection, owner.publicKey, 2 * LAMPORTS_PER_SOL);

  const [agentRoot] = deriveAgentRoot(owner.publicKey, program.programId);
  const [vault] = deriveVault(agentRoot, program.programId);

  await program.methods
    .initializeAgent(device.publicKey)
    .accounts({ owner: owner.publicKey, agentRoot, vault, systemProgram: SystemProgram.programId })
    .signers([owner])
    .rpc();

  const { mint, atas } = await setupMintAndAtas(provider.connection, payer, [vault, dest.publicKey]);
  const [vaultAta, destAta] = atas;

  await program.methods
    .vaultSweepToken()
    .accounts({
      vault,
      vaultAta,
      destinationAta: destAta,
      mint,
      tokenProgram: TOKEN_PROGRAM_ID,
      recoveryAuthority: owner.publicKey,
    })
    .signers([owner])
    .rpc();

  const balance = await provider.connection.getTokenAccountBalance(destAta);
  expect(balance.value.amount).to.equal("1000000");
});
```

- [ ] **Step 11.2: Run the test (expect fail)**

Run:
```bash
anchor test -- --grep "vault_sweep_token"
```
Expected: build error — `vaultSweepToken` undefined.

- [ ] **Step 11.3: Add `vault_sweep_token` to `lib.rs`**

At the top, add the import:

```rust
use anchor_spl::token::{self, Token, TokenAccount, Mint, TransferChecked};
```

In the `#[program]` mod:

```rust
pub fn vault_sweep_token(ctx: Context<VaultSweepToken>) -> Result<()> {
    require_keys_eq!(
        ctx.accounts.recovery_authority.key(),
        ctx.accounts.vault.recovery_authority,
        AgentError::NotRecoveryAuthority
    );

    let amount = ctx.accounts.vault_ata.amount;
    let decimals = ctx.accounts.mint.decimals;

    let agent_root = ctx.accounts.vault.agent_root;
    let bump = ctx.accounts.vault.bump;
    let signer_seeds: &[&[&[u8]]] = &[&[b"vault", agent_root.as_ref(), &[bump]]];

    let cpi_accounts = TransferChecked {
        from: ctx.accounts.vault_ata.to_account_info(),
        mint: ctx.accounts.mint.to_account_info(),
        to: ctx.accounts.destination_ata.to_account_info(),
        authority: ctx.accounts.vault.to_account_info(),
    };
    let cpi_ctx = CpiContext::new_with_signer(
        ctx.accounts.token_program.to_account_info(),
        cpi_accounts,
        signer_seeds,
    );
    token::transfer_checked(cpi_ctx, amount, decimals)?;
    Ok(())
}
```

Add the context:

```rust
#[derive(Accounts)]
pub struct VaultSweepToken<'info> {
    #[account(
        seeds = [b"vault", vault.agent_root.as_ref()],
        bump = vault.bump,
    )]
    pub vault: Account<'info, Vault>,

    #[account(mut, token::authority = vault, token::mint = mint)]
    pub vault_ata: Account<'info, TokenAccount>,

    #[account(mut, token::mint = mint)]
    pub destination_ata: Account<'info, TokenAccount>,

    pub mint: Account<'info, Mint>,

    pub token_program: Program<'info, Token>,
    pub recovery_authority: Signer<'info>,
}
```

- [ ] **Step 11.4: Run the test (expect pass)**

Run:
```bash
anchor test -- --grep "vault_sweep_token"
```
Expected: 1 passing.

- [ ] **Step 11.5: Commit**

```bash
git add programs/agent_program/src/lib.rs tests/agent_program.ts
git commit -m "agent-program: vault_sweep_token drains SPL ATA"
```

---

## Task 12: sweep auth check (regression)

**Files:**
- Modify: `tests/agent_program.ts`

- [ ] **Step 12.1: Write the failing test**

Append:

```ts
it("vault_sweep: non-recovery_authority is rejected", async () => {
  const owner = Keypair.generate();
  const device = Keypair.generate();
  const dest = Keypair.generate();
  const evil = Keypair.generate();
  await fundLamports(provider.connection, owner.publicKey, 2 * LAMPORTS_PER_SOL);
  await fundLamports(provider.connection, evil.publicKey, 0.05 * LAMPORTS_PER_SOL);

  const [agentRoot] = deriveAgentRoot(owner.publicKey, program.programId);
  const [vault] = deriveVault(agentRoot, program.programId);

  await program.methods
    .initializeAgent(device.publicKey)
    .accounts({ owner: owner.publicKey, agentRoot, vault, systemProgram: SystemProgram.programId })
    .signers([owner])
    .rpc();
  await fundLamports(provider.connection, vault, 0.5 * LAMPORTS_PER_SOL);

  let threw = false;
  try {
    await program.methods
      .vaultSweep()
      .accounts({ vault, destination: dest.publicKey, recoveryAuthority: evil.publicKey })
      .signers([evil])
      .rpc();
  } catch (e: any) {
    threw = true;
    expect(e.toString()).to.match(/NotRecoveryAuthority/);
  }
  expect(threw).to.equal(true);
});
```

- [ ] **Step 12.2: Run the test (expect pass)**

Run:
```bash
anchor test -- --grep "non-recovery_authority is rejected"
```
Expected: 1 passing (behavior already enforced in Task 10.3).

- [ ] **Step 12.3: Commit**

```bash
git add tests/agent_program.ts
git commit -m "agent-program: regression test for sweep auth check"
```

---

## Task 13: Devnet deploy script

**Files:**
- Create: `scripts/deploy-agent-program.sh`
- Modify: `Anchor.toml` (will be edited by the script)

- [ ] **Step 13.1: Write `scripts/deploy-agent-program.sh`**

```bash
#!/usr/bin/env bash
# Deploy agent_program to Solana devnet and update Anchor.toml's program id.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

echo "==> anchor build"
anchor build

PROG_ID=$(solana-keygen pubkey target/deploy/agent_program-keypair.json)
echo "==> program id: $PROG_ID"

echo "==> anchor deploy --provider.cluster devnet"
anchor deploy --provider.cluster devnet

# replace the [programs.devnet] line with the real id (idempotent)
sed -i.bak -E "s|^agent_program = \".*\"|agent_program = \"$PROG_ID\"|" Anchor.toml
rm -f Anchor.toml.bak

echo "==> deployed; verifying with solana program show"
solana program show "$PROG_ID" --url devnet
```

- [ ] **Step 13.2: Make executable**

Run:
```bash
chmod +x scripts/deploy-agent-program.sh
```

- [ ] **Step 13.3: Run a dry deploy on devnet**

Pre-req: `solana config set --url devnet` and `solana balance` ≥ 3 SOL (deploy costs ~2 SOL; airdrop more if needed).

Run:
```bash
./scripts/deploy-agent-program.sh
```
Expected: ends with `solana program show` output that lists the program id, owner = BPFLoaderUpgradeab1e..., authority = your wallet, data length > 0.

- [ ] **Step 13.4: Commit**

```bash
git add scripts/deploy-agent-program.sh Anchor.toml
git commit -m "agent-program: devnet deploy script"
```

---

## Task 14: Devnet end-to-end smoke

**Files:**
- Create: `scripts/devnet-smoke.ts`

- [ ] **Step 14.1: Write `scripts/devnet-smoke.ts`**

```ts
/**
 * End-to-end smoke against devnet:
 *   1. initialize_agent
 *   2. fund vault (0.05 SOL)
 *   3. vault_execute → SystemProgram.transfer to a fresh dest
 *   4. vault_rotate_signer → new device key
 *   5. attempt vault_execute with the OLD device key → expect WrongSigner
 *   6. vault_sweep → drain back to the owner
 *
 * Run: ts-node scripts/devnet-smoke.ts
 */
import * as anchor from "@coral-xyz/anchor";
import { Program } from "@coral-xyz/anchor";
import { Connection, Keypair, PublicKey, SystemProgram, LAMPORTS_PER_SOL, clusterApiUrl } from "@solana/web3.js";
import { AgentProgram } from "../target/types/agent_program";
import * as fs from "fs";
import * as os from "os";

async function main() {
  const url = clusterApiUrl("devnet");
  const conn = new Connection(url, "confirmed");

  const keypath = `${os.homedir()}/.config/solana/id.json`;
  const owner = Keypair.fromSecretKey(Uint8Array.from(JSON.parse(fs.readFileSync(keypath, "utf-8"))));
  const wallet = new anchor.Wallet(owner);
  const provider = new anchor.AnchorProvider(conn, wallet, { commitment: "confirmed" });
  anchor.setProvider(provider);

  const program = anchor.workspace.AgentProgram as Program<AgentProgram>;

  const device = Keypair.generate();
  const newDevice = Keypair.generate();
  const dest = Keypair.generate();

  const [agentRoot] = PublicKey.findProgramAddressSync(
    [Buffer.from("agent"), owner.publicKey.toBuffer()],
    program.programId
  );
  const [vault] = PublicKey.findProgramAddressSync(
    [Buffer.from("vault"), agentRoot.toBuffer()],
    program.programId
  );

  console.log("agent_root", agentRoot.toBase58());
  console.log("vault     ", vault.toBase58());

  // 1. init
  console.log("[1] initialize_agent");
  const sig1 = await program.methods
    .initializeAgent(device.publicKey)
    .accounts({ owner: owner.publicKey, agentRoot, vault, systemProgram: SystemProgram.programId })
    .rpc();
  console.log("    sig:", sig1);

  // 2. fund the vault and the device for fees
  console.log("[2] funding vault + device");
  const fundIxs = [
    SystemProgram.transfer({ fromPubkey: owner.publicKey, toPubkey: vault, lamports: 0.05 * LAMPORTS_PER_SOL }),
    SystemProgram.transfer({ fromPubkey: owner.publicKey, toPubkey: device.publicKey, lamports: 0.02 * LAMPORTS_PER_SOL }),
  ];
  const tx = new anchor.web3.Transaction().add(...fundIxs);
  await provider.sendAndConfirm(tx);

  // 3. vault_execute — transfer 0.01 SOL to dest
  console.log("[3] vault_execute → System.transfer");
  const inner = SystemProgram.transfer({
    fromPubkey: vault,
    toPubkey: dest.publicKey,
    lamports: 0.01 * LAMPORTS_PER_SOL,
  });
  const innerData = {
    programId: inner.programId,
    accounts: inner.keys.map((k) => ({ pubkey: k.pubkey, isSigner: k.isSigner, isWritable: k.isWritable })),
    data: inner.data,
  };
  const sig3 = await program.methods
    .vaultExecute(innerData as any)
    .accounts({ vault, currentSigner: device.publicKey })
    .remainingAccounts([
      { pubkey: vault, isSigner: false, isWritable: true },
      { pubkey: dest.publicKey, isSigner: false, isWritable: true },
      { pubkey: SystemProgram.programId, isSigner: false, isWritable: false },
    ])
    .signers([device])
    .rpc();
  console.log("    sig:", sig3);

  // 4. rotate
  console.log("[4] vault_rotate_signer →", newDevice.publicKey.toBase58());
  const sig4 = await program.methods
    .vaultRotateSigner(newDevice.publicKey)
    .accounts({ vault, recoveryAuthority: owner.publicKey })
    .rpc();
  console.log("    sig:", sig4);

  // 5. attempt with old device — expect failure
  console.log("[5] expecting WrongSigner from old device key");
  let threw = false;
  try {
    await program.methods
      .vaultExecute(innerData as any)
      .accounts({ vault, currentSigner: device.publicKey })
      .remainingAccounts([
        { pubkey: vault, isSigner: false, isWritable: true },
        { pubkey: dest.publicKey, isSigner: false, isWritable: true },
        { pubkey: SystemProgram.programId, isSigner: false, isWritable: false },
      ])
      .signers([device])
      .rpc();
  } catch (e: any) {
    threw = true;
    console.log("    rejected as expected:", String(e).split("\n")[0]);
  }
  if (!threw) throw new Error("expected old-signer rejection but tx succeeded");

  // 6. sweep
  console.log("[6] vault_sweep → owner");
  const sig6 = await program.methods
    .vaultSweep()
    .accounts({ vault, destination: owner.publicKey, recoveryAuthority: owner.publicKey })
    .rpc();
  console.log("    sig:", sig6);

  console.log("\nALL GOOD ✓");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
```

- [ ] **Step 14.2: Run the smoke**

Pre-req: Task 13 ran successfully (program is on devnet). Owner wallet has ≥ 0.5 SOL on devnet.

Run:
```bash
yarn ts-node scripts/devnet-smoke.ts
```
Expected: prints `ALL GOOD ✓`. Each step prints a tx signature you can verify on `solscan.io/?cluster=devnet`.

- [ ] **Step 14.3: Commit**

```bash
git add scripts/devnet-smoke.ts
git commit -m "agent-program: devnet end-to-end smoke script"
```

---

## Task 15: Program README

**Files:**
- Create: `programs/agent_program/README.md`

- [ ] **Step 15.1: Write `programs/agent_program/README.md`**

```markdown
# agent_program

On-chain vault for the Daemon device. Phase 1 of the on-chain agent design (see
[`docs/superpowers/specs/2026-04-26-on-chain-agent-design.md`](../../docs/superpowers/specs/2026-04-26-on-chain-agent-design.md)).

## Instructions

| Instruction              | Signer                | Purpose                                                  |
|--------------------------|-----------------------|----------------------------------------------------------|
| `initialize_agent`       | owner (recovery auth) | Create `agent_root` + `vault` PDAs                       |
| `vault_execute`          | current_signer        | CPI an arbitrary inner instruction with vault PDA seeds  |
| `vault_rotate_signer`    | recovery_authority    | Replace `current_signer` (e.g. after device theft)       |
| `vault_sweep`            | recovery_authority    | Drain SOL from vault to a destination (above rent-min)   |
| `vault_sweep_token`      | recovery_authority    | Drain a single SPL ATA owned by the vault                |

## PDAs

```
agent_root = ["agent", owner_pubkey]
vault      = ["vault", agent_root]
```

## Build / test

```bash
anchor build
anchor test
```

`anchor test` spins up a local validator, runs the TypeScript suite under
`tests/`, and tears down. Targeted runs:

```bash
anchor test -- --grep "vault_execute"
```

## Deploy (devnet)

```bash
solana config set --url devnet
solana balance        # need ≥ 3 SOL
./scripts/deploy-agent-program.sh
```

The script writes the deployed program id back into `Anchor.toml` under
`[programs.devnet]`.

## End-to-end smoke (devnet)

```bash
yarn ts-node scripts/devnet-smoke.ts
```

Initializes a fresh agent, executes a vault transfer, rotates the signer,
verifies the old key is rejected, and sweeps back to the owner.

## Out of scope (later phases)

- `identity` PDA (Phase 3)
- `memory` PDA (Phase 4)
- Recovery dApp (Phase 5)
- Audit (parallel, before mainnet)
```

- [ ] **Step 15.2: Commit**

```bash
git add programs/agent_program/README.md
git commit -m "agent-program: README with build/test/deploy instructions"
```

---

## Final verification

After all 15 tasks, run the full suite once more from a clean state:

```bash
anchor clean
anchor test
```

Expected: all tests pass (workspace-loads, init agent_root, init vault, vault_execute SOL, vault_execute auth, vault_execute SPL, rotate happy path, rotate auth, post-rotate signer rejection, sweep SOL, sweep_token, sweep auth) — **12 passing**, no warnings beyond the standard Anchor `cfg` notes.

Then re-run the devnet smoke to confirm the deployed program still matches:

```bash
yarn ts-node scripts/devnet-smoke.ts
```

Expected: `ALL GOOD ✓`.

At this point Phase 1 is complete:
- Program deployed to devnet with a stable id.
- Vault primitives behave correctly under unit + integration tests.
- A reproducible smoke script exists for regression checks before each phase.
- README + deploy script make handoff to subsequent phases self-serve.

Phase 2 (hardware-protected device signer + minimal wizard) gets its own plan once Phase 1 is reviewed and merged.
