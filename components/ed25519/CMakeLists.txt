# Ed25519 signing for Solana x402 payments.
#
# Sources are vendored verbatim from orlp/ed25519 @ master (zlib-like licence,
# see LICENSE in this directory). We only need the sign path, but the
# library's 16 files are tightly coupled so we pull the full set except for
# seed.c (needs /dev/urandom — we don't create fresh seeds on-device) and
# verify.c / add_scalar.c / key_exchange.c (unused). That keeps this to
# ~100 KB of precomputed table + ~6 KB of actual code.
idf_component_register(
    SRCS
        "src/fe.c"
        "src/ge.c"
        "src/keypair.c"
        "src/sc.c"
        "src/sha512.c"
        "src/sign.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
)
