export interface EncryptedWallet {
  encryptedKey: string;
  iv: string;
  salt: string;
}

export interface ChatMessage {
  role: 'system' | 'user' | 'assistant';
  content: string;
}

export interface Message extends ChatMessage {
  id: string;
  cost?: number;
  timestamp: number;
}

export type Network = 'mainnet-beta' | 'devnet';

export interface ModelInfo {
  id: string;
  name: string;
  provider: string;
  inputPrice: number;
  outputPrice: number;
}

export const MODELS: ModelInfo[] = [
  // Free / Ultra-cheap
  { id: 'deepseek/deepseek-chat', name: 'DeepSeek V3', provider: 'DeepSeek', inputPrice: 0.28, outputPrice: 0.42 },
  { id: 'openai/gpt-4o-mini', name: 'GPT-4o Mini', provider: 'OpenAI', inputPrice: 0.15, outputPrice: 0.60 },
  { id: 'openai/gpt-5.4-nano', name: 'GPT-5.4 Nano', provider: 'OpenAI', inputPrice: 0.20, outputPrice: 1.25 },
  { id: 'xai/grok-4-fast-reasoning', name: 'Grok 4 Fast', provider: 'xAI', inputPrice: 0.20, outputPrice: 0.50 },
  { id: 'google/gemini-2.5-flash', name: 'Gemini 2.5 Flash', provider: 'Google', inputPrice: 0.30, outputPrice: 2.50 },
  { id: 'xai/grok-3-mini', name: 'Grok 3 Mini', provider: 'xAI', inputPrice: 0.30, outputPrice: 0.50 },
  // Mid-tier
  { id: 'openai/gpt-5.4-mini', name: 'GPT-5.4 Mini', provider: 'OpenAI', inputPrice: 0.75, outputPrice: 4.50 },
  { id: 'nvidia/kimi-k2.5', name: 'Kimi K2.5', provider: 'Moonshot', inputPrice: 0.60, outputPrice: 3.00 },
  { id: 'anthropic/claude-haiku-4.5', name: 'Claude Haiku 4.5', provider: 'Anthropic', inputPrice: 1.00, outputPrice: 5.00 },
  { id: 'deepseek/deepseek-reasoner', name: 'DeepSeek Reasoner', provider: 'DeepSeek', inputPrice: 0.28, outputPrice: 0.42 },
  { id: 'google/gemini-2.5-pro', name: 'Gemini 2.5 Pro', provider: 'Google', inputPrice: 1.25, outputPrice: 10.00 },
  // Premium
  { id: 'openai/gpt-5.4', name: 'GPT-5.4', provider: 'OpenAI', inputPrice: 2.50, outputPrice: 15.00 },
  { id: 'openai/gpt-4o', name: 'GPT-4o', provider: 'OpenAI', inputPrice: 2.50, outputPrice: 10.00 },
  { id: 'anthropic/claude-sonnet-4.6', name: 'Claude Sonnet 4.6', provider: 'Anthropic', inputPrice: 3.00, outputPrice: 15.00 },
  { id: 'xai/grok-3', name: 'Grok 3', provider: 'xAI', inputPrice: 3.00, outputPrice: 15.00 },
  { id: 'google/gemini-3.1-pro', name: 'Gemini 3.1 Pro', provider: 'Google', inputPrice: 2.00, outputPrice: 12.00 },
  { id: 'anthropic/claude-opus-4.6', name: 'Claude Opus 4.6', provider: 'Anthropic', inputPrice: 5.00, outputPrice: 25.00 },
];

export const USDC_MINT_MAINNET = 'EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v';
export const USDC_MINT_DEVNET = '4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU';
export const USDC_DECIMALS = 6;

const HELIUS_KEY = import.meta.env.VITE_HELIUS_KEY ?? '';
export const JUPITER_API_KEY = import.meta.env.VITE_JUPITER_API_KEY ?? '';

export const RPC_ENDPOINTS: Record<Network, string> = {
  'mainnet-beta': HELIUS_KEY
    ? `https://mainnet.helius-rpc.com/?api-key=${HELIUS_KEY}`
    : 'https://api.mainnet-beta.solana.com',
  devnet: HELIUS_KEY
    ? `https://devnet.helius-rpc.com/?api-key=${HELIUS_KEY}`
    : 'https://api.devnet.solana.com',
};
