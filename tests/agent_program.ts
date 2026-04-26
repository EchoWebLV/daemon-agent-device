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

    const acct = await program.account.agentRoot.fetch(agentRoot);
    expect(acct.owner.toBase58()).to.equal(owner.publicKey.toBase58());
    expect(acct.createdAt.toNumber()).to.be.greaterThan(0);
  });

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
});
