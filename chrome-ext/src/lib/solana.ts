import {
  Connection,
  PublicKey,
  Keypair,
  Transaction,
  SystemProgram,
  sendAndConfirmTransaction,
  LAMPORTS_PER_SOL,
} from '@solana/web3.js';
import {
  getAssociatedTokenAddress,
  createAssociatedTokenAccountInstruction,
  createTransferInstruction,
  getAccount,
  TOKEN_PROGRAM_ID,
  ASSOCIATED_TOKEN_PROGRAM_ID,
} from '@solana/spl-token';
import type { Network } from '../types';
import { USDC_MINT_MAINNET, USDC_MINT_DEVNET, USDC_DECIMALS, RPC_ENDPOINTS, JUPITER_API_KEY } from '../types';

const JUP_BASE = 'https://api.jup.ag';

function jupHeaders(): Record<string, string> {
  const h: Record<string, string> = {};
  if (JUPITER_API_KEY) h['x-api-key'] = JUPITER_API_KEY;
  return h;
}

let _connection: Connection | null = null;
let _network: Network = 'mainnet-beta';
let _customRpc: string | null = null;

export function setCustomRpc(url: string | null) {
  _customRpc = url;
  _connection = null;
}

export function getConnection(network?: Network): Connection {
  const net = network ?? _network;
  const rpcUrl = _customRpc || RPC_ENDPOINTS[net];
  if (!_connection || net !== _network) {
    _network = net;
    const wsUrl = rpcUrl.replace('https://', 'wss://');
    _connection = new Connection(rpcUrl, {
      commitment: 'confirmed',
      wsEndpoint: wsUrl,
    });
  }
  return _connection;
}

export function getUSDCMint(network: Network): PublicKey {
  return new PublicKey(network === 'mainnet-beta' ? USDC_MINT_MAINNET : USDC_MINT_DEVNET);
}

export async function getSOLBalance(publicKey: PublicKey, connection: Connection): Promise<number> {
  const balance = await connection.getBalance(publicKey);
  return balance / LAMPORTS_PER_SOL;
}

export async function getUSDCBalance(
  publicKey: PublicKey,
  connection: Connection,
  network: Network = 'mainnet-beta',
): Promise<number> {
  try {
    const mint = getUSDCMint(network);
    const ata = await getAssociatedTokenAddress(mint, publicKey);
    const account = await getAccount(connection, ata);
    return Number(account.amount) / 10 ** USDC_DECIMALS;
  } catch {
    return 0;
  }
}

export async function sendSOL(
  from: Keypair,
  to: string,
  amount: number,
  connection: Connection,
): Promise<string> {
  const recipient = new PublicKey(to);
  const tx = new Transaction().add(
    SystemProgram.transfer({
      fromPubkey: from.publicKey,
      toPubkey: recipient,
      lamports: Math.round(amount * LAMPORTS_PER_SOL),
    }),
  );
  return sendAndConfirmTransaction(connection, tx, [from]);
}

export async function sendUSDC(
  from: Keypair,
  to: string,
  amount: number,
  connection: Connection,
  network: Network = 'mainnet-beta',
): Promise<string> {
  const mint = getUSDCMint(network);
  const recipient = new PublicKey(to);
  const senderATA = await getAssociatedTokenAddress(mint, from.publicKey);
  const recipientATA = await getAssociatedTokenAddress(mint, recipient);

  const tx = new Transaction();

  try {
    await getAccount(connection, recipientATA);
  } catch {
    tx.add(
      createAssociatedTokenAccountInstruction(
        from.publicKey,
        recipientATA,
        recipient,
        mint,
        TOKEN_PROGRAM_ID,
        ASSOCIATED_TOKEN_PROGRAM_ID,
      ),
    );
  }

  tx.add(
    createTransferInstruction(
      senderATA,
      recipientATA,
      from.publicKey,
      BigInt(Math.round(amount * 10 ** USDC_DECIMALS)),
    ),
  );

  return sendAndConfirmTransaction(connection, tx, [from]);
}

export interface TokenHolding {
  mint: string;
  symbol: string;
  name: string;
  amount: number;
  decimals: number;
  usdPrice: number | null;
  usdValue: number | null;
}

interface JupiterTokenMeta {
  symbol: string;
  name: string;
}

let _tokenMetaCache: Map<string, JupiterTokenMeta> | null = null;
let _tokenMetaFetchPromise: Promise<Map<string, JupiterTokenMeta>> | null = null;

async function fetchTokenMetaMap(): Promise<Map<string, JupiterTokenMeta>> {
  if (_tokenMetaCache) return _tokenMetaCache;
  if (_tokenMetaFetchPromise) return _tokenMetaFetchPromise;

  _tokenMetaFetchPromise = (async () => {
    try {
      const resp = await fetch(`${JUP_BASE}/tokens/v2/tag?query=verified`, { headers: jupHeaders() });
      const list: Array<{ id?: string; address?: string; symbol: string; name: string }> = await resp.json();
      const map = new Map<string, JupiterTokenMeta>();
      for (const t of list) {
        const addr = t.id ?? t.address;
        if (addr) map.set(addr, { symbol: t.symbol, name: t.name });
      }
      _tokenMetaCache = map;
      return map;
    } catch {
      return new Map();
    }
  })();

  return _tokenMetaFetchPromise;
}

interface DexScreenerToken {
  symbol: string;
  name: string;
  priceUsd: number | null;
}

async function fetchDexScreenerMeta(mints: string[]): Promise<Map<string, DexScreenerToken>> {
  const result = new Map<string, DexScreenerToken>();
  if (mints.length === 0) return result;
  try {
    const resp = await fetch(
      `https://api.dexscreener.com/tokens/v1/solana/${mints.join(',')}`,
    );
    const pairs: Array<{
      baseToken: { address: string; symbol: string; name: string };
      priceUsd?: string;
      liquidity?: { usd?: number };
    }> = await resp.json();
    if (!Array.isArray(pairs)) return result;

    for (const pair of pairs) {
      const addr = pair.baseToken?.address;
      if (!addr || result.has(addr)) continue;
      result.set(addr, {
        symbol: pair.baseToken.symbol,
        name: pair.baseToken.name,
        priceUsd: pair.priceUsd ? parseFloat(pair.priceUsd) : null,
      });
    }
  } catch { /* ignore */ }
  return result;
}

async function fetchTokenPrices(mints: string[]): Promise<Map<string, number>> {
  const prices = new Map<string, number>();
  if (mints.length === 0) return prices;
  try {
    const ids = mints.join(',');
    const resp = await fetch(`${JUP_BASE}/price/v3?ids=${ids}`, { headers: jupHeaders() });
    const data: Record<string, { usdPrice: number }> = await resp.json();
    for (const [mint, info] of Object.entries(data)) {
      if (info?.usdPrice) prices.set(mint, info.usdPrice);
    }
  } catch { /* ignore */ }
  return prices;
}

export async function getTokenHoldings(
  publicKey: PublicKey,
  connection: Connection,
): Promise<TokenHolding[]> {
  try {
    const resp = await connection.getParsedTokenAccountsByOwner(publicKey, {
      programId: TOKEN_PROGRAM_ID,
    });

    const rawTokens = resp.value
      .map((item) => {
        const info = item.account.data.parsed?.info;
        if (!info) return null;
        const amount = info.tokenAmount?.uiAmount ?? 0;
        if (amount === 0) return null;
        return {
          mint: info.mint as string,
          amount: amount as number,
          decimals: (info.tokenAmount?.decimals ?? 0) as number,
        };
      })
      .filter((t): t is { mint: string; amount: number; decimals: number } => t !== null);

    if (rawTokens.length === 0) return [];

    const mints = rawTokens.map((t) => t.mint);
    const [metaMap, priceMap] = await Promise.all([
      fetchTokenMetaMap(),
      fetchTokenPrices(mints),
    ]);

    const unknownMints = mints.filter((m) => !metaMap.has(m));
    const dexMap = unknownMints.length > 0
      ? await fetchDexScreenerMeta(unknownMints)
      : new Map<string, DexScreenerToken>();

    return rawTokens.map((t) => {
      const jupMeta = metaMap.get(t.mint);
      const dexMeta = dexMap.get(t.mint);

      const symbol = jupMeta?.symbol ?? dexMeta?.symbol ?? 'UNKNOWN';
      const name = jupMeta?.name ?? dexMeta?.name ?? t.mint.slice(0, 8) + '...';
      const usdPrice = priceMap.get(t.mint) ?? dexMeta?.priceUsd ?? null;

      return {
        mint: t.mint,
        symbol,
        name,
        amount: t.amount,
        decimals: t.decimals,
        usdPrice,
        usdValue: usdPrice !== null ? t.amount * usdPrice : null,
      };
    });
  } catch {
    return [];
  }
}

export async function getSOLPrice(): Promise<number | null> {
  try {
    const resp = await fetch(`${JUP_BASE}/price/v3?ids=So11111111111111111111111111111111111111112`, {
      headers: jupHeaders(),
    });
    const data: Record<string, { usdPrice: number }> = await resp.json();
    return data['So11111111111111111111111111111111111111112']?.usdPrice ?? null;
  } catch {
    return null;
  }
}

export function isValidAddress(address: string): boolean {
  try {
    new PublicKey(address);
    return true;
  } catch {
    return false;
  }
}

export function truncateAddress(address: string, chars = 4): string {
  return `${address.slice(0, chars)}...${address.slice(-chars)}`;
}
