/**
 * End-to-end smoke against devnet:
 *   1. initialize_agent (creates agent_root + vault PDAs)
 *   2. fund vault + device key
 *   3. mint a fresh SPL test token, give the vault an ATA, mint 100 to it
 *   4. vault_execute → SPL TransferChecked from vault ATA to dest ATA
 *   5. vault_rotate_signer → new device key
 *   6. attempt vault_execute with the OLD device → expect WrongSigner
 *   7. vault_sweep_token → drain remaining tokens to owner
 *   8. vault_sweep → drain SOL above rent-min to owner
 *
 * Run AFTER scripts/deploy-agent-program.sh has put the program on devnet.
 *
 *   yarn ts-node scripts/devnet-smoke.ts
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
  createAssociatedTokenAccount,
  mintTo,
  TOKEN_PROGRAM_ID,
  createTransferCheckedInstruction,
} from "@solana/spl-token";
import { AgentProgram } from "../target/types/agent_program";
import * as fs from "fs";
import * as os from "os";

async function main() {
  const url = clusterApiUrl("devnet");
  const conn = new Connection(url, "confirmed");

  const keypath = `${os.homedir()}/.config/solana/id.json`;
  const owner = Keypair.fromSecretKey(
    Uint8Array.from(JSON.parse(fs.readFileSync(keypath, "utf-8")))
  );
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

  console.log("[1] initialize_agent");
  const sig1 = await program.methods
    .initializeAgent(device.publicKey)
    .accounts({
      owner: owner.publicKey,
      agentRoot,
      vault,
      systemProgram: SystemProgram.programId,
    })
    .rpc();
  console.log("    sig:", sig1);

  console.log("[2] funding vault + device");
  const fundIxs = [
    SystemProgram.transfer({
      fromPubkey: owner.publicKey,
      toPubkey: vault,
      lamports: 0.05 * LAMPORTS_PER_SOL,
    }),
    SystemProgram.transfer({
      fromPubkey: owner.publicKey,
      toPubkey: device.publicKey,
      lamports: 0.02 * LAMPORTS_PER_SOL,
    }),
  ];
  await provider.sendAndConfirm(new Transaction().add(...fundIxs));

  console.log("[3] minting test SPL token + ATAs");
  const mint = await createMint(conn, owner, owner.publicKey, null, 6);
  const vaultAta = await createAssociatedTokenAccount(
    conn,
    owner,
    mint,
    vault,
    undefined,
    undefined,
    undefined,
    true
  );
  const destAta = await createAssociatedTokenAccount(conn, owner, mint, dest.publicKey);
  const ownerAta = await createAssociatedTokenAccount(conn, owner, mint, owner.publicKey);
  await mintTo(conn, owner, mint, vaultAta, owner, 100_000_000);
  console.log("    mint:", mint.toBase58(), "vault_ata:", vaultAta.toBase58());

  console.log("[4] vault_execute → SPL TransferChecked (10 token units)");
  const inner = createTransferCheckedInstruction(vaultAta, mint, destAta, vault, 10_000_000n, 6);
  const innerData = {
    programId: inner.programId,
    accounts: inner.keys.map((k) => ({
      pubkey: k.pubkey,
      isSigner: k.isSigner,
      isWritable: k.isWritable,
    })),
    data: inner.data,
  };
  const sig4 = await program.methods
    .vaultExecute(innerData as any)
    .accounts({ vault, currentSigner: device.publicKey })
    .remainingAccounts([
      ...inner.keys.map((k) => ({ pubkey: k.pubkey, isSigner: false, isWritable: k.isWritable })),
      { pubkey: TOKEN_PROGRAM_ID, isSigner: false, isWritable: false },
    ])
    .signers([device])
    .rpc();
  console.log("    sig:", sig4);

  console.log("[5] vault_rotate_signer →", newDevice.publicKey.toBase58());
  const sig5 = await program.methods
    .vaultRotateSigner(newDevice.publicKey)
    .accounts({ vault, recoveryAuthority: owner.publicKey })
    .rpc();
  console.log("    sig:", sig5);

  console.log("[6] expecting WrongSigner from old device key");
  let threw = false;
  try {
    await program.methods
      .vaultExecute(innerData as any)
      .accounts({ vault, currentSigner: device.publicKey })
      .remainingAccounts([
        ...inner.keys.map((k) => ({ pubkey: k.pubkey, isSigner: false, isWritable: k.isWritable })),
        { pubkey: TOKEN_PROGRAM_ID, isSigner: false, isWritable: false },
      ])
      .signers([device])
      .rpc();
  } catch (e: any) {
    threw = true;
    console.log("    rejected as expected:", String(e).split("\n")[0]);
  }
  if (!threw) throw new Error("expected old-signer rejection but tx succeeded");

  console.log("[7] vault_sweep_token → owner");
  const sig7 = await program.methods
    .vaultSweepToken()
    .accounts({
      vault,
      vaultAta,
      destinationAta: ownerAta,
      mint,
      tokenProgram: TOKEN_PROGRAM_ID,
      recoveryAuthority: owner.publicKey,
    })
    .rpc();
  console.log("    sig:", sig7);

  console.log("[8] vault_sweep → owner");
  const sig8 = await program.methods
    .vaultSweep()
    .accounts({ vault, destination: owner.publicKey, recoveryAuthority: owner.publicKey })
    .rpc();
  console.log("    sig:", sig8);

  console.log("\nALL GOOD ✓");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
