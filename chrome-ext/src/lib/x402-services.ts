import type { ToolDefinition } from './tools';
import { x402Fetch } from './x402-fetch';
import { getStorageValue, setStorageValue, StorageKeys } from './storage';

export interface ServiceParam {
  name: string;
  label: string;
  type: 'string' | 'number';
  required?: boolean;
  description: string;
}

export interface ServiceEndpoint {
  id: string;
  name: string;
  toolDescription: string;
  method: 'GET' | 'POST';
  path: string;
  params: ServiceParam[];
  bodyTemplate?: 'json-fields';
  wrapInput?: boolean;
}

export interface X402Service {
  id: string;
  name: string;
  description: string;
  category: X402Category;
  baseUrl: string;
  priceRange: string;
  status: 'live' | 'beta' | 'coming-soon';
  endpoints: ServiceEndpoint[];
  isCustom?: boolean;
}

export type X402Category =
  | 'ai'
  | 'crypto-data'
  | 'defi'
  | 'security'
  | 'infra'
  | 'agent'
  | 'data';

export const CATEGORY_META: Record<X402Category, { label: string; color: string }> = {
  ai:            { label: 'AI / ML',      color: '#a78bfa' },
  'crypto-data': { label: 'Crypto Data',  color: '#60a5fa' },
  defi:          { label: 'DeFi',         color: '#34d399' },
  security:      { label: 'Security',     color: '#f87171' },
  infra:         { label: 'Infra',        color: '#fbbf24' },
  agent:         { label: 'Agents',       color: '#f472b6' },
  data:          { label: 'Data',         color: '#38bdf8' },
};

const SERVICES: X402Service[] = [
  {
    id: 'solsignal',
    name: 'SolSignal',
    description: 'Solana token safety scanner — DexScreener, RugCheck, GoPlus & Jupiter simulation into one SAFE/AVOID verdict.',
    category: 'security',
    baseUrl: 'https://solsignal-api.onrender.com',
    priceRange: '$0.01',
    status: 'live',
    endpoints: [
      {
        id: 'scan',
        name: 'Token Safety Scan',
        toolDescription: 'Scan a Solana token for rug pull risk, liquidity issues, and safety signals using SolSignal (x402 paid). Returns a SAFE/AVOID verdict with detailed risk breakdown.',
        method: 'GET',
        path: '/api/scan',
        params: [
          { name: 'token', label: 'Token Mint', type: 'string', required: true, description: 'Solana token mint address to scan' },
        ],
      },
    ],
  },
  {
    id: 'sentinel',
    name: 'Sentinel',
    description: 'Trust verification — protocol trust scoring, token safety, DeFi risk, OFAC screening.',
    category: 'security',
    baseUrl: 'https://sentinel-awms.onrender.com',
    priceRange: '$0.005 – $0.025',
    status: 'live',
    endpoints: [
      {
        id: 'trust-score',
        name: 'Trust Score',
        toolDescription: 'Get a trust/safety score for an address or protocol using Sentinel (x402 paid). Returns risk level, trust score, and flags.',
        method: 'GET',
        path: '/api/trust-score',
        params: [
          { name: 'address', label: 'Address', type: 'string', required: true, description: 'Wallet or contract address to score' },
        ],
      },
      {
        id: 'ofac-check',
        name: 'OFAC Screening',
        toolDescription: 'Screen a blockchain address against OFAC sanctions list using Sentinel (x402 paid).',
        method: 'GET',
        path: '/api/ofac-check',
        params: [
          { name: 'address', label: 'Address', type: 'string', required: true, description: 'Address to screen against OFAC list' },
        ],
      },
    ],
  },
  {
    id: 'cybera',
    name: 'CYBERA Compliance',
    description: 'VASP address ID (20K+ addresses, 29 chains), risk scoring, sanctions & mixer screening.',
    category: 'security',
    baseUrl: 'https://compliance-api-ruddy.vercel.app',
    priceRange: '$0.01',
    status: 'live',
    endpoints: [
      {
        id: 'identify',
        name: 'Address Identification',
        toolDescription: 'Identify the VASP or entity behind a blockchain address using CYBERA (x402 paid). Covers 20K+ addresses across 29 chains.',
        method: 'GET',
        path: '/api/identify',
        params: [
          { name: 'address', label: 'Address', type: 'string', required: true, description: 'Blockchain address to identify' },
          { name: 'chain', label: 'Chain', type: 'string', required: false, description: 'Chain name (solana, ethereum, bitcoin). Defaults to solana.' },
        ],
      },
    ],
  },
  {
    id: 'defi-signal',
    name: 'DeFi Signal',
    description: 'New pool risk scoring (0–10), Solana whale alerts (>$100K), and Dune-enriched on-chain intel.',
    category: 'defi',
    baseUrl: 'https://defi-signal-agent-production.up.railway.app',
    priceRange: '$0.01 – $0.10',
    status: 'live',
    endpoints: [
      {
        id: 'pool-risk',
        name: 'Pool Risk Score',
        toolDescription: 'Get risk score (0–10) for a DeFi pool using DeFi Signal (x402 paid). Higher score = higher risk.',
        method: 'GET',
        path: '/api/pool-risk',
        params: [
          { name: 'pool', label: 'Pool Address', type: 'string', required: true, description: 'Pool contract address to score' },
        ],
      },
      {
        id: 'whale-alerts',
        name: 'Whale Alerts',
        toolDescription: 'Get recent Solana whale movements (>$100K) using DeFi Signal (x402 paid).',
        method: 'GET',
        path: '/api/whale-alerts',
        params: [],
      },
    ],
  },
  {
    id: 'deepblue',
    name: 'DeepBlue Trading',
    description: 'AI crypto intelligence — live signals, prediction analytics, whale tracking.',
    category: 'defi',
    baseUrl: 'https://api.deepbluebase.xyz',
    priceRange: '$0.01 – $0.05',
    status: 'live',
    endpoints: [
      {
        id: 'signals',
        name: 'Trading Signals',
        toolDescription: 'Get AI-generated trading signals for a crypto token using DeepBlue (x402 paid).',
        method: 'GET',
        path: '/api/signals',
        params: [
          { name: 'token', label: 'Token Symbol', type: 'string', required: false, description: 'Token symbol (e.g. SOL, BTC)' },
        ],
      },
      {
        id: 'whales',
        name: 'Whale Tracking',
        toolDescription: 'Track whale wallet movements and large transactions using DeepBlue (x402 paid).',
        method: 'GET',
        path: '/api/whales',
        params: [],
      },
    ],
  },
  {
    id: 'moonmaker',
    name: 'MoonMaker',
    description: 'AI-native crypto data — signals, market context, DeFi regime detection, ETF flows, DEX alpha.',
    category: 'defi',
    baseUrl: 'https://api.moonmaker.cc',
    priceRange: '$0.02 – $0.10',
    status: 'live',
    endpoints: [
      {
        id: 'market-context',
        name: 'Market Context',
        toolDescription: 'Get current market regime and directional signals using MoonMaker (x402 paid).',
        method: 'GET',
        path: '/api/market-context',
        params: [],
      },
      {
        id: 'dex-alpha',
        name: 'DEX Alpha',
        toolDescription: 'Get DEX trading alpha signals and opportunities using MoonMaker (x402 paid).',
        method: 'GET',
        path: '/api/dex-alpha',
        params: [],
      },
    ],
  },
  {
    id: 'kerdos',
    name: 'Kerdos Market Intel',
    description: 'Crypto sentiment scoring, BTC/ETH regime direction, funding rates, liquidation risk.',
    category: 'defi',
    baseUrl: 'https://nonvisceral-eloisa-mousily.ngrok-free.dev',
    priceRange: '$0.01 – $0.05',
    status: 'live',
    endpoints: [
      {
        id: 'sentiment',
        name: 'Sentiment Score',
        toolDescription: 'Get crypto market sentiment and fear/greed analysis using Kerdos (x402 paid).',
        method: 'GET',
        path: '/api/sentiment',
        params: [],
      },
      {
        id: 'funding-rates',
        name: 'Funding Rates',
        toolDescription: 'Get current funding rates across major exchanges using Kerdos (x402 paid).',
        method: 'GET',
        path: '/api/funding-rates',
        params: [],
      },
    ],
  },
  {
    id: 'xquik',
    name: 'Xquik',
    description: 'Real-time X (Twitter) data API — tweet search, user profiles, trends.',
    category: 'data',
    baseUrl: 'https://xquik.com',
    priceRange: '$0.001 – $0.01',
    status: 'live',
    endpoints: [
      {
        id: 'search',
        name: 'Tweet Search',
        toolDescription: 'Search for recent tweets matching a query using Xquik (x402 paid). Good for crypto sentiment and news.',
        method: 'GET',
        path: '/api/search',
        params: [
          { name: 'q', label: 'Query', type: 'string', required: true, description: 'Search query (e.g. $SOL, Solana)' },
          { name: 'limit', label: 'Limit', type: 'number', required: false, description: 'Max results (default 10)' },
        ],
      },
      {
        id: 'trends',
        name: 'Trending Topics',
        toolDescription: 'Get current trending topics on X/Twitter using Xquik (x402 paid).',
        method: 'GET',
        path: '/api/trends',
        params: [],
      },
    ],
  },
  {
    id: 'agentfeed',
    name: 'AgentFeed',
    description: 'Real-time structured data feeds — market data, news, and social signals.',
    category: 'data',
    baseUrl: 'https://agentfeed.io',
    priceRange: '$0.001 – $0.02',
    status: 'live',
    endpoints: [
      {
        id: 'feed',
        name: 'Data Feed',
        toolDescription: 'Get a structured data feed (market, news, or social) using AgentFeed (x402 paid).',
        method: 'GET',
        path: '/api/feed',
        params: [
          { name: 'type', label: 'Feed Type', type: 'string', required: true, description: 'Feed type: market, news, or social' },
          { name: 'token', label: 'Token', type: 'string', required: false, description: 'Token symbol filter (e.g. SOL, BTC)' },
        ],
      },
    ],
  },
  {
    id: 'gpu-bridge',
    name: 'GPU-Bridge',
    description: '30-service GPU inference — LLM, image gen, embeddings, STT, TTS, PDF processing.',
    category: 'ai',
    baseUrl: 'https://gpubridge.xyz',
    priceRange: '$0.00002 – $0.001',
    status: 'live',
    endpoints: [
      {
        id: 'inference',
        name: 'Text Generation',
        toolDescription: 'Run text generation on a GPU model via GPU-Bridge (x402 paid). Cheap inference.',
        method: 'POST',
        path: '/api/inference',
        bodyTemplate: 'json-fields',
        params: [
          { name: 'model', label: 'Model', type: 'string', required: true, description: 'Model name (e.g. llama-3.1-8b)' },
          { name: 'prompt', label: 'Prompt', type: 'string', required: true, description: 'Text prompt' },
          { name: 'max_tokens', label: 'Max Tokens', type: 'number', required: false, description: 'Max tokens (default 256)' },
        ],
      },
    ],
  },
  {
    id: 'x402engine',
    name: 'x402engine',
    description: '74 endpoints: 44 LLMs, image/video gen, crypto data, web search, code execution, TTS, IPFS.',
    category: 'infra',
    baseUrl: 'https://x402engine.app',
    priceRange: '$0.001 – $0.50',
    status: 'live',
    endpoints: [
      {
        id: 'web-search',
        name: 'Web Search',
        toolDescription: 'Search the web and get structured results using x402engine (x402 paid). Use for real-time info.',
        method: 'GET',
        path: '/api/search',
        params: [
          { name: 'q', label: 'Query', type: 'string', required: true, description: 'Search query' },
        ],
      },
    ],
  },
];

// ─── Custom services persistence ───

let _cachedAllServices: X402Service[] = [...SERVICES];

export async function loadCustomServices(): Promise<X402Service[]> {
  const stored = await getStorageValue<X402Service[]>(StorageKeys.CUSTOM_SERVICES);
  return stored || [];
}

export async function saveCustomService(service: X402Service): Promise<void> {
  const custom = await loadCustomServices();
  const existing = custom.findIndex((s) => s.id === service.id);
  if (existing >= 0) {
    custom[existing] = service;
  } else {
    custom.push(service);
  }
  await setStorageValue(StorageKeys.CUSTOM_SERVICES, custom);
  await refreshAllServices();
}

export async function removeCustomService(serviceId: string): Promise<void> {
  const custom = await loadCustomServices();
  const filtered = custom.filter((s) => s.id !== serviceId);
  await setStorageValue(StorageKeys.CUSTOM_SERVICES, filtered);
  await refreshAllServices();
}

async function refreshAllServices(): Promise<X402Service[]> {
  const custom = await loadCustomServices();
  _cachedAllServices = [...SERVICES, ...custom];
  return _cachedAllServices;
}

export function getAllServicesCached(): X402Service[] {
  return _cachedAllServices;
}

export async function fetchServices(): Promise<X402Service[]> {
  return refreshAllServices();
}

// ─── Convert enabled services into AI tool definitions + executor ───

function makeToolName(serviceId: string, endpointId: string): string {
  return `x402_${serviceId}_${endpointId}`;
}

export function servicesToToolDefs(enabledIds: string[]): ToolDefinition[] {
  const defs: ToolDefinition[] = [];
  for (const svc of _cachedAllServices) {
    if (!enabledIds.includes(svc.id)) continue;
    for (const ep of svc.endpoints) {
      const properties: Record<string, unknown> = {};
      const required: string[] = [];
      for (const p of ep.params) {
        properties[p.name] = {
          type: p.type,
          description: p.description,
        };
        if (p.required) required.push(p.name);
      }

      if (svc.isCustom && Object.keys(properties).length === 0) {
        properties['input'] = {
          type: 'string',
          description: 'Input query or parameters as a JSON string. Read the tool description to determine what to pass.',
        };
      }

      defs.push({
        type: 'function',
        function: {
          name: makeToolName(svc.id, ep.id),
          description: ep.toolDescription,
          parameters: {
            type: 'object',
            properties,
            required,
          },
        },
      });
    }
  }
  return defs;
}

export async function executeServiceTool(
  toolName: string,
  rawArgs: Record<string, unknown>,
): Promise<string> {
  const parts = toolName.replace('x402_', '').split('_');
  const endpointId = parts.pop()!;
  const serviceId = parts.join('_');

  const allServices = _cachedAllServices;
  const svc = allServices.find((s) => s.id === serviceId);
  if (!svc) return JSON.stringify({ error: `Unknown x402 service: ${serviceId}` });

  const ep = svc.endpoints.find((e) => e.id === endpointId);
  if (!ep) return JSON.stringify({ error: `Unknown endpoint: ${endpointId}` });

  let args = rawArgs;
  if (args.input && typeof args.input === 'string' && Object.keys(args).length === 1) {
    try {
      const parsed = JSON.parse(args.input);
      if (typeof parsed === 'object' && parsed !== null) args = parsed;
    } catch { /* use input as-is */ }
  }

  let url: string;
  let body: string | undefined;

  if (ep.method === 'GET') {
    const qp = new URLSearchParams();
    if (ep.params.length > 0) {
      for (const p of ep.params) {
        const val = args[p.name];
        if (val !== undefined && val !== null && val !== '') {
          qp.set(p.name, String(val));
        }
      }
    } else {
      for (const [k, v] of Object.entries(args)) {
        if (v !== undefined && v !== null && v !== '') qp.set(k, String(v));
      }
    }
    const qs = qp.toString();
    url = `${svc.baseUrl}${ep.path}${qs ? `?${qs}` : ''}`;
  } else {
    url = `${svc.baseUrl}${ep.path}`;
    if (ep.wrapInput) {
      const topLevel: Record<string, unknown> = {};
      const input: Record<string, unknown> = {};
      const topKeys = new Set(['model', 'async']);
      for (const [k, v] of Object.entries(args)) {
        if (v === undefined || v === null || v === '') continue;
        if (topKeys.has(k)) {
          topLevel[k] = v;
        } else {
          input[k] = v;
        }
      }
      topLevel.input = input;
      body = JSON.stringify(topLevel);
    } else if (ep.params.length > 0 && ep.bodyTemplate === 'json-fields') {
      const obj: Record<string, unknown> = {};
      for (const p of ep.params) {
        const val = args[p.name];
        if (val !== undefined && val !== null && val !== '') {
          obj[p.name] = p.type === 'number' ? Number(val) : val;
        }
      }
      body = JSON.stringify(obj);
    } else {
      body = JSON.stringify(args);
    }
  }

  const result = await x402Fetch(url, { method: ep.method, body });

  if (!result.ok) {
    return JSON.stringify({ error: `${svc.name} returned status ${result.status}`, data: result.data });
  }

  const response: Record<string, unknown> = {
    source: svc.name,
    costUsd: result.costUsd,
    data: result.data,
  };
  return JSON.stringify(response);
}

export function isServiceTool(name: string): boolean {
  return name.startsWith('x402_');
}

export function getEnabledServiceNames(enabledIds: string[]): string[] {
  return _cachedAllServices.filter((s) => enabledIds.includes(s.id)).map((s) => s.name);
}
