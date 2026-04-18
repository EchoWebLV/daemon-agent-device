import React, { useState, useEffect, useRef, useCallback } from 'react';
import { PublicKey } from '@solana/web3.js';
import { ChatMessage, ThinkingIndicator } from '../components/ChatMessage';
import { ModelSelector } from '../components/ModelSelector';
import { BalanceBadge } from '../components/BalanceBadge';
import { getStorageValue, setStorageValue, onStorageChanged, StorageKeys, getSession, saveSession } from '../lib/storage';
import { hasWallet, unlockWallet } from '../lib/wallet';
import { initLLMClient, estimateCost, chatWithTools } from '../lib/llm';
import { getUSDCBalance, getSOLBalance, getSOLPrice, getTokenHoldings, getConnection, setCustomRpc, type TokenHolding } from '../lib/solana';
import { MODELS, USDC_MINT_MAINNET, USDC_MINT_DEVNET, type Message, type Network } from '../types';
import {
  type Conversation,
  getAllConversations,
  createConversation,
  deleteConversation,
  getMessages,
  saveMessagesAndUpdateConv,
} from '../lib/chatdb';
import { ServicesTab } from '../components/ServicesTab';
import { DaemonLogo } from '../components/DaemonLogo';
import { getEnabledServiceNames, fetchServices } from '../lib/x402-services';

export function SidePanel() {
  const [messages, setMessages] = useState<Message[]>([]);
  const [input, setInput] = useState('');
  const [model, setModel] = useState('deepseek/deepseek-chat');
  const [usdcBalance, setUsdcBalance] = useState(0);
  const [isStreaming, setIsStreaming] = useState(false);
  const [isUnlocked, setIsUnlocked] = useState(false);
  const [walletExists, setWalletExists] = useState(false);
  const [password, setPassword] = useState('');
  const [error, setError] = useState('');
  const [address, setAddress] = useState('');
  const [network, setNetwork] = useState<Network>('mainnet-beta');
  const [solBalance, setSolBalance] = useState(0);
  const [solPrice, setSolPrice] = useState<number | null>(null);
  const [tokenHoldings, setTokenHoldings] = useState<TokenHolding[]>([]);
  const [sessionSpend, setSessionSpend] = useState(0);
  const [activeTool, setActiveTool] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);

  const [conversations, setConversations] = useState<Conversation[]>([]);
  const [activeConvId, setActiveConvId] = useState<string | null>(null);
  const [drawerOpen, setDrawerOpen] = useState(false);
  const [activeTab, setActiveTab] = useState<'chat' | 'services' | 'smart-money'>('chat');
  const [isTypingPw, setIsTypingPw] = useState(false);
  const [enabledServiceIds, setEnabledServiceIds] = useState<string[]>([]);

  const messagesEndRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLTextAreaElement>(null);
  const typingTimerRef = useRef<number | null>(null);

  const scrollToBottom = useCallback(() => {
    messagesEndRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, []);

  useEffect(() => {
    scrollToBottom();
  }, [messages, isStreaming, scrollToBottom]);

  useEffect(() => {
    (async () => {
      const exists = await hasWallet();
      setWalletExists(exists);
      const addr = await getStorageValue<string>(StorageKeys.WALLET_ADDRESS);
      const net = await getStorageValue<Network>(StorageKeys.WALLET_NETWORK);
      const savedModel = await getStorageValue<string>(StorageKeys.LAST_MODEL);
      const cachedBalance = await getStorageValue<number>(StorageKeys.USDC_BALANCE);
      const storedRpc = await getStorageValue<string>(StorageKeys.CUSTOM_RPC);
      const storedEnabledSvcs = await getStorageValue<string[]>(StorageKeys.ENABLED_SERVICES);

      if (storedRpc) setCustomRpc(storedRpc);
      await fetchServices();
      if (storedEnabledSvcs) setEnabledServiceIds(storedEnabledSvcs);
      if (addr) setAddress(addr);
      if (net) setNetwork(net);
      if (savedModel) setModel(savedModel);
      if (cachedBalance !== null) setUsdcBalance(cachedBalance);

      const convs = await getAllConversations();
      setConversations(convs);
      if (convs.length > 0) {
        const latest = convs[0];
        setActiveConvId(latest.id);
        const msgs = await getMessages(latest.id);
        setMessages(msgs);
      }

      if (exists) {
        const cachedKey = await getSession();
        if (cachedKey) {
          await initLLMClient(cachedKey);
          setIsUnlocked(true);
        }
      }

      setLoading(false);
    })();

    const unsub = onStorageChanged((changes) => {
      if (changes[StorageKeys.WALLET_ADDRESS]) {
        setAddress(changes[StorageKeys.WALLET_ADDRESS].newValue ?? '');
        setWalletExists(true);
      }
      if (changes[StorageKeys.USDC_BALANCE]) {
        setUsdcBalance(changes[StorageKeys.USDC_BALANCE].newValue ?? 0);
      }
      if (changes[StorageKeys.WALLET_NETWORK]) {
        setNetwork(changes[StorageKeys.WALLET_NETWORK].newValue ?? 'mainnet-beta');
      }
    });
    return unsub;
  }, []);

  const refreshBalance = useCallback(async () => {
    if (!address) return;
    try {
      const conn = getConnection(network);
      const pubkey = new PublicKey(address);
      const [usdcBal, solBal, tokens, solPx] = await Promise.all([
        getUSDCBalance(pubkey, conn, network),
        getSOLBalance(pubkey, conn),
        getTokenHoldings(pubkey, conn),
        getSOLPrice(),
      ]);
      setUsdcBalance(usdcBal);
      setSolBalance(solBal);
      setSolPrice(solPx);
      setTokenHoldings(tokens);
      await setStorageValue(StorageKeys.USDC_BALANCE, usdcBal);
    } catch {
      /* ignore */
    }
  }, [address, network]);

  async function handleUnlock() {
    if (!password) return;
    setError('');
    try {
      const key = await unlockWallet(password);
      await saveSession(key);
      await initLLMClient(key);
      setIsUnlocked(true);
      refreshBalance();
    } catch {
      setError('Wrong password');
    }
  }

  async function loadConversation(convId: string) {
    if (convId === activeConvId) {
      setDrawerOpen(false);
      return;
    }
    const msgs = await getMessages(convId);
    setMessages(msgs);
    setActiveConvId(convId);
    setDrawerOpen(false);
    setError('');
  }

  async function handleNewChat() {
    setMessages([]);
    setActiveConvId(null);
    setSessionSpend(0);
    setDrawerOpen(false);
    setError('');
  }

  async function handleDeleteConversation(convId: string, e: React.MouseEvent) {
    e.stopPropagation();
    await deleteConversation(convId);
    const updated = await getAllConversations();
    setConversations(updated);
    if (convId === activeConvId) {
      if (updated.length > 0) {
        await loadConversation(updated[0].id);
      } else {
        setMessages([]);
        setActiveConvId(null);
      }
    }
  }

  async function refreshConversationList() {
    const convs = await getAllConversations();
    setConversations(convs);
  }

  async function handleSend() {
    const text = input.trim();
    if (!text || isStreaming) return;

    const userMsg: Message = {
      id: crypto.randomUUID(),
      role: 'user',
      content: text,
      timestamp: Date.now(),
    };

    const updatedMessages = [...messages, userMsg];
    setMessages(updatedMessages);
    setInput('');
    setIsStreaming(true);
    setError('');

    let convId = activeConvId;
    if (!convId) {
      const conv = await createConversation(model);
      convId = conv.id;
      setActiveConvId(convId);
    }

    const assistantId = crypto.randomUUID();
    const inputForCost = updatedMessages.map((m) => m.content).join('\n');

    try {
      const modelInfo = MODELS.find((m) => m.id === model);

      const solValueStr = solPrice !== null
        ? `${solBalance.toFixed(4)} SOL (~$${(solBalance * solPrice).toFixed(2)})`
        : `${solBalance.toFixed(4)} SOL`;

      const usdcMint = network === 'mainnet-beta' ? USDC_MINT_MAINNET : USDC_MINT_DEVNET;
      const tokenLines = tokenHoldings
        .filter((t) => t.mint !== usdcMint)
        .map((t) => {
          const val = t.usdValue !== null ? ` (~$${t.usdValue.toFixed(2)})` : '';
          const price = t.usdPrice !== null ? ` @ $${t.usdPrice.toFixed(6)}` : '';
          return `  - ${t.symbol} (${t.name}): ${t.amount}${price}${val} [mint: ${t.mint}]`;
        })
        .join('\n');

      const totalUsdValue = (() => {
        let total = usdcBalance;
        if (solPrice !== null) total += solBalance * solPrice;
        for (const t of tokenHoldings) {
          if (t.mint !== usdcMint && t.usdValue !== null) total += t.usdValue;
        }
        return total;
      })();

      const systemContent = [
        'You are Daemon, an AI assistant embedded in a Solana wallet Chrome extension.',
        'The user is chatting with you from inside their wallet. You have live awareness of their wallet state.',
        '',
        '## Current Wallet State',
        `- Address: ${address}`,
        `- Network: ${network}`,
        `- SOL: ${solValueStr}`,
        `- USDC: ${usdcBalance.toFixed(2)} USDC`,
        tokenLines ? `- Other tokens:\n${tokenLines}` : '- Other tokens: none',
        `- Estimated portfolio value: ~$${totalUsdValue.toFixed(2)}`,
        `- Session spend: $${sessionSpend.toFixed(4)}`,
        `- Model: ${model}`,
        '',
        'Use this context naturally. If the user asks about their balance, tokens, address, or portfolio, answer from the live data above.',
        '',
        '## Tools Available',
        'You have built-in tools for: swap quotes, token safety/rug checks, token profiles, trending tokens, new pair launches,',
        'top holder analysis, wallet portfolio scanning, transaction history, token search, price checks, and SWAP EXECUTION.',
        ...(enabledServiceIds.length > 0
          ? [
              '',
              '## x402 Paid Services (enabled by user)',
              `The user has enabled these x402 services: ${getEnabledServiceNames(enabledServiceIds).join(', ')}.`,
              'These are premium paid tools (cost USDC). Use them when they provide better data than built-in tools.',
              'The x402 tools have names starting with "x402_". Prefer built-in (free) tools when sufficient.',
            ]
          : []),
        '',
        'Use tools proactively when relevant.',
        '',
        '## Swap Execution Rules (CRITICAL)',
        '1. When the user wants to buy/sell/swap tokens, FIRST call get_swap_quote to show them the quote.',
        '2. Present the quote clearly: input amount, output amount, price impact, route.',
        '3. Ask the user to confirm with something like "Shall I execute this swap?"',
        '4. ONLY call execute_swap AFTER the user explicitly confirms (e.g. "yes", "do it", "go ahead", "confirm").',
        '5. NEVER execute a swap without the user confirming first. This is real money.',
        '6. After execution, show the transaction signature and Solscan link.',
        '',
        'Common mint addresses for swaps:',
        '- SOL: So11111111111111111111111111111111111111112',
        '- USDC: EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v',
        'For other tokens, use search_token first to find the mint address.',
        '',
        'Be concise, helpful, and knowledgeable about Solana, DeFi, and crypto.',
      ].join('\n');

      const chatMessages = [
        { role: 'system' as const, content: systemContent },
        ...updatedMessages.map((m) => ({
          role: m.role as 'user' | 'assistant' | 'system',
          content: m.content,
        })),
      ];

      const result = await chatWithTools(model, chatMessages, (toolName) => {
        setActiveTool(toolName);
      }, enabledServiceIds);
      setActiveTool(null);
      const fullResponse = result.content;

      const cost = modelInfo ? estimateCost(modelInfo, inputForCost, fullResponse) : 0;
      const finalMsg: Message = {
        id: assistantId,
        role: 'assistant',
        content: fullResponse,
        cost,
        timestamp: Date.now(),
      };

      setMessages((prev) => {
        const without = prev.filter((m) => m.id !== assistantId);
        return [...without, finalMsg];
      });

      setSessionSpend((s) => s + cost);
      await setStorageValue(StorageKeys.SESSION_SPEND, sessionSpend + cost);

      await saveMessagesAndUpdateConv(convId, [userMsg, finalMsg]);
      await refreshConversationList();
      refreshBalance();
    } catch (e: any) {
      setError(e.message || 'Failed to get response');
      setMessages(updatedMessages);
    } finally {
      setIsStreaming(false);
    }
  }

  async function handleModelChange(modelId: string) {
    setModel(modelId);
    await setStorageValue(StorageKeys.LAST_MODEL, modelId);
  }

  function handleKeyDown(e: React.KeyboardEvent) {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleSend();
    }
  }

  if (loading) {
    return (
      <div className="flex items-center justify-center h-full">
        <div className="w-6 h-6 border-2 border-daemon-accent border-t-transparent rounded-full animate-spin" />
      </div>
    );
  }

  if (!walletExists) {
    return (
      <div className="flex flex-col items-center justify-center h-full px-6 text-center">
        <img src="icons/logo.png" alt="Daemon" className="w-40 h-40 object-contain mb-2" />
        <h2 className="text-lg font-semibold mb-2">No Wallet Found</h2>
        <p className="text-sm text-daemon-dim">
          Open the Daemon popup to create or import a wallet, then return here to chat.
        </p>
      </div>
    );
  }

  if (!isUnlocked) {
    const handlePwChange = (e: React.ChangeEvent<HTMLInputElement>) => {
      setPassword(e.target.value);
      setIsTypingPw(true);
      if (typingTimerRef.current) clearTimeout(typingTimerRef.current);
      typingTimerRef.current = window.setTimeout(() => setIsTypingPw(false), 600);
    };

    return (
      <div className="flex flex-col items-center justify-center h-full px-6">
        <DaemonLogo isTyping={isTypingPw} className="w-40 h-40" />
        <h2 className="text-lg font-semibold mb-1 mt-4">Unlock to Chat</h2>
        <p className="text-xs text-daemon-dim mb-6">Enter your password to start chatting with AI.</p>
        <input
          type="password"
          placeholder="Enter password"
          value={password}
          onChange={handlePwChange}
          onKeyDown={(e) => e.key === 'Enter' && handleUnlock()}
          className="w-full max-w-sm bg-daemon-surface border border-daemon-border rounded-lg px-4 py-3 text-sm text-daemon-text placeholder-daemon-dim focus:outline-none focus:border-daemon-accent/50 mb-3"
          autoFocus
        />
        {error && <p className="text-red-400 text-xs mb-3">{error}</p>}
        <button
          onClick={handleUnlock}
          className="w-full max-w-sm py-3 rounded-xl bg-daemon-accent text-daemon-bg font-semibold text-sm hover:bg-daemon-accent/90 transition-colors"
        >
          Unlock
        </button>
      </div>
    );
  }

  return (
    <div className="flex flex-col h-full">
      {/* Tab bar */}
      <div className="flex border-b border-daemon-border bg-daemon-bg">
        {([['chat', 'Chat'], ['services', 'Services'], ['smart-money', 'Smart Money']] as const).map(([id, label]) => (
          <button
            key={id}
            onClick={() => setActiveTab(id)}
            className={`flex-1 py-2.5 text-xs font-medium transition-colors relative ${
              activeTab === id
                ? 'text-daemon-accent'
                : 'text-daemon-dim hover:text-daemon-text'
            }`}
          >
            {label}
            {activeTab === id && (
              <span className="absolute bottom-0 left-1/4 right-1/4 h-0.5 bg-daemon-accent rounded-full" />
            )}
          </button>
        ))}
      </div>

      {activeTab === 'services' ? (
        <ServicesTab onEnabledChange={setEnabledServiceIds} />
      ) : activeTab === 'smart-money' ? (
        <iframe
          src="https://web-production-b87f0.up.railway.app/simple"
          className="flex-1 w-full border-0"
          allow="clipboard-read; clipboard-write"
        />
      ) : (
        <>
          {/* Chat header */}
          <div className="flex items-center justify-between px-4 py-3 border-b border-daemon-border bg-daemon-bg/80 backdrop-blur-sm relative z-20">
            <div className="flex items-center gap-2 min-w-0">
              <button
                onClick={() => setDrawerOpen((o) => !o)}
                className="text-daemon-dim hover:text-daemon-text transition-colors p-1 flex-shrink-0"
                title="Chat history"
              >
                <svg width="16" height="16" viewBox="0 0 16 16" fill="none">
                  <path d="M2 4h12M2 8h12M2 12h12" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
                </svg>
              </button>
              <div className="flex-1 min-w-0 mr-2">
                <ModelSelector selectedModel={model} onModelChange={handleModelChange} />
              </div>
            </div>
            <div className="flex items-center gap-2 flex-shrink-0">
              <BalanceBadge usdcBalance={usdcBalance} compact />
              <button
                onClick={handleNewChat}
                className="text-daemon-dim hover:text-daemon-accent transition-colors p-1"
                title="New chat"
              >
                <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
                  <path d="M7 2v10M2 7h10" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
                </svg>
              </button>
            </div>
          </div>

          {/* Conversation drawer */}
          {drawerOpen && (
            <div className="border-b border-daemon-border bg-daemon-bg overflow-hidden">
              <div className="max-h-64 overflow-y-auto">
                {conversations.length === 0 ? (
                  <p className="text-xs text-daemon-dim text-center py-4">No conversations yet</p>
                ) : (
                  conversations.map((conv) => (
                    <button
                      key={conv.id}
                      onClick={() => loadConversation(conv.id)}
                      className={`w-full flex items-center gap-2 px-4 py-2.5 text-left hover:bg-daemon-surface/60 transition-colors group ${
                        conv.id === activeConvId ? 'bg-daemon-surface border-l-2 border-daemon-accent' : ''
                      }`}
                    >
                      <div className="flex-1 min-w-0">
                        <p className={`text-xs truncate ${
                          conv.id === activeConvId ? 'text-daemon-text font-medium' : 'text-gray-300'
                        }`}>
                          {conv.title}
                        </p>
                        <p className="text-[10px] text-daemon-dim font-mono mt-0.5">
                          {conv.messageCount} msgs &middot; {formatRelative(conv.updatedAt)}
                        </p>
                      </div>
                      <button
                        onClick={(e) => handleDeleteConversation(conv.id, e)}
                        className="opacity-0 group-hover:opacity-100 text-daemon-dim hover:text-red-400 transition-all p-0.5 flex-shrink-0"
                        title="Delete"
                      >
                        <svg width="12" height="12" viewBox="0 0 12 12" fill="none">
                          <path d="M3 3l6 6M9 3L3 9" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" />
                        </svg>
                      </button>
                    </button>
                  ))
                )}
              </div>
            </div>
          )}

          {usdcBalance === 0 && (
            <div className="px-4 py-2 bg-yellow-500/10 border-b border-yellow-500/20">
              <p className="text-xs text-yellow-400">Top up your wallet with USDC to continue chatting.</p>
            </div>
          )}

          {/* Messages */}
          <div className="flex-1 overflow-y-auto px-4 py-4">
            {messages.length === 0 && (
              <div className="flex flex-col items-center justify-center h-full text-center">
                <img src="icons/daemon.png" alt="Daemon" className="w-12 h-12 object-contain mb-3" />
                <p className="text-sm text-daemon-dim mb-1">Start a conversation</p>
                <p className="text-xs text-daemon-dim/60">Each message is paid with USDC via x402</p>
              </div>
            )}
            {messages.map((msg) => (
              <ChatMessage key={msg.id} message={msg} />
            ))}
            {isStreaming && messages.every((m) => m.id && m.content) && (
              <div>
                <ThinkingIndicator />
                {activeTool && (
                  <p className="text-[10px] text-daemon-accent font-mono ml-10 -mt-1 mb-2 animate-pulse">
                    Using {activeTool.replace(/_/g, ' ')}...
                  </p>
                )}
              </div>
            )}
            <div ref={messagesEndRef} />
          </div>

          {error && (
            <div className="px-4 py-2 bg-red-500/10 border-t border-red-500/20">
              <p className="text-xs text-red-400">{error}</p>
            </div>
          )}

          {/* Input */}
          <div className="px-4 py-3 border-t border-daemon-border bg-daemon-bg">
            <div className="flex items-end gap-2">
              <textarea
                ref={inputRef}
                value={input}
                onChange={(e) => setInput(e.target.value)}
                onKeyDown={handleKeyDown}
                placeholder="Type a message..."
                rows={1}
                className="flex-1 bg-daemon-surface border border-daemon-border rounded-xl px-4 py-3 text-sm text-daemon-text placeholder-daemon-dim focus:outline-none focus:border-daemon-accent/50 resize-none max-h-32 transition-colors"
                style={{ minHeight: '44px' }}
                disabled={isStreaming}
              />
              <button
                onClick={handleSend}
                disabled={isStreaming || !input.trim()}
                className="h-11 w-11 rounded-xl bg-daemon-accent text-daemon-bg flex items-center justify-center hover:bg-daemon-accent/90 transition-colors disabled:opacity-30 flex-shrink-0"
              >
                <svg width="16" height="16" viewBox="0 0 16 16" fill="none">
                  <path d="M14 2L7 9M14 2l-4.5 12-2-5.5L2 6.5 14 2z" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
                </svg>
              </button>
            </div>
            {sessionSpend > 0 && (
              <p className="text-[10px] text-daemon-dim font-mono mt-2 text-center">
                Session: ~${sessionSpend.toFixed(4)} spent
              </p>
            )}
          </div>
        </>
      )}
    </div>
  );
}

function formatRelative(ts: number): string {
  const diff = Date.now() - ts;
  const mins = Math.floor(diff / 60_000);
  if (mins < 1) return 'just now';
  if (mins < 60) return `${mins}m ago`;
  const hrs = Math.floor(mins / 60);
  if (hrs < 24) return `${hrs}h ago`;
  const days = Math.floor(hrs / 24);
  if (days < 7) return `${days}d ago`;
  return new Date(ts).toLocaleDateString();
}
