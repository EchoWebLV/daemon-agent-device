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
