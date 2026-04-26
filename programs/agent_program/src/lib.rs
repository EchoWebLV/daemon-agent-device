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
        msg!("agent_root initialized; device={}", device);
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

    pub system_program: Program<'info, System>,
}
