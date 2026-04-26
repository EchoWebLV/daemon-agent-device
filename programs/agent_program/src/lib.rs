use anchor_lang::prelude::*;

pub mod state;

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
