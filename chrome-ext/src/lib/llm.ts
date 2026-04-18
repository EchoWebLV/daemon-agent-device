import { SolanaLLMClient, type ChatResponse, type ChatMessage as X402ChatMessage } from './x402-client';
import type { ChatMessage, ModelInfo } from '../types';
import { RPC_ENDPOINTS } from '../types';
import { getStorageValue, StorageKeys } from './storage';
import { TOOL_DEFINITIONS, executeTool, setWalletKey } from './tools';
import {
  servicesToToolDefs,
  executeServiceTool,
  isServiceTool,
} from './x402-services';

let client: SolanaLLMClient | null = null;

export async function initLLMClient(privateKeyBs58: string) {
  const customRpc = await getStorageValue<string>(StorageKeys.CUSTOM_RPC);
  const rpcUrl = customRpc || RPC_ENDPOINTS['mainnet-beta'];
  client = new SolanaLLMClient({ privateKey: privateKeyBs58, rpcUrl });
  setWalletKey(privateKeyBs58);
}

export function getLLMClient(): SolanaLLMClient | null {
  return client;
}

const MAX_TOOL_ROUNDS = 5;

export async function chatWithTools(
  model: string,
  messages: ChatMessage[],
  onToolCall?: (toolName: string) => void,
  enabledServiceIds?: string[],
): Promise<{ content: string; usage?: { prompt_tokens: number; completion_tokens: number } }> {
  if (!client) throw new Error('LLM client not initialized. Unlock your wallet first.');

  const x402Tools = enabledServiceIds?.length
    ? servicesToToolDefs(enabledServiceIds)
    : [];
  const allTools = [...TOOL_DEFINITIONS, ...x402Tools];

  const apiMessages: X402ChatMessage[] = messages.map((m) => ({
    role: m.role as X402ChatMessage['role'],
    content: m.content,
  }));

  let rounds = 0;
  while (rounds < MAX_TOOL_ROUNDS) {
    const result: ChatResponse = await client.chatCompletion(model, apiMessages, {
      tools: allTools,
      toolChoice: 'auto',
    });

    const choice = result.choices?.[0];
    if (!choice) throw new Error('No response from model');

    const msg = choice.message;

    if (choice.finish_reason === 'tool_calls' || msg.tool_calls?.length) {
      apiMessages.push({
        role: 'assistant',
        content: msg.content ?? null,
        tool_calls: msg.tool_calls,
      });

      for (const tc of msg.tool_calls ?? []) {
        onToolCall?.(tc.function.name);
        let args: Record<string, unknown> = {};
        try {
          args = JSON.parse(tc.function.arguments);
        } catch { /* ignore */ }

        const toolResult = isServiceTool(tc.function.name)
          ? await executeServiceTool(tc.function.name, args)
          : await executeTool(tc.function.name, args);

        apiMessages.push({
          role: 'tool',
          tool_call_id: tc.id,
          content: toolResult,
        });
      }

      rounds++;
      continue;
    }

    return {
      content: msg.content ?? '',
      usage: result.usage,
    };
  }

  return { content: 'Tool call limit reached. Please try a simpler query.' };
}

export function estimateCost(
  model: ModelInfo,
  inputText: string,
  outputText: string,
): number {
  const inputTokens = Math.ceil(inputText.length / 4);
  const outputTokens = Math.ceil(outputText.length / 4);
  return (inputTokens * model.inputPrice + outputTokens * model.outputPrice) / 1_000_000;
}
