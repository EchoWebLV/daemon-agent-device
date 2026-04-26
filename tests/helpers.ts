import * as anchor from "@coral-xyz/anchor";
import { PublicKey, Keypair, SystemProgram } from "@solana/web3.js";

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

export async function fundLamports(
  conn: anchor.web3.Connection,
  to: PublicKey,
  lamports: number
) {
  const sig = await conn.requestAirdrop(to, lamports);
  await conn.confirmTransaction(sig, "confirmed");
}
