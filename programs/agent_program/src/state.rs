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
