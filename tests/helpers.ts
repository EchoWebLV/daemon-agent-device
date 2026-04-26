import * as anchor from "@coral-xyz/anchor";
import {
  PublicKey,
  Keypair,
  SystemProgram,
  Transaction,
  SystemProgram as SP,
} from "@solana/web3.js";
import {
  createMint,
  createAssociatedTokenAccount,
  mintTo,
  TOKEN_PROGRAM_ID,
  getAssociatedTokenAddressSync,
  createTransferCheckedInstruction,
} from "@solana/spl-token";

export const PROGRAM_ID = new PublicKey(
  // overwritten by anchor.workspace.AgentProgram.programId at runtime;
  // this constant is only used for direct PDA derivation outside tests.
  "FNVcw2kCnzSxZwqNEnoSMmVnTLXaiusxupLZ4CWkYPAA"
);

export {
  TOKEN_PROGRAM_ID,
  getAssociatedTokenAddressSync,
  createTransferCheckedInstruction,
};

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

/**
 * Fund a public key by transferring lamports from the AnchorProvider wallet.
 * Uses a system-program transfer instead of requestAirdrop, which is broken
 * on solana-test-validator 2.3.x.
 */
export async function fundLamports(
  conn: anchor.web3.Connection,
  to: PublicKey,
  lamports: number
) {
  const provider = anchor.AnchorProvider.env();
  const tx = new Transaction().add(
    SP.transfer({
      fromPubkey: provider.wallet.publicKey,
      toPubkey: to,
      lamports,
    })
  );
  await provider.sendAndConfirm(tx);
}

/**
 * Create a fresh SPL mint, create ATAs for the given owners (with the vault PDA
 * able to be one of them via `allowOwnerOffCurve = true`), and mint
 * `mintAmountToFirst` to the first owner's ATA. Returns the mint and the list
 * of ATAs (in the same order as `owners`).
 */
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
    const ata = await createAssociatedTokenAccount(
      conn,
      payer,
      mint,
      o,
      undefined,
      undefined,
      undefined,
      true
    );
    atas.push(ata);
  }
  await mintTo(conn, payer, mint, atas[0], payer, Number(mintAmountToFirst));
  return { mint, atas };
}
