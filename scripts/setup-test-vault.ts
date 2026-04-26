/**
 * One-time vault setup, driven by the recovery_authority keypair.
 * Argv: <cluster> <device-pubkey-base58>
 *
 *   yarn ts-node scripts/setup-test-vault.ts devnet  8AS42hPuWj...
 *   yarn ts-node scripts/setup-test-vault.ts mainnet 8AS42hPuWj...
 *
 * On devnet: owner = ~/.config/solana/id.json (CLI wallet), uses a fresh
 * minted test SPL.
 *
 * On mainnet: owner = .secrets/agent-deployer.json (the program's upgrade
 * authority), uses real USDC EPjFWdd5..., funds the vault with $1 of real
 * USDC out of the deployer wallet's existing balance.
 *
 * Idempotent — handles three cases:
 *   - vault doesn't exist:         calls initialize_agent
 *   - vault exists, signer wrong:  calls vault_rotate_signer
 *   - vault exists, signer right:  no-op for the program
 *
 * Always: tops up SOL on vault + device, ensures the vault's USDC ATA
 * exists, transfers a small amount of USDC into the vault, persists
 * everything to target/test-vault-<cluster>.json for the firmware.
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
  createTransferCheckedInstruction,
  TOKEN_PROGRAM_ID,
} from "@solana/spl-token";
import { AgentProgram } from "../target/types/agent_program";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";

const REAL_USDC_MAINNET = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";

// $1 of USDC = 1_000_000 micro-units. Initial test funding for the vault.
const TEST_VAULT_USDC_FUND = 1_000_000n;

async function main() {
  const argv = process.argv.slice(2);
  if (argv.length !== 2 || !["devnet", "mainnet"].includes(argv[0])) {
    console.error("usage: ts-node scripts/setup-test-vault.ts {devnet|mainnet} <DEVICE_PUBKEY>");
    process.exit(1);
  }
  const cluster = argv[0] as "devnet" | "mainnet";
  const devicePk = new PublicKey(argv[1]);

  const conn = new Connection(
    cluster === "devnet" ? clusterApiUrl("devnet") : clusterApiUrl("mainnet-beta"),
    "confirmed"
  );

  const ownerKeyPath =
    cluster === "devnet"
      ? `${os.homedir()}/.config/solana/id.json`
      : path.resolve(".secrets/agent-deployer.json");
  const owner = Keypair.fromSecretKey(
    Uint8Array.from(JSON.parse(fs.readFileSync(ownerKeyPath, "utf-8")))
  );
  console.log("cluster     ", cluster);
  console.log("owner key   ", ownerKeyPath);

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

  // 2. fund vault + device key with SOL (mainnet uses smaller amounts —
  //    every lamport costs real money)
  console.log("[2] funding vault + device with SOL");
  const vaultLamports  = cluster === "mainnet" ? 0.005 * LAMPORTS_PER_SOL : 0.05 * LAMPORTS_PER_SOL;
  const deviceLamports = cluster === "mainnet" ? 0.005 * LAMPORTS_PER_SOL : 0.02 * LAMPORTS_PER_SOL;
  const ixs: anchor.web3.TransactionInstruction[] = [];
  const vaultBal = await conn.getBalance(vault);
  if (vaultBal < vaultLamports) {
    ixs.push(SystemProgram.transfer({
      fromPubkey: owner.publicKey, toPubkey: vault,
      lamports: vaultLamports - vaultBal,
    }));
  }
  const deviceBal = await conn.getBalance(devicePk);
  if (deviceBal < deviceLamports) {
    ixs.push(SystemProgram.transfer({
      fromPubkey: owner.publicKey, toPubkey: devicePk,
      lamports: deviceLamports - deviceBal,
    }));
  }
  if (ixs.length) {
    const sig = await provider.sendAndConfirm(new Transaction().add(...ixs));
    console.log("    sig:", sig);
  } else {
    console.log("    already funded");
  }

  // 3. set up the USDC mint + ATAs
  let mint: PublicKey;
  let ownerAta: PublicKey;
  let vaultAta: PublicKey;

  if (cluster === "devnet") {
    console.log("[3] minting fresh test USDC + vault & owner ATAs");
    mint = await createMint(conn, owner, owner.publicKey, null, 6);
    const va = await getOrCreateAssociatedTokenAccount(conn, owner, mint, vault, true);
    const oa = await getOrCreateAssociatedTokenAccount(conn, owner, mint, owner.publicKey);
    vaultAta = va.address;
    ownerAta = oa.address;
    await mintTo(conn, owner, mint, vaultAta, owner, 1_000_000);
  } else {
    console.log("[3] using real USDC + ensuring vault & owner & device ATAs");
    mint = new PublicKey(REAL_USDC_MAINNET);
    const va = await getOrCreateAssociatedTokenAccount(conn, owner, mint, vault, true);
    const oa = await getOrCreateAssociatedTokenAccount(conn, owner, mint, owner.publicKey);
    const da = await getOrCreateAssociatedTokenAccount(conn, owner, mint, devicePk);
    vaultAta = va.address;
    ownerAta = oa.address;

    // Top up the vault's USDC if it's below the test threshold.
    const vaultBal = (await conn.getTokenAccountBalance(vaultAta)).value.amount;
    if (BigInt(vaultBal) < TEST_VAULT_USDC_FUND) {
      const need = TEST_VAULT_USDC_FUND - BigInt(vaultBal);
      console.log(`    funding vault USDC: ${vaultBal} → ${TEST_VAULT_USDC_FUND} (sending ${need})`);
      const ix = createTransferCheckedInstruction(
        ownerAta, mint, vaultAta, owner.publicKey, need, 6
      );
      const sig = await provider.sendAndConfirm(new Transaction().add(ix));
      console.log("    sig:", sig);
    } else {
      console.log("    vault USDC already ≥ $1");
    }

    // Ensure the device's ATA has a small buffer for the spending-account
    // pattern: the x402 facilitator's exact-scheme validator wants ix[2]
    // to be a direct TransferChecked from the device's ATA, so the device
    // needs a few micro-USDC sitting there. Each call refills the same
    // amount in ix[3] via vault_execute, so this buffer is one-time.
    const deviceBuffer = 100_000n; // 0.1 USDC — covers ~3000 calls @ $0.00003 each
    const deviceUsdcBal = (await conn.getTokenAccountBalance(da.address)).value.amount;
    if (BigInt(deviceUsdcBal) < deviceBuffer) {
      const need = deviceBuffer - BigInt(deviceUsdcBal);
      console.log(`    funding device USDC: ${deviceUsdcBal} → ${deviceBuffer} (sending ${need})`);
      const ix = createTransferCheckedInstruction(
        ownerAta, mint, da.address, owner.publicKey, need, 6
      );
      const sig = await provider.sendAndConfirm(new Transaction().add(ix));
      console.log("    sig:", sig);
    } else {
      console.log("    device USDC already ≥ buffer");
    }
  }
  console.log("    mint:     ", mint.toBase58());
  console.log("    vault_ata:", vaultAta.toBase58());
  console.log("    owner_ata:", ownerAta.toBase58());

  // 4. persist for the firmware
  const out = path.resolve(`target/test-vault-${cluster}.json`);
  fs.mkdirSync(path.dirname(out), { recursive: true });
  fs.writeFileSync(
    out,
    JSON.stringify(
      {
        cluster,
        program_id: program.programId.toBase58(),
        owner: owner.publicKey.toBase58(),
        device: devicePk.toBase58(),
        agent_root: agentRoot.toBase58(),
        vault: vault.toBase58(),
        usdc_mint: mint.toBase58(),
        vault_usdc_ata: vaultAta.toBase58(),
        owner_usdc_ata: ownerAta.toBase58(),
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
