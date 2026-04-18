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

const SOLANA_API_URL = 'https://sol.blockrun.ai/api';
const SOLANA_NETWORK_ID = 'solana:5eykt4UsFv8P8NJdTREpY1vzqKqZKvdp';
const USDC_MINT = 'EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v';
const DEFAULT_MAX_TOKENS = 1024;
const DEFAULT_TIMEOUT = 60_000;
const COMPUTE_UNIT_PRICE = 1;
const COMPUTE_UNIT_LIMIT = 8_000;


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

export interface ToolCall {
  id: string;
  type: 'function';
  function: { name: string; arguments: string };
}

export interface ChatMessage {
  role: 'system' | 'user' | 'assistant' | 'tool';
  content?: string | null;
  tool_calls?: ToolCall[];
  tool_call_id?: string;
}

export interface ChatResponse {
  id: string;
  object: string;
  created: number;
  model: string;
  choices: Array<{
    index: number;
    message: ChatMessage;
    finish_reason?: string;
  }>;
  usage?: {
    prompt_tokens: number;
    completion_tokens: number;
    total_tokens: number;
  };
}

export interface SolanaLLMClientOptions {
  privateKey: string;
  apiUrl?: string;
  rpcUrl?: string;
  timeout?: number;
}

export class SolanaLLMClient {
  private privateKey: string;
  private apiUrl: string;
  private rpcUrl: string;
  private timeout: number;
  private sessionTotalUsd = 0;
  private sessionCalls = 0;
  private addressCache: string | null = null;

  constructor(options: SolanaLLMClientOptions) {
    if (!options.privateKey) {
      throw new Error('Private key required.');
    }
    this.privateKey = options.privateKey;
    this.apiUrl = (options.apiUrl || SOLANA_API_URL).replace(/\/$/, '');
    this.rpcUrl = options.rpcUrl || 'https://api.mainnet-beta.solana.com';
    this.timeout = options.timeout || DEFAULT_TIMEOUT;
  }

  async getWalletAddress(): Promise<string> {
    if (!this.addressCache) {
      const bytes = bs58.decode(this.privateKey);
      const kp = Keypair.fromSecretKey(bytes);
      this.addressCache = kp.publicKey.toBase58();
    }
    return this.addressCache;
  }

  async chat(
    model: string,
    prompt: string,
    options?: { system?: string; maxTokens?: number; temperature?: number },
  ): Promise<string> {
    const messages: ChatMessage[] = [];
    if (options?.system) messages.push({ role: 'system', content: options.system });
    messages.push({ role: 'user', content: prompt });
    const result = await this.chatCompletion(model, messages, {
      maxTokens: options?.maxTokens,
      temperature: options?.temperature,
    });
    return result.choices[0]?.message?.content || '';
  }

  async chatCompletion(
    model: string,
    messages: ChatMessage[],
    options?: {
      maxTokens?: number;
      temperature?: number;
      topP?: number;
      tools?: Array<{ type: string; function: Record<string, unknown> }>;
      toolChoice?: string;
    },
  ): Promise<ChatResponse> {
    const body: Record<string, unknown> = {
      model,
      messages,
      max_tokens: options?.maxTokens || DEFAULT_MAX_TOKENS,
    };
    if (options?.temperature !== undefined) body.temperature = options.temperature;
    if (options?.topP !== undefined) body.top_p = options.topP;
    if (options?.tools?.length) body.tools = options.tools;
    if (options?.toolChoice !== undefined) body.tool_choice = options.toolChoice;
    return this.requestWithPayment('/v1/chat/completions', body);
  }

  getSpending() {
    return { totalUsd: this.sessionTotalUsd, calls: this.sessionCalls };
  }

  private async requestWithPayment(endpoint: string, body: Record<string, unknown>): Promise<ChatResponse> {
    const url = `${this.apiUrl}${endpoint}`;
    const response = await this.fetchWithTimeout(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });

    if (response.status === 402) {
      return this.handlePaymentAndRetry(url, body, response);
    }

    if (!response.ok) {
      const errText = await response.text().catch(() => 'Request failed');
      throw new Error(`API error ${response.status}: ${errText}`);
    }

    return response.json();
  }

  private async handlePaymentAndRetry(
    url: string,
    body: Record<string, unknown>,
    response: Response,
  ): Promise<ChatResponse> {
    let paymentHeader = response.headers.get('payment-required');
    if (!paymentHeader) {
      try {
        const text = await response.text();
        const respBody = JSON.parse(text);
        if (respBody.accepts || respBody.x402Version) {
          paymentHeader = btoa(JSON.stringify(respBody));
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
    if (!amount) throw new Error('No payment amount in 402 response');

    const feePayer = option.extra?.feePayer;
    if (!feePayer) throw new Error('Missing feePayer in 402 response');

    const paymentPayload = await this.createPaymentPayload(
      option.payTo,
      amount,
      feePayer,
      {
        resourceUrl: paymentRequired.resource?.url || url,
        resourceDescription: paymentRequired.resource?.description || 'BlockRun Solana AI API call',
        maxTimeoutSeconds: option.maxTimeoutSeconds || 300,
        extra: option.extra,
        extensions: paymentRequired.extensions,
      },
    );

    const retryResponse = await this.fetchWithTimeout(url, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'PAYMENT-SIGNATURE': paymentPayload,
      },
      body: JSON.stringify(body),
    });

    if (retryResponse.status === 402) {
      throw new Error('Payment was rejected. Check your Solana USDC balance.');
    }

    const retryText = await retryResponse.text();
    if (!retryResponse.ok) {
      throw new Error(`API error after payment ${retryResponse.status}: ${retryText}`);
    }

    const costUsd = parseFloat(amount) / 1e6;
    this.sessionCalls += 1;
    this.sessionTotalUsd += costUsd;

    return JSON.parse(retryText);
  }

  private async createPaymentPayload(
    recipient: string,
    amount: string,
    feePayer: string,
    options: {
      resourceUrl: string;
      resourceDescription: string;
      maxTimeoutSeconds: number;
      extra?: Record<string, unknown>;
      extensions?: Record<string, unknown>;
    },
  ): Promise<string> {
    const secretKey = bs58.decode(this.privateKey);
    const keypair = Keypair.fromSecretKey(secretKey);
    const connection = new Connection(this.rpcUrl, { commitment: 'confirmed' });

    const feePayerPubkey = new PublicKey(feePayer);
    const ownerPubkey = keypair.publicKey;
    const tokenMint = new PublicKey(USDC_MINT);
    const payToPubkey = new PublicKey(recipient);

    const mintInfo = await getMint(connection, tokenMint);
    const sourceATA = await getAssociatedTokenAddress(tokenMint, ownerPubkey, false);
    const destinationATA = await getAssociatedTokenAddress(tokenMint, payToPubkey, false);
    const { blockhash } = await connection.getLatestBlockhash();

    const setComputeUnitPriceIx = ComputeBudgetProgram.setComputeUnitPrice({
      microLamports: COMPUTE_UNIT_PRICE,
    });
    const setComputeUnitLimitIx = ComputeBudgetProgram.setComputeUnitLimit({
      units: COMPUTE_UNIT_LIMIT,
    });
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
      instructions: [setComputeUnitLimitIx, setComputeUnitPriceIx, transferIx],
    }).compileToV0Message();

    const transaction = new VersionedTransaction(messageV0);
    transaction.sign([keypair]);

    const serializedTx = Buffer.from(transaction.serialize()).toString('base64');

    const paymentData = {
      x402Version: 2,
      resource: {
        url: options.resourceUrl,
        description: options.resourceDescription,
        mimeType: 'application/json',
      },
      accepted: {
        scheme: 'exact',
        network: SOLANA_NETWORK_ID,
        amount,
        asset: USDC_MINT,
        payTo: recipient,
        maxTimeoutSeconds: options.maxTimeoutSeconds,
        extra: options.extra || { feePayer },
      },
      payload: { transaction: serializedTx },
      extensions: options.extensions || {},
    };

    return btoa(JSON.stringify(paymentData));
  }

  private async fetchWithTimeout(url: string, init: RequestInit): Promise<Response> {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), this.timeout);
    try {
      return await fetch(url, { ...init, signal: controller.signal });
    } finally {
      clearTimeout(timeoutId);
    }
  }
}
