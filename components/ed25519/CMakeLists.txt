# Ed25519 signing for Solana x402 payments.
#
# Sources are vendored verbatim from orlp/ed25519 @ master (zlib-like licence,
# see LICENSE in this directory). The library's 16 files are tightly coupled
# so we pull the full set except seed.c (needs /dev/urandom — we don't create
# fresh seeds on-device) and add_scalar.c / key_exchange.c (unused). verify.c
# is included so swap_test_sig_for_test can self-verify spliced signatures.
idf_component_register(
    SRCS
        "src/fe.c"
        "src/ge.c"
        "src/keypair.c"
        "src/pk_on_curve.c"
        "src/sc.c"
        "src/sha512.c"
        "src/sign.c"
        "src/verify.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
)
