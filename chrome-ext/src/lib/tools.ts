import {
  Connection,
  Keypair,
  VersionedTransaction,
} from '@solana/web3.js';
import bs58 from 'bs58';
import { RPC_ENDPOINTS, JUPITER_API_KEY } from '../types';
import { getStorageValue, StorageKeys } from './storage';

const SOL_MINT = 'So11111111111111111111111111111111111111112';
const JUP_BASE = 'https://api.jup.ag';

function jupHeaders(): Record<string, string> {
  const h: Record<string, string> = {};
  if (JUPITER_API_KEY) h['x-api-key'] = JUPITER_API_KEY;
  return h;
}

let _walletPrivateKey: string | null = null;

export function setWalletKey(key: string | null) {
  _walletPrivateKey = key;
}

function getKeypair(): Keypair {
  if (!_walletPrivateKey) throw new Error('Wallet not unlocked');
  return Keypair.fromSecretKey(bs58.decode(_walletPrivateKey));
}

export interface ToolDefinition {
  type: 'function';
  function: {
    name: string;
    description: string;
    parameters: Record<string, unknown>;
  };
}

export const TOOL_DEFINITIONS: ToolDefinition[] = [
  {
    type: 'function',
    function: {
      name: 'get_swap_quote',
      description: 'Get a swap quote from Jupiter DEX aggregator. Returns output amount, price impact, route, and fees for a token swap on Solana.',
      parameters: {
        type: 'object',
        properties: {
          inputMint: { type: 'string', description: 'Input token mint address. Use "So11111111111111111111111111111111111111112" for SOL.' },
          outputMint: { type: 'string', description: 'Output token mint address. Use "So11111111111111111111111111111111111111112" for SOL.' },
          amount: { type: 'number', description: 'Amount of input token in human-readable units (e.g. 1.5 for 1.5 SOL)' },
          inputDecimals: { type: 'number', description: 'Decimals of input token (6 for USDC, 9 for SOL). Default 9.' },
          slippageBps: { type: 'number', description: 'Max slippage in basis points. Default 50 (0.5%).' },
        },
        required: ['inputMint', 'outputMint', 'amount'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'check_token_safety',
      description: 'Run a safety/rug check on a Solana token. Returns risk score, mint authority status, freeze authority, top holder concentration, LP status, and specific risk warnings.',
      parameters: {
        type: 'object',
        properties: {
          mint: { type: 'string', description: 'Token mint address to check' },
        },
        required: ['mint'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_token_profile',
      description: 'Get full token profile: price, 5m/1h/6h/24h price changes, 24h volume, liquidity, FDV, market cap. Uses DexScreener data.',
      parameters: {
        type: 'object',
        properties: {
          mint: { type: 'string', description: 'Token mint address' },
        },
        required: ['mint'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_trending_tokens',
      description: 'Get currently trending/hot tokens on Solana DEXes. Returns tokens ranked by recent momentum and trading activity.',
      parameters: {
        type: 'object',
        properties: {
          limit: { type: 'number', description: 'Number of tokens to return. Default 10.' },
        },
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_new_pairs',
      description: 'Get newly created trading pairs/token launches on Solana in the last few hours. Useful for finding new launches early.',
      parameters: {
        type: 'object',
        properties: {
          limit: { type: 'number', description: 'Number of pairs to return. Default 10.' },
        },
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_top_holders',
      description: 'Get the top 20 largest holders of a Solana token with their percentage of total supply. Useful for checking whale concentration.',
      parameters: {
        type: 'object',
        properties: {
          mint: { type: 'string', description: 'Token mint address' },
        },
        required: ['mint'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_wallet_portfolio',
      description: 'Scan any Solana wallet address and return all token holdings with names, amounts, and USD values.',
      parameters: {
        type: 'object',
        properties: {
          walletAddress: { type: 'string', description: 'Solana wallet address to scan' },
        },
        required: ['walletAddress'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_transaction_history',
      description: 'Get recent transactions for a Solana wallet. Returns the last N transactions with type, amounts, and timestamps.',
      parameters: {
        type: 'object',
        properties: {
          walletAddress: { type: 'string', description: 'Solana wallet address' },
          limit: { type: 'number', description: 'Number of transactions. Default 10, max 20.' },
        },
        required: ['walletAddress'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'search_token',
      description: 'Search for a Solana token by name or ticker symbol. Returns matching tokens with mint address, price, liquidity, and 24h volume.',
      parameters: {
        type: 'object',
        properties: {
          query: { type: 'string', description: 'Token name or symbol to search for (e.g. "BONK", "Jupiter", "dogwifhat")' },
        },
        required: ['query'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'execute_swap',
      description: 'Execute a token swap on Jupiter DEX. IMPORTANT: Always call get_swap_quote first and show the user the quote details. Only call execute_swap after the user explicitly confirms they want to proceed. This spends real money.',
      parameters: {
        type: 'object',
        properties: {
          inputMint: { type: 'string', description: 'Input token mint address. Use "So11111111111111111111111111111111111111112" for SOL.' },
          outputMint: { type: 'string', description: 'Output token mint address. Use "So11111111111111111111111111111111111111112" for SOL.' },
          amount: { type: 'number', description: 'Amount of input token in human-readable units (e.g. 1.5 for 1.5 SOL)' },
          inputDecimals: { type: 'number', description: 'Decimals of input token (6 for USDC, 9 for SOL). Default 9.' },
          slippageBps: { type: 'number', description: 'Max slippage in basis points. Default 50 (0.5%).' },
        },
        required: ['inputMint', 'outputMint', 'amount'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_price_alert_check',
      description: 'Get the current price of one or more tokens. Provide mint addresses or well-known symbols. Returns current USD price for each.',
      parameters: {
        type: 'object',
        properties: {
          mints: {
            type: 'array',
            items: { type: 'string' },
            description: 'Array of token mint addresses. Use "So11111111111111111111111111111111111111112" for SOL, "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v" for USDC.',
          },
        },
        required: ['mints'],
      },
    },
  },
];

async function getRpcUrl(): Promise<string> {
  const custom = await getStorageValue<string>(StorageKeys.CUSTOM_RPC);
  return custom || RPC_ENDPOINTS['mainnet-beta'];
}

async function rpcCall(method: string, params: unknown[]): Promise<unknown> {
  const rpcUrl = await getRpcUrl();
  const resp = await fetch(rpcUrl, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', id: 1, method, params }),
  });
  const data = await resp.json();
  if (data.error) throw new Error(data.error.message);
  return data.result;
}

// ─── Tool Implementations ──────────────────────────────────────────

async function getSwapQuote(args: Record<string, unknown>): Promise<string> {
  const inputMint = args.inputMint as string;
  const outputMint = args.outputMint as string;
  const amount = args.amount as number;
  const inputDecimals = (args.inputDecimals as number) ?? 9;
  const slippageBps = (args.slippageBps as number) ?? 50;

  const rawAmount = Math.round(amount * 10 ** inputDecimals);
  const params = new URLSearchParams({
    inputMint,
    outputMint,
    amount: rawAmount.toString(),
    slippageBps: slippageBps.toString(),
  });

  const resp = await fetch(`${JUP_BASE}/swap/v2/order?${params}`, {
    headers: jupHeaders(),
  });
  if (!resp.ok) throw new Error(`Jupiter quote failed: ${resp.status} ${await resp.text()}`);
  const data = await resp.json();

  const outDecimals = outputMint === SOL_MINT ? 9 : 6;
  const outAmount = parseInt(data.outAmount) / 10 ** outDecimals;

  return JSON.stringify({
    inputAmount: amount,
    inputMint,
    outputAmount: outAmount,
    outputMint,
    router: data.router,
    feeBps: data.feeBps,
    slippageBps,
  });
}

async function checkTokenSafety(args: Record<string, unknown>): Promise<string> {
  const mint = args.mint as string;
  const resp = await fetch(`https://api.rugcheck.xyz/v1/tokens/${mint}/report/summary`);
  if (!resp.ok) {
    const full = await fetch(`https://api.rugcheck.xyz/v1/tokens/${mint}/report`);
    if (!full.ok) throw new Error(`RugCheck failed: ${full.status}`);
    const data = await full.json();
    return JSON.stringify({
      mint,
      score: data.score,
      risks: data.risks?.map((r: any) => ({ name: r.name, level: r.level, description: r.description })) ?? [],
      topHolders: data.topHolders?.slice(0, 10).map((h: any) => ({ address: h.address, pct: h.pct })) ?? [],
      mintAuthority: data.token?.mintAuthority ?? null,
      freezeAuthority: data.token?.freezeAuthority ?? null,
      isToken2022: data.token?.isToken2022 ?? false,
      totalMarketLiquidity: data.totalMarketLiquidity,
    });
  }
  const data = await resp.json();
  return JSON.stringify(data);
}

async function getTokenProfile(args: Record<string, unknown>): Promise<string> {
  const mint = args.mint as string;
  const resp = await fetch(`https://api.dexscreener.com/tokens/v1/solana/${mint}`);
  if (!resp.ok) throw new Error(`DexScreener failed: ${resp.status}`);
  const pairs: any[] = await resp.json();
  if (!Array.isArray(pairs) || pairs.length === 0) return JSON.stringify({ error: 'No trading pairs found for this token' });

  const top = pairs.sort((a: any, b: any) => (b.liquidity?.usd ?? 0) - (a.liquidity?.usd ?? 0))[0];
  return JSON.stringify({
    name: top.baseToken?.name,
    symbol: top.baseToken?.symbol,
    mint,
    priceUsd: top.priceUsd,
    priceChange: {
      m5: top.priceChange?.m5,
      h1: top.priceChange?.h1,
      h6: top.priceChange?.h6,
      h24: top.priceChange?.h24,
    },
    volume24h: top.volume?.h24,
    liquidity: top.liquidity?.usd,
    fdv: top.fdv,
    marketCap: top.marketCap,
    pairAddress: top.pairAddress,
    dexId: top.dexId,
    pairCreatedAt: top.pairCreatedAt,
    totalPairs: pairs.length,
    url: top.url,
  });
}

async function getTrendingTokens(args: Record<string, unknown>): Promise<string> {
  const limit = Math.min((args.limit as number) ?? 10, 20);
  const resp = await fetch('https://api.dexscreener.com/token-boosts/top/v1');
  if (!resp.ok) throw new Error(`DexScreener trending failed: ${resp.status}`);
  const data: any[] = await resp.json();

  const solanaTokens = data
    .filter((t: any) => t.chainId === 'solana')
    .slice(0, limit)
    .map((t: any, i: number) => ({
      rank: i + 1,
      name: t.description || t.tokenAddress,
      mint: t.tokenAddress,
      url: t.url,
      totalAmount: t.totalAmount,
    }));

  if (solanaTokens.length === 0) {
    return JSON.stringify({ message: 'No trending Solana tokens found. Fetching all chains.', tokens: data.slice(0, limit) });
  }

  const mints = solanaTokens.map((t: any) => t.mint).join(',');
  let priceData: Record<string, any> = {};
  try {
    const priceResp = await fetch(`${JUP_BASE}/price/v3?ids=${mints}`, { headers: jupHeaders() });
    priceData = await priceResp.json();
  } catch { /* ignore */ }

  for (const t of solanaTokens) {
    const p = priceData[t.mint];
    if (p?.usdPrice) (t as any).priceUsd = p.usdPrice;
  }

  return JSON.stringify({ tokens: solanaTokens });
}

async function getNewPairs(args: Record<string, unknown>): Promise<string> {
  const limit = Math.min((args.limit as number) ?? 10, 20);
  const resp = await fetch('https://api.dexscreener.com/latest/dex/pairs/solana');
  if (!resp.ok) throw new Error(`DexScreener new pairs failed: ${resp.status}`);
  const data = await resp.json();
  const pairs = (data.pairs ?? [])
    .sort((a: any, b: any) => (b.pairCreatedAt ?? 0) - (a.pairCreatedAt ?? 0))
    .slice(0, limit)
    .map((p: any) => ({
      name: p.baseToken?.name,
      symbol: p.baseToken?.symbol,
      mint: p.baseToken?.address,
      priceUsd: p.priceUsd,
      volume24h: p.volume?.h24,
      liquidity: p.liquidity?.usd,
      priceChange24h: p.priceChange?.h24,
      pairCreatedAt: p.pairCreatedAt ? new Date(p.pairCreatedAt).toISOString() : null,
      dexId: p.dexId,
      url: p.url,
    }));

  return JSON.stringify({ pairs });
}

async function getTopHolders(args: Record<string, unknown>): Promise<string> {
  const mint = args.mint as string;
  const result: any = await rpcCall('getTokenLargestAccounts', [mint]);
  const accounts = result?.value ?? [];

  const supplyResult: any = await rpcCall('getTokenSupply', [mint]);
  const totalSupply = parseFloat(supplyResult?.value?.uiAmountString ?? '0');

  const holders = accounts.slice(0, 20).map((a: any, i: number) => ({
    rank: i + 1,
    address: a.address,
    amount: parseFloat(a.uiAmountString ?? '0'),
    pct: totalSupply > 0 ? ((parseFloat(a.uiAmountString ?? '0') / totalSupply) * 100).toFixed(2) + '%' : 'N/A',
  }));

  const top10Pct = totalSupply > 0
    ? holders.slice(0, 10).reduce((sum: number, h: any) => sum + h.amount, 0) / totalSupply * 100
    : 0;

  return JSON.stringify({
    mint,
    totalSupply,
    top10HolderConcentration: top10Pct.toFixed(2) + '%',
    holders,
  });
}

async function getWalletPortfolio(args: Record<string, unknown>): Promise<string> {
  const wallet = args.walletAddress as string;

  const solResult: any = await rpcCall('getBalance', [wallet]);
  const solBalance = (solResult?.value ?? 0) / 1e9;

  const tokenResult: any = await rpcCall('getTokenAccountsByOwner', [
    wallet,
    { programId: 'TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA' },
    { encoding: 'jsonParsed' },
  ]);

  const tokens = (tokenResult?.value ?? [])
    .map((item: any) => {
      const info = item.account?.data?.parsed?.info;
      if (!info) return null;
      const amount = info.tokenAmount?.uiAmount ?? 0;
      if (amount === 0) return null;
      return { mint: info.mint, amount, decimals: info.tokenAmount?.decimals ?? 0 };
    })
    .filter(Boolean);

  const allMints = [SOL_MINT, ...tokens.map((t: any) => t.mint)];
  let prices: Record<string, any> = {};
  try {
    const priceResp = await fetch(`${JUP_BASE}/price/v3?ids=${allMints.join(',')}`, { headers: jupHeaders() });
    prices = await priceResp.json();
  } catch { /* ignore */ }

  let metaMap: Record<string, any> = {};
  try {
    const metaResp = await fetch(`${JUP_BASE}/tokens/v2/tag?query=verified`, { headers: jupHeaders() });
    const metaList: any[] = await metaResp.json();
    for (const m of metaList) metaMap[m.id ?? m.address] = m;
  } catch { /* ignore */ }

  const solPrice = prices[SOL_MINT]?.usdPrice ?? null;
  const holdings = tokens.map((t: any) => {
    const meta = metaMap[t.mint];
    const price = prices[t.mint]?.usdPrice ?? null;
    return {
      symbol: meta?.symbol ?? 'UNKNOWN',
      name: meta?.name ?? t.mint.slice(0, 8) + '...',
      mint: t.mint,
      amount: t.amount,
      priceUsd: price,
      valueUsd: price !== null ? t.amount * price : null,
    };
  });

  let totalValue = solPrice ? solBalance * solPrice : 0;
  for (const h of holdings) if (h.valueUsd) totalValue += h.valueUsd;

  return JSON.stringify({
    wallet,
    solBalance,
    solPriceUsd: solPrice,
    solValueUsd: solPrice ? solBalance * solPrice : null,
    tokens: holdings.sort((a: any, b: any) => (b.valueUsd ?? 0) - (a.valueUsd ?? 0)),
    estimatedTotalValueUsd: totalValue,
  });
}

async function getTransactionHistory(args: Record<string, unknown>): Promise<string> {
  const wallet = args.walletAddress as string;
  const limit = Math.min((args.limit as number) ?? 10, 20);

  const sigs: any = await rpcCall('getSignaturesForAddress', [wallet, { limit }]);
  if (!sigs || sigs.length === 0) return JSON.stringify({ wallet, transactions: [] });

  const signatures = sigs.map((s: any) => s.signature);
  const txs: any = await rpcCall('getTransactions', [
    signatures,
    { encoding: 'jsonParsed', maxSupportedTransactionVersion: 0 },
  ]);

  const transactions = (txs ?? []).map((tx: any, i: number) => {
    if (!tx) return { signature: signatures[i], status: 'not found' };

    const meta = tx.meta;
    const blockTime = tx.blockTime ? new Date(tx.blockTime * 1000).toISOString() : null;
    const err = meta?.err ? JSON.stringify(meta.err) : null;
    const fee = meta?.fee ? meta.fee / 1e9 : 0;

    const instructions = tx.transaction?.message?.instructions ?? [];
    const types = instructions
      .map((ix: any) => ix.parsed?.type || ix.programId)
      .filter(Boolean)
      .slice(0, 5);

    const preBalances = meta?.preBalances ?? [];
    const postBalances = meta?.postBalances ?? [];
    const solChange = preBalances.length > 0 && postBalances.length > 0
      ? (postBalances[0] - preBalances[0]) / 1e9
      : null;

    return {
      signature: signatures[i],
      blockTime,
      status: err ? `failed: ${err}` : 'success',
      fee: fee.toFixed(6) + ' SOL',
      solChange: solChange !== null ? solChange.toFixed(6) + ' SOL' : null,
      instructionTypes: types,
    };
  });

  return JSON.stringify({ wallet, transactions });
}

async function searchToken(args: Record<string, unknown>): Promise<string> {
  const query = args.query as string;
  const resp = await fetch(`https://api.dexscreener.com/latest/dex/search?q=${encodeURIComponent(query)}`);
  if (!resp.ok) throw new Error(`DexScreener search failed: ${resp.status}`);
  const data = await resp.json();

  const solPairs = (data.pairs ?? [])
    .filter((p: any) => p.chainId === 'solana')
    .slice(0, 10);

  const seen = new Set<string>();
  const results = [];
  for (const p of solPairs) {
    const mint = p.baseToken?.address;
    if (!mint || seen.has(mint)) continue;
    seen.add(mint);
    results.push({
      name: p.baseToken?.name,
      symbol: p.baseToken?.symbol,
      mint,
      priceUsd: p.priceUsd,
      volume24h: p.volume?.h24,
      liquidity: p.liquidity?.usd,
      priceChange24h: p.priceChange?.h24,
      fdv: p.fdv,
      url: p.url,
    });
  }

  return JSON.stringify({ query, results });
}

async function getPriceAlertCheck(args: Record<string, unknown>): Promise<string> {
  const mints = args.mints as string[];
  const ids = mints.join(',');
  const resp = await fetch(`${JUP_BASE}/price/v3?ids=${ids}`, { headers: jupHeaders() });
  if (!resp.ok) throw new Error(`Jupiter price failed: ${resp.status}`);
  const data = await resp.json();

  const prices: Record<string, unknown>[] = [];
  for (const mint of mints) {
    const info = data[mint];
    prices.push({
      mint,
      priceUsd: info?.usdPrice ?? null,
    });
  }

  return JSON.stringify({ prices });
}

async function executeSwap(args: Record<string, unknown>): Promise<string> {
  const inputMint = args.inputMint as string;
  const outputMint = args.outputMint as string;
  const amount = args.amount as number;
  const inputDecimals = (args.inputDecimals as number) ?? 9;
  const slippageBps = (args.slippageBps as number) ?? 50;

  const keypair = getKeypair();
  const taker = keypair.publicKey.toBase58();
  const rawAmount = Math.round(amount * 10 ** inputDecimals);

  const orderParams = new URLSearchParams({
    inputMint,
    outputMint,
    amount: rawAmount.toString(),
    slippageBps: slippageBps.toString(),
    taker,
  });

  const orderResp = await fetch(`${JUP_BASE}/swap/v2/order?${orderParams}`, {
    headers: jupHeaders(),
  });
  if (!orderResp.ok) {
    const errBody = await orderResp.text();
    throw new Error(`Jupiter /order failed: ${orderResp.status} — ${errBody}`);
  }
  const order = await orderResp.json();

  if (!order.transaction) throw new Error('No transaction returned from Jupiter /order');

  const txBuf = Buffer.from(order.transaction, 'base64');
  const transaction = VersionedTransaction.deserialize(txBuf);
  transaction.sign([keypair]);

  const signedTxBase64 = Buffer.from(transaction.serialize()).toString('base64');

  const execResp = await fetch(`${JUP_BASE}/swap/v2/execute`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', ...jupHeaders() },
    body: JSON.stringify({
      signedTransaction: signedTxBase64,
      requestId: order.requestId,
    }),
  });
  if (!execResp.ok) {
    const errBody = await execResp.text();
    throw new Error(`Jupiter /execute failed: ${execResp.status} — ${errBody}`);
  }
  const result = await execResp.json();

  if (result.status !== 'Success') {
    return JSON.stringify({
      status: 'failed',
      signature: result.signature,
      error: result.error ?? `code ${result.code}`,
    });
  }

  const outDecimals = outputMint === SOL_MINT ? 9 : 6;
  return JSON.stringify({
    status: 'success',
    signature: result.signature,
    explorerUrl: `https://solscan.io/tx/${result.signature}`,
    inputMint,
    outputMint,
    inputAmount: amount,
    outputAmount: parseInt(result.outputAmountResult || order.outAmount) / 10 ** outDecimals,
  });
}

// ─── Executor ──────────────────────────────────────────────────────

const EXECUTORS: Record<string, (args: Record<string, unknown>) => Promise<string>> = {
  get_swap_quote: getSwapQuote,
  check_token_safety: checkTokenSafety,
  get_token_profile: getTokenProfile,
  get_trending_tokens: getTrendingTokens,
  get_new_pairs: getNewPairs,
  get_top_holders: getTopHolders,
  get_wallet_portfolio: getWalletPortfolio,
  get_transaction_history: getTransactionHistory,
  search_token: searchToken,
  get_price_alert_check: getPriceAlertCheck,
  execute_swap: executeSwap,
};

export async function executeTool(name: string, args: Record<string, unknown>): Promise<string> {
  const executor = EXECUTORS[name];
  if (!executor) return JSON.stringify({ error: `Unknown tool: ${name}` });
  try {
    return await executor(args);
  } catch (e: any) {
    return JSON.stringify({ error: e.message || 'Tool execution failed' });
  }
}
