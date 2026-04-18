import {
  Connection,
  PublicKey,
  Keypair,
  TransactionMessage,
  VersionedTransaction,
  ComputeBudgetProgram,
} from '@solana/web3.js';
import {
  getAssociatedTokenAddress,
  createTransferCheckedInstruction,
  getMint,
} from '@solana/spl-token';
import bs58 from 'bs58';
import { getSession, getStorageValue, StorageKeys } from './storage';
import { RPC_ENDPOINTS } from '../types';

const SOLANA_NETWORK_ID = 'solana:5eykt4UsFv8P8NJdTREpY1vzqKqZKvdp';
const USDC_MINT = 'EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v';
const COMPUTE_UNIT_PRICE = 1;
const COMPUTE_UNIT_LIMIT = 8_000;
const TIMEOUT_MS = 60_000;

interface PaymentRequired {
  x402Version: number;
  accepts: PaymentOption[];
  resource?: { url: string; description?: string; mimeType?: string };
  extensions?: Record<string, unknown>;
}

interface PaymentOption {
  scheme: string;
  network: string;
  asset: string;
  amount?: string;
  maxAmountRequired?: string;
  payTo: string;
  maxTimeoutSeconds?: number;
  extra?: { name?: string; version?: string; feePayer?: string };
}

export interface X402FetchResult {
  ok: boolean;
  status: number;
  data: unknown;
  costUsd: number;
}

async function getKeys(): Promise<{ privateKey: string; rpcUrl: string }> {
  const privateKey = await getSession();
  if (!privateKey) throw new Error('Wallet session expired. Please unlock your wallet.');
  const customRpc = await getStorageValue<string>(StorageKeys.CUSTOM_RPC);
  const rpcUrl = customRpc || RPC_ENDPOINTS['mainnet-beta'];
  return { privateKey, rpcUrl };
}

function fetchWithTimeout(url: string, init: RequestInit): Promise<Response> {
  const controller = new AbortController();
  const timeoutId = setTimeout(() => controller.abort(), TIMEOUT_MS);
  return fetch(url, { ...init, signal: controller.signal }).finally(() =>
    clearTimeout(timeoutId),
  );
}

async function parseBody(res: Response): Promise<unknown> {
  const clone = res.clone();
  try {
    return await clone.json();
  } catch {
    return await res.text();
  }
}

async function createPaymentPayload(
  privateKey: string,
  rpcUrl: string,
  recipient: string,
  amount: string,
  feePayer: string,
  resource: { url: string; description: string },
  maxTimeoutSeconds: number,
  extra?: Record<string, unknown>,
  extensions?: Record<string, unknown>,
): Promise<string> {
  const secretKey = bs58.decode(privateKey);
  const keypair = Keypair.fromSecretKey(secretKey);
  const connection = new Connection(rpcUrl, { commitment: 'confirmed' });

  const feePayerPubkey = new PublicKey(feePayer);
  const ownerPubkey = keypair.publicKey;
  const tokenMint = new PublicKey(USDC_MINT);
  const payToPubkey = new PublicKey(recipient);

  const mintInfo = await getMint(connection, tokenMint);
  const sourceATA = await getAssociatedTokenAddress(tokenMint, ownerPubkey, false);
  const destinationATA = await getAssociatedTokenAddress(tokenMint, payToPubkey, false);
  const { blockhash } = await connection.getLatestBlockhash();

  const transferIx = createTransferCheckedInstruction(
    sourceATA,
    tokenMint,
    destinationATA,
    ownerPubkey,
    BigInt(amount),
    mintInfo.decimals,
  );

  const messageV0 = new TransactionMessage({
    payerKey: feePayerPubkey,
    recentBlockhash: blockhash,
    instructions: [
      ComputeBudgetProgram.setComputeUnitLimit({ units: COMPUTE_UNIT_LIMIT }),
      ComputeBudgetProgram.setComputeUnitPrice({ microLamports: COMPUTE_UNIT_PRICE }),
      transferIx,
    ],
  }).compileToV0Message();

  const transaction = new VersionedTransaction(messageV0);
  transaction.sign([keypair]);

  const serializedTx = Buffer.from(transaction.serialize()).toString('base64');

  return btoa(
    JSON.stringify({
      x402Version: 2,
      resource: { ...resource, mimeType: 'application/json' },
      accepted: {
        scheme: 'exact',
        network: SOLANA_NETWORK_ID,
        amount,
        asset: USDC_MINT,
        payTo: recipient,
        maxTimeoutSeconds,
        extra: extra || { feePayer },
      },
      payload: { transaction: serializedTx },
      extensions: extensions || {},
    }),
  );
}

/**
 * Generic x402 fetch — makes a request to any x402-enabled endpoint,
 * handles the 402 payment flow with Solana USDC, and returns the response.
 */
export async function x402Fetch(
  url: string,
  options: {
    method?: string;
    headers?: Record<string, string>;
    body?: string;
  } = {},
): Promise<X402FetchResult> {
  const { privateKey, rpcUrl } = await getKeys();
  const method = options.method || 'GET';

  const reqInit: RequestInit = {
    method,
    headers: {
      'Content-Type': 'application/json',
      ...(options.headers || {}),
    },
  };
  if (options.body && method !== 'GET') reqInit.body = options.body;

  const response = await fetchWithTimeout(url, reqInit);

  if (response.status !== 402) {
    const data = await parseBody(response);
    return { ok: response.ok, status: response.status, data, costUsd: 0 };
  }

  let paymentHeader = response.headers.get('payment-required');
  if (!paymentHeader) {
    try {
      const body = await parseBody(response);
      if (body && typeof body === 'object' && ('accepts' in body || 'x402Version' in body)) {
        paymentHeader = btoa(JSON.stringify(body));
      }
    } catch { /* ignore */ }
  }
  if (!paymentHeader) {
    throw new Error('402 response but no payment requirements found');
  }

  const paymentRequired: PaymentRequired = JSON.parse(atob(paymentHeader));
  if (!paymentRequired.accepts?.length) {
    throw new Error('No payment options in 402 response');
  }

  const option =
    paymentRequired.accepts.find((o) => o.network === SOLANA_NETWORK_ID) ||
    paymentRequired.accepts[0];

  const amount = option.amount || option.maxAmountRequired;
  if (!amount) throw new Error('No payment amount specified');

  const feePayer = option.extra?.feePayer;
  if (!feePayer) throw new Error('Missing feePayer in payment requirements');

  const paymentPayload = await createPaymentPayload(
    privateKey,
    rpcUrl,
    option.payTo,
    amount,
    feePayer,
    {
      url: paymentRequired.resource?.url || url,
      description: paymentRequired.resource?.description || 'x402 API call',
    },
    option.maxTimeoutSeconds || 300,
    option.extra,
    paymentRequired.extensions,
  );

  const retryResponse = await fetchWithTimeout(url, {
    ...reqInit,
    headers: {
      ...(reqInit.headers as Record<string, string>),
      'PAYMENT-SIGNATURE': paymentPayload,
    },
  });

  if (retryResponse.status === 402) {
    throw new Error('Payment rejected. Check your Solana USDC balance.');
  }

  const data = await parseBody(retryResponse);
  const costUsd = parseFloat(amount) / 1e6;

  return { ok: retryResponse.ok, status: retryResponse.status, data, costUsd };
}
