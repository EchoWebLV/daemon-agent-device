# agent_program

On-chain vault for the Daemon device. Phase 1 of the on-chain agent design (see
[`../../docs/superpowers/specs/2026-04-26-on-chain-agent-design.md`](../../docs/superpowers/specs/2026-04-26-on-chain-agent-design.md)).

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
./scripts/anchor-build.sh
./scripts/anchor-test.sh
```

The wrappers handle two host-environment quirks: anchor 0.32.1's combined build
forwards `--tools-version` to the IDL stage where it isn't recognized, and
`solana-test-validator` 2.3.x's startup detection trips anchor's built-in
poller. See the script headers for the workaround details.

Targeted runs:

```bash
./scripts/anchor-test.sh -- --grep "vault_execute"
```

## Deploy (devnet)

```bash
solana config set --url devnet
solana balance        # need ≥ 3 SOL — airdrop a few times if low
./scripts/deploy-agent-program.sh
```

The script writes the deployed program id back into `Anchor.toml` under
`[programs.devnet]`.

## End-to-end smoke (devnet)

```bash
yarn ts-node scripts/devnet-smoke.ts
```

Initializes a fresh agent, drives a vault-signed SPL transfer, rotates the
signer, verifies the old key is rejected, and sweeps everything back to the
owner. Run after the deploy script lands the program on devnet.

## Key design points

- **`vault_execute` is the workhorse** — the device key signs a vault_execute
  call wrapping any inner instruction (USDC transfer, Jupiter swap), and the
  program re-signs the inner ix as the vault PDA via `invoke_signed` with
  `["vault", agent_root]` seeds. The inner ix can target any program; the only
  constraint is that whoever needs to sign as the vault PDA must accept it as
  the signer (i.e. the inner ix's account list marks the vault as a signer,
  even though it's a PDA — Solana's runtime resolves this through the
  invoke_signed path).
- **`vault_sweep` is the SOL panic button** — direct lamport accounting,
  bypasses System.transfer (which won't accept a data-bearing PDA as source).
  Leaves vault account at exactly its rent-exempt minimum so the PDA stays
  alive for future operations.
- **`vault_sweep_token` is the SPL panic button** — vault PDA signs
  TransferChecked as authority via CPI seeds, drains the full ATA balance.

## Out of scope (later phases)

- `identity` PDA: name, prompt, voice, services (Phase 3)
- `memory` PDA: chat-turn ring buffer + summary + Shadow Drive log head (Phase 4)
- Recovery dApp for human-driven rotate/sweep (Phase 5)
- External audit (parallel, before mainnet)
- Stripped-down build profile for cheap mainnet deploy (`--no-idl` already
  separates the stripped path; mainnet deploy will use the stripped binary)
