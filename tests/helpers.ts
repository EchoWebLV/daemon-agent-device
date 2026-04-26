import * as anchor from "@coral-xyz/anchor";
import {
  PublicKey,
  Keypair,
  SystemProgram,
  Transaction,
  SystemProgram as SP,
} from "@solana/web3.js";

export const PROGRAM_ID = new PublicKey(
  // overwritten by anchor.workspace.AgentProgram.programId at runtime;
  // this constant is only used for direct PDA derivation outside tests.
  "FNVcw2kCnzSxZwqNEnoSMmVnTLXaiusxupLZ4CWkYPAA"
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
