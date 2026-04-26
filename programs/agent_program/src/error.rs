use anchor_lang::prelude::*;

#[error_code]
pub enum AgentError {
    #[msg("signer is not the vault's current_signer")]
    WrongSigner,
    #[msg("signer is not the recovery_authority")]
    NotRecoveryAuthority,
    #[msg("nothing to sweep above rent-min")]
    NothingToSweep,
}
