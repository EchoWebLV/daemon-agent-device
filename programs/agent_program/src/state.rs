use anchor_lang::prelude::*;

#[account]
pub struct AgentRoot {
    pub owner: Pubkey,
    pub created_at: i64,
    pub bump: u8,
}

impl AgentRoot {
    pub const LEN: usize = 8 + 32 + 8 + 1;
}

#[account]
pub struct Vault {
    pub agent_root: Pubkey,
    pub current_signer: Pubkey,
    pub recovery_authority: Pubkey,
    pub signer_rotated_at: i64,
    pub bump: u8,
}

impl Vault {
    pub const LEN: usize = 8 + 32 + 32 + 32 + 8 + 1;
}
