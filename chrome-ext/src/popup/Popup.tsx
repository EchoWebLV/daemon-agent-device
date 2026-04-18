import React, { useState, useEffect, useCallback } from 'react';
import { PublicKey } from '@solana/web3.js';
import { QRCodeSVG } from 'qrcode.react';
import { WalletSetup } from '../components/WalletSetup';
import { BalanceBadge } from '../components/BalanceBadge';
import { DaemonLogo } from '../components/DaemonLogo';
import { hasWallet, unlockWallet, keypairFromBs58 } from '../lib/wallet';
import {
  getSOLBalance,
  getUSDCBalance,
  getConnection,
  sendSOL,
  sendUSDC,
  isValidAddress,
  truncateAddress,
  setCustomRpc,
} from '../lib/solana';
import { getStorageValue, setStorageValue, StorageKeys, getSession, saveSession } from '../lib/storage';
import type { Network } from '../types';

type View = 'loading' | 'setup' | 'unlock' | 'dashboard' | 'receive' | 'send';

export function Popup() {
  const [view, setView] = useState<View>('loading');
  const [address, setAddress] = useState('');
  const [network, setNetwork] = useState<Network>('mainnet-beta');
  const [solBalance, setSolBalance] = useState(0);
  const [usdcBalance, setUsdcBalance] = useState(0);
  const [privateKey, setPrivateKey] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(false);
  const [copied, setCopied] = useState(false);

  const [sendTo, setSendTo] = useState('');
  const [sendAmount, setSendAmount] = useState('');
  const [sendToken, setSendToken] = useState<'SOL' | 'USDC'>('USDC');
  const [sendStatus, setSendStatus] = useState<'idle' | 'sending' | 'success' | 'error'>('idle');
  const [txSig, setTxSig] = useState('');
  const [rpcUrl, setRpcUrl] = useState('');
  const [showSettings, setShowSettings] = useState(false);
  const [isTypingPw, setIsTypingPw] = useState(false);
  const typingTimerRef = React.useRef<number | null>(null);

  useEffect(() => {
    (async () => {
      const walletExists = await hasWallet();
      const storedAddr = await getStorageValue<string>(StorageKeys.WALLET_ADDRESS);
      const storedNet = await getStorageValue<Network>(StorageKeys.WALLET_NETWORK);
      const storedRpc = await getStorageValue<string>(StorageKeys.CUSTOM_RPC);
      if (storedNet) setNetwork(storedNet);
      if (storedRpc) {
        setRpcUrl(storedRpc);
        setCustomRpc(storedRpc);
      }
      if (walletExists && storedAddr) {
        setAddress(storedAddr);
        const cachedKey = await getSession();
        if (cachedKey) {
          setPrivateKey(cachedKey);
          setView('dashboard');
        } else {
          setView('unlock');
        }
      } else {
        setView('setup');
      }
    })();
  }, []);

  const fetchBalances = useCallback(async () => {
    if (!address) return;
    try {
      const conn = getConnection(network);
      const pubkey = new PublicKey(address);
      const [sol, usdc] = await Promise.all([
        getSOLBalance(pubkey, conn),
        getUSDCBalance(pubkey, conn, network),
      ]);
      setSolBalance(sol);
      setUsdcBalance(usdc);
      await setStorageValue(StorageKeys.SOL_BALANCE, sol);
      await setStorageValue(StorageKeys.USDC_BALANCE, usdc);
    } catch (e) {
      console.error('Balance fetch failed:', e);
    }
  }, [address, network]);

  useEffect(() => {
    if (view === 'dashboard') {
      fetchBalances();
      const interval = setInterval(fetchBalances, 30_000);
      return () => clearInterval(interval);
    }
  }, [view, fetchBalances]);

  async function handleUnlock() {
    if (!password) return;
    setError('');
    setLoading(true);
    try {
      const key = await unlockWallet(password);
      await saveSession(key);
      setPrivateKey(key);
      setView('dashboard');
    } catch {
      setError('Wrong password');
    } finally {
      setLoading(false);
    }
  }

  function handleWalletCreated(addr: string, pk: string) {
    setAddress(addr);
    setPrivateKey(pk);
    setView('dashboard');
  }

  async function handleSend() {
    if (!sendTo || !sendAmount) return;
    if (!isValidAddress(sendTo)) {
      setError('Invalid Solana address');
      return;
    }
    const amount = parseFloat(sendAmount);
    if (isNaN(amount) || amount <= 0) {
      setError('Invalid amount');
      return;
    }
    setError('');
    setSendStatus('sending');
    try {
      const keypair = keypairFromBs58(privateKey);
      const conn = getConnection(network);
      const sig =
        sendToken === 'SOL'
          ? await sendSOL(keypair, sendTo, amount, conn)
          : await sendUSDC(keypair, sendTo, amount, conn, network);
      setTxSig(sig);
      setSendStatus('success');
      fetchBalances();
    } catch (e: any) {
      setError(e.message || 'Transaction failed');
      setSendStatus('error');
    }
  }

  async function openSidePanel() {
    try {
      const win = await chrome.windows.getCurrent();
      await chrome.sidePanel.open({ windowId: win.id! });
    } catch (e) {
      console.error('Failed to open side panel:', e);
    }
  }

  function copyAddress() {
    navigator.clipboard.writeText(address);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  }

  if (view === 'loading') {
    return (
      <div className="flex items-center justify-center h-full">
        <div className="w-6 h-6 border-2 border-daemon-accent border-t-transparent rounded-full animate-spin" />
      </div>
    );
  }

  if (view === 'setup') {
    return <WalletSetup onComplete={handleWalletCreated} />;
  }

  if (view === 'unlock') {
    const handlePwChange = (e: React.ChangeEvent<HTMLInputElement>) => {
      setPassword(e.target.value);
      setIsTypingPw(true);
      if (typingTimerRef.current) clearTimeout(typingTimerRef.current);
      typingTimerRef.current = window.setTimeout(() => setIsTypingPw(false), 600);
    };

    return (
      <div className="flex flex-col items-center justify-center h-full px-6">
        <DaemonLogo isTyping={isTypingPw} className="w-40 h-40" />
        <h2 className="text-lg font-semibold mb-1 mt-4">Welcome Back</h2>
        <p className="text-xs text-daemon-dim mb-6">{truncateAddress(address, 6)}</p>
        <input
          type="password"
          placeholder="Enter password"
          value={password}
          onChange={handlePwChange}
          onKeyDown={(e) => e.key === 'Enter' && handleUnlock()}
          className="w-full bg-daemon-surface border border-daemon-border rounded-lg px-4 py-3 text-sm text-daemon-text placeholder-daemon-dim focus:outline-none focus:border-daemon-accent/50 mb-3"
          autoFocus
        />
        {error && <p className="text-red-400 text-xs mb-3">{error}</p>}
        <button
          onClick={handleUnlock}
          disabled={loading}
          className="w-full py-3 rounded-xl bg-daemon-accent text-daemon-bg font-semibold text-sm hover:bg-daemon-accent/90 transition-colors disabled:opacity-50"
        >
          {loading ? 'Unlocking...' : 'Unlock'}
        </button>
      </div>
    );
  }

  if (view === 'receive') {
    return (
      <div className="flex flex-col h-full px-6 pt-6">
        <button onClick={() => setView('dashboard')} className="text-daemon-dim text-sm mb-4 text-left hover:text-daemon-text">
          &larr; Back
        </button>
        <h2 className="text-lg font-semibold mb-4">Receive</h2>
        <div className="flex flex-col items-center">
          <div className="bg-white p-4 rounded-xl mb-4">
            <QRCodeSVG value={address} size={180} bgColor="#ffffff" fgColor="#111111" />
          </div>
          <p className="font-mono text-xs text-daemon-dim break-all text-center leading-relaxed mb-4 select-all">
            {address}
          </p>
          <button
            onClick={copyAddress}
            className="w-full py-3 rounded-xl bg-daemon-surface border border-daemon-border text-sm text-daemon-text hover:border-daemon-accent/30 transition-colors"
          >
            {copied ? 'Copied!' : 'Copy Address'}
          </button>
        </div>
      </div>
    );
  }

  if (view === 'send') {
    if (sendStatus === 'success') {
      return (
        <div className="flex flex-col items-center justify-center h-full px-6">
          <div className="w-14 h-14 rounded-full bg-daemon-accent/20 flex items-center justify-center mb-4">
            <span className="text-2xl text-daemon-accent">&#10003;</span>
          </div>
          <h2 className="text-lg font-semibold mb-2">Transaction Sent</h2>
          <p className="font-mono text-[10px] text-daemon-dim break-all text-center mb-6 px-4">{txSig}</p>
          <button
            onClick={() => { setSendStatus('idle'); setSendTo(''); setSendAmount(''); setTxSig(''); setView('dashboard'); }}
            className="w-full py-3 rounded-xl bg-daemon-accent text-daemon-bg font-semibold text-sm"
          >
            Done
          </button>
        </div>
      );
    }

    return (
      <div className="flex flex-col h-full px-6 pt-6">
        <button onClick={() => { setView('dashboard'); setError(''); setSendStatus('idle'); }} className="text-daemon-dim text-sm mb-4 text-left hover:text-daemon-text">
          &larr; Back
        </button>
        <h2 className="text-lg font-semibold mb-4">Send</h2>
        <div className="flex gap-2 mb-4">
          {(['SOL', 'USDC'] as const).map((token) => (
            <button
              key={token}
              onClick={() => setSendToken(token)}
              className={`flex-1 py-2 rounded-lg text-sm font-medium transition-colors ${
                sendToken === token
                  ? 'bg-daemon-accent text-daemon-bg'
                  : 'bg-daemon-surface border border-daemon-border text-daemon-text'
              }`}
            >
              {token}
            </button>
          ))}
        </div>
        <input
          placeholder="Recipient address"
          value={sendTo}
          onChange={(e) => setSendTo(e.target.value)}
          className="w-full bg-daemon-surface border border-daemon-border rounded-lg px-4 py-3 text-sm font-mono text-daemon-text placeholder-daemon-dim focus:outline-none focus:border-daemon-accent/50 mb-3"
        />
        <div className="relative mb-4">
          <input
            type="number"
            placeholder="Amount"
            value={sendAmount}
            onChange={(e) => setSendAmount(e.target.value)}
            step="any"
            min="0"
            className="w-full bg-daemon-surface border border-daemon-border rounded-lg px-4 py-3 pr-16 text-sm font-mono text-daemon-text placeholder-daemon-dim focus:outline-none focus:border-daemon-accent/50"
          />
          <span className="absolute right-4 top-1/2 -translate-y-1/2 text-xs text-daemon-dim">{sendToken}</span>
        </div>
        <p className="text-xs text-daemon-dim mb-4">
          Available: {sendToken === 'SOL' ? `${solBalance.toFixed(4)} SOL` : `$${usdcBalance.toFixed(2)} USDC`}
        </p>
        {error && <p className="text-red-400 text-xs mb-3">{error}</p>}
        <button
          onClick={handleSend}
          disabled={sendStatus === 'sending'}
          className="w-full py-3 rounded-xl bg-daemon-accent text-daemon-bg font-semibold text-sm hover:bg-daemon-accent/90 transition-colors disabled:opacity-50"
        >
          {sendStatus === 'sending' ? 'Sending...' : `Send ${sendToken}`}
        </button>
      </div>
    );
  }

  return (
    <div className="flex flex-col h-full px-5 pt-5 pb-4">
      <div className="flex items-center justify-between mb-5">
        <div className="flex items-center gap-2">
          <div className="w-8 h-8 rounded-lg flex items-center justify-center overflow-hidden">
            <img src="icons/daemon.png" alt="Daemon" className="w-7 h-7 object-contain" />
          </div>
          <span className="font-semibold text-sm">Daemon</span>
        </div>
        <button
          onClick={copyAddress}
          className="font-mono text-xs text-daemon-dim hover:text-daemon-text transition-colors bg-daemon-surface px-3 py-1.5 rounded-lg border border-daemon-border"
          title={address}
        >
          {copied ? 'Copied!' : truncateAddress(address, 5)}
        </button>
      </div>

      <BalanceBadge usdcBalance={usdcBalance} solBalance={solBalance} />

      <div className="grid grid-cols-3 gap-2 mt-5">
        <button
          onClick={() => setView('receive')}
          className="flex flex-col items-center gap-1.5 py-3 rounded-xl bg-daemon-surface border border-daemon-border hover:border-daemon-accent/30 transition-colors"
        >
          <span className="text-lg">&darr;</span>
          <span className="text-xs text-daemon-dim">Receive</span>
        </button>
        <button
          onClick={() => setView('send')}
          className="flex flex-col items-center gap-1.5 py-3 rounded-xl bg-daemon-surface border border-daemon-border hover:border-daemon-accent/30 transition-colors"
        >
          <span className="text-lg">&uarr;</span>
          <span className="text-xs text-daemon-dim">Send</span>
        </button>
        <button
          onClick={openSidePanel}
          className="flex flex-col items-center gap-1.5 py-3 rounded-xl bg-daemon-accent/10 border border-daemon-accent/20 hover:bg-daemon-accent/15 transition-colors"
        >
          <img src="icons/daemon.png" alt="Chat" className="w-5 h-5 object-contain" />
          <span className="text-xs text-daemon-accent">Chat</span>
        </button>
      </div>

      <div className="mt-auto pt-4 space-y-2">
        <div className="flex items-center justify-between">
          <span className="text-[10px] text-daemon-dim font-mono">
            {network === 'mainnet-beta' ? 'Mainnet' : 'Devnet'}
          </span>
          <div className="flex gap-3">
            <button
              onClick={() => setShowSettings((s) => !s)}
              className="text-[10px] text-daemon-dim hover:text-daemon-accent transition-colors"
            >
              {showSettings ? 'Hide Settings' : 'Settings'}
            </button>
            <button
              onClick={async () => {
                const next: Network = network === 'mainnet-beta' ? 'devnet' : 'mainnet-beta';
                setNetwork(next);
                await setStorageValue(StorageKeys.WALLET_NETWORK, next);
                setCustomRpc(rpcUrl || null);
                fetchBalances();
              }}
              className="text-[10px] text-daemon-dim hover:text-daemon-accent transition-colors"
            >
              Switch Network
            </button>
          </div>
        </div>
        {showSettings && (
          <div className="space-y-2">
            <label className="text-[10px] text-daemon-dim block">RPC URL (Helius, QuickNode, etc.)</label>
            <input
              value={rpcUrl}
              onChange={(e) => setRpcUrl(e.target.value)}
              placeholder="https://mainnet.helius-rpc.com/?api-key=..."
              className="w-full bg-daemon-surface border border-daemon-border rounded-lg px-3 py-2 text-[11px] font-mono text-daemon-text placeholder-daemon-dim/50 focus:outline-none focus:border-daemon-accent/50"
            />
            <button
              onClick={async () => {
                const url = rpcUrl.trim() || null;
                setCustomRpc(url);
                await setStorageValue(StorageKeys.CUSTOM_RPC, url ?? '');
                fetchBalances();
              }}
              className="w-full py-1.5 rounded-lg bg-daemon-surface border border-daemon-border text-[10px] text-daemon-text hover:border-daemon-accent/30 transition-colors"
            >
              Save &amp; Refresh
            </button>
          </div>
        )}
      </div>
    </div>
  );
}
