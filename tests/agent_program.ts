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
});
