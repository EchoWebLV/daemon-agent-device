/**
 * One-time test-vault setup, driven by the user's CLI wallet (recovery_authority).
 * Argv: <device-pubkey-base58>
 *
 *   yarn ts-node scripts/setup-test-vault.ts 8AS42hPuWjnJc3ATso8wz59yuTrwtj2jUHE33pqCKhZm
 *
 * Idempotent — handles three cases:
 *   - vault doesn't exist:         calls initialize_agent
 *   - vault exists, signer wrong:  calls vault_rotate_signer
 *   - vault exists, signer right:  no-op for the program
 *
 * Always: tops up SOL on vault + device, mints a fresh test "USDC" SPL,
 * creates the vault's ATA, mints test tokens, persists everything to
 * target/test-vault.json for the firmware to load.
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
} from "@solana/spl-token";
import { AgentProgram } from "../target/types/agent_program";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";

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

  // 1. init OR rotate
  const existing = await conn.getAccountInfo(vault);
  if (!existing) {
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
  } else {
    const v = await program.account.vault.fetch(vault);
    if (!v.currentSigner.equals(devicePk)) {
      console.log("[1] rotate_signer →", devicePk.toBase58());
      const sig = await program.methods
        .vaultRotateSigner(devicePk)
        .accountsPartial({ vault, recoveryAuthority: owner.publicKey })
        .rpc();
      console.log("    sig:", sig);
    } else {
      console.log("[1] vault already pointed at device — skipping rotate");
    }
  }

  // 2. fund vault + device key
  console.log("[2] funding vault + device");
  const ixs: anchor.web3.TransactionInstruction[] = [];
  const vaultBal = await conn.getBalance(vault);
  if (vaultBal < 0.05 * LAMPORTS_PER_SOL) {
    ixs.push(
      SystemProgram.transfer({
        fromPubkey: owner.publicKey,
        toPubkey: vault,
        lamports: 0.05 * LAMPORTS_PER_SOL - vaultBal,
      })
    );
  }
  const deviceBal = await conn.getBalance(devicePk);
  if (deviceBal < 0.02 * LAMPORTS_PER_SOL) {
    ixs.push(
      SystemProgram.transfer({
        fromPubkey: owner.publicKey,
        toPubkey: devicePk,
        lamports: 0.02 * LAMPORTS_PER_SOL - deviceBal,
      })
    );
  }
  if (ixs.length) {
    const sig = await provider.sendAndConfirm(new Transaction().add(...ixs));
    console.log("    sig:", sig);
  } else {
    console.log("    already funded");
  }

  // 3. mint test "USDC" + vault ATA + owner ATA (destination for tests)
  console.log("[3] minting test USDC + vault & owner ATAs");
  const mint = await createMint(conn, owner, owner.publicKey, null, 6);
  const vaultAta = await getOrCreateAssociatedTokenAccount(
    conn,
    owner,
    mint,
    vault,
    true
  );
  const ownerAta = await getOrCreateAssociatedTokenAccount(
    conn,
    owner,
    mint,
    owner.publicKey
  );
  await mintTo(conn, owner, mint, vaultAta.address, owner, 1_000_000);
  console.log("    mint:     ", mint.toBase58());
  console.log("    vault_ata:", vaultAta.address.toBase58());
  console.log("    owner_ata:", ownerAta.address.toBase58());

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
        owner_usdc_ata: ownerAta.address.toBase58(),
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
