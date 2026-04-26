// Solana PDA derivation needs to reject pubkey candidates that happen to land
// ON the Ed25519 curve (those would be valid keypair pubkeys, not PDAs).
// orlp/ed25519's ge_frombytes_negate_vartime returns 0 iff the 32-byte input
// decodes to a valid Edwards point — exactly the condition we want.

#include "ed25519.h"
#include "ge.h"

int ED25519_DECLSPEC ed25519_pk_is_on_curve(const unsigned char pk[32]) {
    ge_p3 A;
    return ge_frombytes_negate_vartime(&A, pk) == 0 ? 1 : 0;
}
