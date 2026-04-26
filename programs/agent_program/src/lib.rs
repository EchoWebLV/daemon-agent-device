use anchor_lang::prelude::*;
use anchor_lang::solana_program::instruction::{AccountMeta, Instruction};
use anchor_lang::solana_program::program::invoke_signed;

pub mod error;
pub mod state;

use error::*;
use state::*;

declare_id!("FNVcw2kCnzSxZwqNEnoSMmVnTLXaiusxupLZ4CWkYPAA");

#[program]
pub mod agent_program {
    use super::*;

    pub fn initialize_agent(ctx: Context<InitializeAgent>, device: Pubkey) -> Result<()> {
        let root = &mut ctx.accounts.agent_root;
        root.owner = ctx.accounts.owner.key();
        root.created_at = Clock::get()?.unix_timestamp;
        root.bump = ctx.bumps.agent_root;

        let vault = &mut ctx.accounts.vault;
        vault.agent_root = root.key();
        vault.current_signer = device;
        vault.recovery_authority = ctx.accounts.owner.key();
        vault.signer_rotated_at = root.created_at;
        vault.bump = ctx.bumps.vault;

        msg!("agent + vault initialized; device={}", device);
        Ok(())
    }

    pub fn vault_execute(ctx: Context<VaultExecute>, inner: InnerIx) -> Result<()> {
        let vault = &ctx.accounts.vault;
        require_keys_eq!(
            ctx.accounts.current_signer.key(),
            vault.current_signer,
            AgentError::WrongSigner
        );

        // remaining_accounts must contain all AccountInfos used by the inner ix
        // PLUS the inner ix's program account. Solana's runtime resolves metas
        // to AccountInfos by pubkey when invoke_signed runs.
        let metas: Vec<AccountMeta> = inner
            .accounts
            .iter()
            .map(|a| {
                if a.is_writable {
                    AccountMeta::new(a.pubkey, a.is_signer)
                } else {
                    AccountMeta::new_readonly(a.pubkey, a.is_signer)
                }
            })
            .collect();

        let ix = Instruction {
            program_id: inner.program_id,
            accounts: metas,
            data: inner.data,
        };

        let agent_root = vault.agent_root;
        let signer_seeds: &[&[u8]] = &[b"vault", agent_root.as_ref(), &[vault.bump]];

        invoke_signed(&ix, ctx.remaining_accounts, &[signer_seeds])?;
        Ok(())
    }

    pub fn vault_rotate_signer(ctx: Context<VaultRotateSigner>, new_signer: Pubkey) -> Result<()> {
        let vault = &mut ctx.accounts.vault;
        require_keys_eq!(
            ctx.accounts.recovery_authority.key(),
            vault.recovery_authority,
            AgentError::NotRecoveryAuthority
        );
        vault.current_signer = new_signer;
        vault.signer_rotated_at = Clock::get()?.unix_timestamp;
        msg!("vault rotated; new_signer={}", new_signer);
        Ok(())
    }
}

#[derive(AnchorSerialize, AnchorDeserialize, Clone)]
pub struct InnerIxAccount {
    pub pubkey: Pubkey,
    pub is_signer: bool,
    pub is_writable: bool,
}

#[derive(AnchorSerialize, AnchorDeserialize, Clone)]
pub struct InnerIx {
    pub program_id: Pubkey,
    pub accounts: Vec<InnerIxAccount>,
    pub data: Vec<u8>,
}

#[derive(Accounts)]
pub struct VaultExecute<'info> {
    #[account(
        mut,
        seeds = [b"vault", vault.agent_root.as_ref()],
        bump = vault.bump,
    )]
    pub vault: Account<'info, Vault>,

    pub current_signer: Signer<'info>,
}

#[derive(Accounts)]
pub struct VaultRotateSigner<'info> {
    #[account(
        mut,
        seeds = [b"vault", vault.agent_root.as_ref()],
        bump = vault.bump,
    )]
    pub vault: Account<'info, Vault>,

    pub recovery_authority: Signer<'info>,
}

#[derive(Accounts)]
pub struct InitializeAgent<'info> {
    #[account(mut)]
    pub owner: Signer<'info>,

    #[account(
        init,
        payer = owner,
        space = AgentRoot::LEN,
        seeds = [b"agent", owner.key().as_ref()],
        bump
    )]
    pub agent_root: Account<'info, AgentRoot>,

    #[account(
        init,
        payer = owner,
        space = Vault::LEN,
        seeds = [b"vault", agent_root.key().as_ref()],
        bump
    )]
    pub vault: Account<'info, Vault>,

    pub system_program: Program<'info, System>,
}
