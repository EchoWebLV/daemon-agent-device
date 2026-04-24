#!/usr/bin/env python3
"""Standalone x402 client for probing endpoints.

Mirrors chrome-ext/src/lib/x402-fetch.ts so results match what the
device is doing: hit the URL, parse the 402 challenge, sign a USDC
TransferChecked with the wallet as authority + the challenge's feePayer
as the Solana fee payer, retry with a PAYMENT-SIGNATURE header, print
the final status + body.

    python3 tests/x402_probe.py PRIV METHOD URL [json_body]
"""
import base64
import json
import sys

import base58
import requests
from solana.rpc.api import Client
from solders.hash import Hash
from solders.instruction import AccountMeta, Instruction
from solders.keypair import Keypair
from solders.message import MessageV0
from solders.pubkey import Pubkey
from solders.signature import Signature
from solders.transaction import VersionedTransaction

USDC_MINT = Pubkey.from_string("EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v")
USDC_DECIMALS = 6
SOLANA_NETWORK = "solana:5eykt4UsFv8P8NJdTREpY1vzqKqZKvdp"
TOKEN_PROGRAM = Pubkey.from_string("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA")
ASSOC_PROGRAM = Pubkey.from_string("ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL")
COMPUTE_BUDGET = Pubkey.from_string("ComputeBudget111111111111111111111111111111")
RPC_URL = "https://api.mainnet-beta.solana.com"


def get_ata(owner: Pubkey, mint: Pubkey) -> Pubkey:
    pda, _ = Pubkey.find_program_address(
        [bytes(owner), bytes(TOKEN_PROGRAM), bytes(mint)], ASSOC_PROGRAM
    )
    return pda


def transfer_checked_ix(
    src_ata: Pubkey, mint: Pubkey, dst_ata: Pubkey,
    authority: Pubkey, amount: int, decimals: int,
) -> Instruction:
    data = bytes([12]) + amount.to_bytes(8, "little") + bytes([decimals])
    return Instruction(
        program_id=TOKEN_PROGRAM,
        accounts=[
            AccountMeta(src_ata, False, True),
            AccountMeta(mint, False, False),
            AccountMeta(dst_ata, False, True),
            AccountMeta(authority, True, False),
        ],
        data=data,
    )


def set_cu_limit_ix(units: int) -> Instruction:
    return Instruction(
        program_id=COMPUTE_BUDGET,
        accounts=[],
        data=bytes([2]) + units.to_bytes(4, "little"),
    )


def set_cu_price_ix(micro_lamports: int) -> Instruction:
    return Instruction(
        program_id=COMPUTE_BUDGET,
        accounts=[],
        data=bytes([3]) + micro_lamports.to_bytes(8, "little"),
    )


def build_payment_header(
    priv_b58: str, recipient_b58: str, amount: str, fee_payer_b58: str,
    resource_url: str, resource_desc: str, max_timeout: int,
    extra, extensions, rpc_url: str = RPC_URL,
) -> str:
    kp = Keypair.from_bytes(base58.b58decode(priv_b58))
    owner = kp.pubkey()
    fee_payer = Pubkey.from_string(fee_payer_b58)
    recipient = Pubkey.from_string(recipient_b58)
    src_ata = get_ata(owner, USDC_MINT)
    dst_ata = get_ata(recipient, USDC_MINT)

    client = Client(rpc_url)
    blockhash = client.get_latest_blockhash().value.blockhash

    msg = MessageV0.try_compile(
        payer=fee_payer,
        instructions=[
            set_cu_limit_ix(8000),
            set_cu_price_ix(1),
            transfer_checked_ix(src_ata, USDC_MINT, dst_ata, owner,
                                int(amount), USDC_DECIMALS),
        ],
        address_lookup_table_accounts=[],
        recent_blockhash=blockhash,
    )
    # Partial-sign: the facilitator signs as fee_payer later. solders'
    # VersionedTransaction(msg, [kp]) rejects unsigned slots, so we populate
    # signatures manually — empty for every signer except our own.
    # Gotcha: bytes(MessageV0) returns the legacy serialization (no 0x80
    # version prefix), but the VersionedTransaction wire format embeds the
    # message WITH the prefix. Signing bytes(msg) makes the signature fail
    # verification against the on-wire bytes. Prepend 0x80 when signing.
    num_signers = msg.header.num_required_signatures
    signer_keys = list(msg.account_keys)[:num_signers]
    my_idx = signer_keys.index(owner)
    sigs = [Signature.default()] * num_signers
    sigs[my_idx] = kp.sign_message(b"\x80" + bytes(msg))
    tx = VersionedTransaction.populate(msg, sigs)
    serialized = bytes(tx)

    payload = {
        "x402Version": 2,
        "resource": {
            "url": resource_url,
            "description": resource_desc,
            "mimeType": "application/json",
        },
        "accepted": {
            "scheme": "exact",
            "network": SOLANA_NETWORK,
            "amount": str(amount),
            "asset": str(USDC_MINT),
            "payTo": recipient_b58,
            "maxTimeoutSeconds": max_timeout,
            "extra": extra or {"feePayer": fee_payer_b58},
        },
        "payload": {"transaction": base64.b64encode(serialized).decode("ascii")},
        "extensions": extensions or {},
    }
    return base64.b64encode(json.dumps(payload).encode()).decode()


def x402_fetch(priv_b58: str, method: str, url: str, body=None,
               rpc_url: str = RPC_URL) -> requests.Response:
    method = method.upper()
    headers = {"Content-Type": "application/json"}
    data = body if method not in ("GET", "DELETE") else None
    r = requests.request(method, url, headers=headers, data=data, timeout=60)
    if r.status_code != 402:
        return r

    challenge = r.headers.get("payment-required")
    if not challenge:
        challenge = base64.b64encode(r.content).decode()
    pr = json.loads(base64.b64decode(challenge))

    opt = next((o for o in pr["accepts"] if o["network"] == SOLANA_NETWORK),
               pr["accepts"][0])
    amount = opt.get("amount") or opt.get("maxAmountRequired")
    fee_payer = opt["extra"]["feePayer"]

    header_val = build_payment_header(
        priv_b58, opt["payTo"], amount, fee_payer,
        pr.get("resource", {}).get("url", url),
        pr.get("resource", {}).get("description", "x402 API call"),
        opt.get("maxTimeoutSeconds", 300),
        opt.get("extra"),
        pr.get("extensions"),
        rpc_url=rpc_url,
    )
    return requests.request(
        method, url,
        headers={**headers, "PAYMENT-SIGNATURE": header_val},
        data=data, timeout=60,
    )


def main():
    if len(sys.argv) < 4:
        print("usage: x402_probe.py PRIV METHOD URL [json_body]", file=sys.stderr)
        sys.exit(2)
    priv, method, url = sys.argv[1], sys.argv[2], sys.argv[3]
    body = sys.argv[4] if len(sys.argv) >= 5 else None
    r = x402_fetch(priv, method, url, body)
    print(f"HTTP {r.status_code}")
    try:
        print(json.dumps(r.json(), indent=2)[:4000])
    except Exception:
        print(r.text[:4000])


if __name__ == "__main__":
    main()
