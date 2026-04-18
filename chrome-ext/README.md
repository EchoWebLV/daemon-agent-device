# Daemon — Chrome Extension PoC

A Chrome extension (Manifest V3) that is a basic Solana wallet with an AI chat interface. Users fund it with USDC on Solana and chat with multiple AI models. Each message is paid for with USDC via x402 protocol using `@blockrun/llm`. No subscriptions, no API keys. Crypto is the credit system.

## Setup

```bash
npm install
npm run build
```

## Load in Chrome

1. Open `chrome://extensions`
2. Enable **Developer mode**
3. Click **Load unpacked** and select the `dist/` folder
4. The Daemon icon appears in your toolbar

## Usage

1. Click the Daemon icon to open the popup
2. **Create** a new wallet or **Import** an existing private key (base58)
3. Set a password to encrypt the key locally
4. Fund the wallet with USDC on Solana mainnet
5. Click **Chat** to open the side panel
6. Enter your password to unlock, pick a model, and start chatting

## Architecture

| Area | Tech |
|---|---|
| Extension | Manifest V3 |
| UI | React 19 + TypeScript + Tailwind CSS |
| Bundler | Vite 6 |
| Wallet | `@solana/web3.js` + `@solana/spl-token` |
| AI | `@blockrun/llm` (x402 on Solana) |
| Encryption | AES-256-GCM via SubtleCrypto |
| Storage | `chrome.storage.local` |

## Project Structure

```
popup.html / sidepanel.html    Entry HTML files
public/manifest.json           Chrome extension manifest
src/
  popup/Popup.tsx              Wallet UI (create, balances, send, receive)
  sidepanel/SidePanel.tsx      AI chat interface
  background/service-worker.ts Badge updates + alarms
  lib/
    wallet.ts                  Keypair gen, AES encryption
    solana.ts                  RPC connection, balance, transfers
    llm.ts                     @blockrun/llm wrapper + streaming
    storage.ts                 chrome.storage helpers
  components/
    WalletSetup.tsx            Create / import wallet flow
    ChatMessage.tsx            Message bubble with markdown
    ModelSelector.tsx          AI model dropdown
    BalanceBadge.tsx           USDC balance display
  types/index.ts               Shared types, models, constants
```

## Available Models

| Model | Provider | Input / Output Price (per 1M tokens) |
|---|---|---|
| `deepseek/deepseek-chat` | DeepSeek | $0.28 / $0.42 |
| `openai/gpt-4o-mini` | OpenAI | $0.15 / $0.60 |
| `google/gemini-2.5-flash` | Google | $0.30 / $2.50 |
| `xai/grok-3-mini` | xAI | $0.30 / $0.50 |
| `openai/gpt-4o` | OpenAI | $2.50 / $10.00 |
| `anthropic/claude-haiku-4.5` | Anthropic | $1.00 / $5.00 |
| `anthropic/claude-sonnet-4.6` | Anthropic | $3.00 / $15.00 |

## Development

Watch mode (rebuild on save):

```bash
npm run dev
```

After rebuilding, reload the extension in `chrome://extensions`.
