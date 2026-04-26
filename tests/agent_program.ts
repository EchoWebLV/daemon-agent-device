import * as anchor from "@coral-xyz/anchor";
import { Program } from "@coral-xyz/anchor";
import { Keypair, PublicKey, SystemProgram, LAMPORTS_PER_SOL } from "@solana/web3.js";
import { expect } from "chai";
import { AgentProgram } from "../target/types/agent_program";
import {
  deriveAgentRoot,
  deriveVault,
  fundLamports,
  setupMintAndAtas,
  TOKEN_PROGRAM_ID,
  createTransferCheckedInstruction,
} from "./helpers";

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

  it("vault_execute: device signer drives SPL TransferChecked CPI", async () => {
    const owner = Keypair.generate();
    const device = Keypair.generate();
    const dest = Keypair.generate();
    const payer = (provider.wallet as anchor.Wallet).payer;

    await fundLamports(provider.connection, owner.publicKey, 2 * LAMPORTS_PER_SOL);
    await fundLamports(provider.connection, device.publicKey, 0.05 * LAMPORTS_PER_SOL);

    const [agentRoot] = deriveAgentRoot(owner.publicKey, program.programId);
    const [vault] = deriveVault(agentRoot, program.programId);

    await program.methods
      .initializeAgent(device.publicKey)
      .accounts({ owner: owner.publicKey, agentRoot, vault, systemProgram: SystemProgram.programId })
      .signers([owner])
      .rpc();

    const { mint, atas } = await setupMintAndAtas(provider.connection, payer, [vault, dest.publicKey]);
    const [vaultAta, destAta] = atas;

    const inner = createTransferCheckedInstruction(vaultAta, mint, destAta, vault, 100_000n, 6);
    const innerData = {
      programId: inner.programId,
      accounts: inner.keys.map((k) => ({ pubkey: k.pubkey, isSigner: k.isSigner, isWritable: k.isWritable })),
      data: inner.data,
    };

    await program.methods
      .vaultExecute(innerData as any)
      .accounts({ vault, currentSigner: device.publicKey })
      .remainingAccounts([
        ...inner.keys.map((k) => ({ pubkey: k.pubkey, isSigner: false, isWritable: k.isWritable })),
        { pubkey: TOKEN_PROGRAM_ID, isSigner: false, isWritable: false },
      ])
      .signers([device])
      .rpc();

    const destAcct = await provider.connection.getTokenAccountBalance(destAta);
    expect(destAcct.value.amount).to.equal("100000");
  });

  it("vault_execute: wrong signer is rejected with WrongSigner", async () => {
    const owner = Keypair.generate();
    const device = Keypair.generate();
    const evil = Keypair.generate();
    const dest = Keypair.generate();
    const payer = (provider.wallet as anchor.Wallet).payer;

    await fundLamports(provider.connection, owner.publicKey, 2 * LAMPORTS_PER_SOL);
    await fundLamports(provider.connection, evil.publicKey, 0.05 * LAMPORTS_PER_SOL);

    const [agentRoot] = deriveAgentRoot(owner.publicKey, program.programId);
    const [vault] = deriveVault(agentRoot, program.programId);

    await program.methods
      .initializeAgent(device.publicKey)
      .accounts({ owner: owner.publicKey, agentRoot, vault, systemProgram: SystemProgram.programId })
      .signers([owner])
      .rpc();

    const { mint, atas } = await setupMintAndAtas(provider.connection, payer, [vault, dest.publicKey]);
    const [vaultAta, destAta] = atas;

    const inner = createTransferCheckedInstruction(vaultAta, mint, destAta, vault, 50_000n, 6);
    const innerData = {
      programId: inner.programId,
      accounts: inner.keys.map((k) => ({ pubkey: k.pubkey, isSigner: k.isSigner, isWritable: k.isWritable })),
      data: inner.data,
    };

    let threw = false;
    try {
      await program.methods
        .vaultExecute(innerData as any)
        .accounts({ vault, currentSigner: evil.publicKey })
        .remainingAccounts([
          ...inner.keys.map((k) => ({ pubkey: k.pubkey, isSigner: false, isWritable: k.isWritable })),
          { pubkey: TOKEN_PROGRAM_ID, isSigner: false, isWritable: false },
        ])
        .signers([evil])
        .rpc();
    } catch (e: any) {
      threw = true;
      expect(e.toString()).to.match(/WrongSigner/);
    }
    expect(threw).to.equal(true);
  });
});
