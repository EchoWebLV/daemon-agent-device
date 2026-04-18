import React, { useState } from 'react';
import { createWallet, importWallet } from '../lib/wallet';

interface WalletSetupProps {
  onComplete: (address: string, privateKey: string) => void;
}

type Step = 'choose' | 'create-password' | 'create-backup' | 'import-key' | 'import-password';

export function WalletSetup({ onComplete }: WalletSetupProps) {
  const [step, setStep] = useState<Step>('choose');
  const [password, setPassword] = useState('');
  const [confirmPassword, setConfirmPassword] = useState('');
  const [importKey, setImportKey] = useState('');
  const [backupKey, setBackupKey] = useState('');
  const [address, setAddress] = useState('');
  const [saved, setSaved] = useState(false);
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(false);

  async function handleCreate() {
    if (password.length < 8) {
      setError('Password must be at least 8 characters');
      return;
    }
    if (password !== confirmPassword) {
      setError('Passwords do not match');
      return;
    }
    setError('');
    setLoading(true);
    try {
      const result = await createWallet(password);
      setAddress(result.address);
      setBackupKey(result.backupKey);
      setStep('create-backup');
    } catch (e: any) {
      setError(e.message);
    } finally {
      setLoading(false);
    }
  }

  async function handleImport() {
    if (!importKey.trim()) {
      setError('Please enter a private key');
      return;
    }
    setError('');
    setStep('import-password');
  }

  async function handleImportConfirm() {
    if (password.length < 8) {
      setError('Password must be at least 8 characters');
      return;
    }
    if (password !== confirmPassword) {
      setError('Passwords do not match');
      return;
    }
    setError('');
    setLoading(true);
    try {
      const result = await importWallet(importKey.trim(), password);
      onComplete(result.address, importKey.trim());
    } catch (e: any) {
      setError(e.message || 'Invalid private key');
    } finally {
      setLoading(false);
    }
  }

  function handleBackupConfirm() {
    onComplete(address, backupKey);
  }

  if (step === 'choose') {
    return (
      <div className="flex flex-col items-center justify-center h-full px-6">
        <img src="icons/logo.png" alt="Daemon" className="w-40 h-40 object-contain mb-2" />
        <h1 className="text-xl font-semibold mb-1">Daemon</h1>
        <p className="text-sm text-daemon-dim mb-8 text-center">
          The first living wallet. AI chat powered&nbsp;by&nbsp;your&nbsp;USDC.
        </p>
        <button
          onClick={() => setStep('create-password')}
          className="w-full py-3 rounded-xl bg-daemon-accent text-daemon-bg font-semibold text-sm hover:bg-daemon-accent/90 transition-colors glow-accent-sm mb-3"
        >
          Create New Wallet
        </button>
        <button
          onClick={() => setStep('import-key')}
          className="w-full py-3 rounded-xl bg-daemon-surface border border-daemon-border text-daemon-text font-medium text-sm hover:border-daemon-accent/30 transition-colors"
        >
          Import Wallet
        </button>
      </div>
    );
  }

  if (step === 'create-password') {
    return (
      <div className="flex flex-col h-full px-6 pt-8">
        <button onClick={() => { setStep('choose'); setError(''); }} className="text-daemon-dim text-sm mb-4 text-left hover:text-daemon-text">
          ← Back
        </button>
        <h2 className="text-lg font-semibold mb-1">Set a Password</h2>
        <p className="text-xs text-daemon-dim mb-6">This encrypts your private key locally.</p>
        <input
          type="password"
          placeholder="Password (min 8 characters)"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
          className="w-full bg-daemon-surface border border-daemon-border rounded-lg px-4 py-3 text-sm text-daemon-text placeholder-daemon-dim focus:outline-none focus:border-daemon-accent/50 mb-3"
        />
        <input
          type="password"
          placeholder="Confirm password"
          value={confirmPassword}
          onChange={(e) => setConfirmPassword(e.target.value)}
          className="w-full bg-daemon-surface border border-daemon-border rounded-lg px-4 py-3 text-sm text-daemon-text placeholder-daemon-dim focus:outline-none focus:border-daemon-accent/50 mb-4"
        />
        {error && <p className="text-red-400 text-xs mb-3">{error}</p>}
        <button
          onClick={handleCreate}
          disabled={loading}
          className="w-full py-3 rounded-xl bg-daemon-accent text-daemon-bg font-semibold text-sm hover:bg-daemon-accent/90 transition-colors disabled:opacity-50"
        >
          {loading ? 'Creating...' : 'Create Wallet'}
        </button>
      </div>
    );
  }

  if (step === 'create-backup') {
    return (
      <div className="flex flex-col h-full px-6 pt-8">
        <h2 className="text-lg font-semibold mb-1">Backup Your Key</h2>
        <p className="text-xs text-daemon-dim mb-4">
          Save this private key somewhere safe. It's the only way to recover your wallet.
        </p>
        <div className="bg-daemon-surface border border-daemon-border rounded-lg p-4 mb-4">
          <p className="font-mono text-xs text-daemon-accent break-all leading-relaxed select-all">
            {backupKey}
          </p>
        </div>
        <button
          onClick={() => navigator.clipboard.writeText(backupKey)}
          className="w-full py-2 rounded-lg bg-daemon-surface border border-daemon-border text-sm text-daemon-text hover:border-daemon-accent/30 transition-colors mb-4"
        >
          Copy to Clipboard
        </button>
        <label className="flex items-center gap-2 text-sm text-daemon-dim mb-4 cursor-pointer">
          <input
            type="checkbox"
            checked={saved}
            onChange={(e) => setSaved(e.target.checked)}
            className="accent-daemon-accent"
          />
          I've saved my private key
        </label>
        <button
          onClick={handleBackupConfirm}
          disabled={!saved}
          className="w-full py-3 rounded-xl bg-daemon-accent text-daemon-bg font-semibold text-sm hover:bg-daemon-accent/90 transition-colors disabled:opacity-30"
        >
          Continue to Wallet
        </button>
      </div>
    );
  }

  if (step === 'import-key') {
    return (
      <div className="flex flex-col h-full px-6 pt-8">
        <button onClick={() => { setStep('choose'); setError(''); }} className="text-daemon-dim text-sm mb-4 text-left hover:text-daemon-text">
          ← Back
        </button>
        <h2 className="text-lg font-semibold mb-1">Import Wallet</h2>
        <p className="text-xs text-daemon-dim mb-6">Enter your base58-encoded private key.</p>
        <textarea
          placeholder="Private key (base58)"
          value={importKey}
          onChange={(e) => setImportKey(e.target.value)}
          rows={3}
          className="w-full bg-daemon-surface border border-daemon-border rounded-lg px-4 py-3 text-sm font-mono text-daemon-text placeholder-daemon-dim focus:outline-none focus:border-daemon-accent/50 mb-4 resize-none"
        />
        {error && <p className="text-red-400 text-xs mb-3">{error}</p>}
        <button
          onClick={handleImport}
          className="w-full py-3 rounded-xl bg-daemon-accent text-daemon-bg font-semibold text-sm hover:bg-daemon-accent/90 transition-colors"
        >
          Next
        </button>
      </div>
    );
  }

  if (step === 'import-password') {
    return (
      <div className="flex flex-col h-full px-6 pt-8">
        <button onClick={() => { setStep('import-key'); setError(''); }} className="text-daemon-dim text-sm mb-4 text-left hover:text-daemon-text">
          ← Back
        </button>
        <h2 className="text-lg font-semibold mb-1">Set a Password</h2>
        <p className="text-xs text-daemon-dim mb-6">Encrypt your imported key.</p>
        <input
          type="password"
          placeholder="Password (min 8 characters)"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
          className="w-full bg-daemon-surface border border-daemon-border rounded-lg px-4 py-3 text-sm text-daemon-text placeholder-daemon-dim focus:outline-none focus:border-daemon-accent/50 mb-3"
        />
        <input
          type="password"
          placeholder="Confirm password"
          value={confirmPassword}
          onChange={(e) => setConfirmPassword(e.target.value)}
          className="w-full bg-daemon-surface border border-daemon-border rounded-lg px-4 py-3 text-sm text-daemon-text placeholder-daemon-dim focus:outline-none focus:border-daemon-accent/50 mb-4"
        />
        {error && <p className="text-red-400 text-xs mb-3">{error}</p>}
        <button
          onClick={handleImportConfirm}
          disabled={loading}
          className="w-full py-3 rounded-xl bg-daemon-accent text-daemon-bg font-semibold text-sm hover:bg-daemon-accent/90 transition-colors disabled:opacity-50"
        >
          {loading ? 'Importing...' : 'Import & Encrypt'}
        </button>
      </div>
    );
  }

  return null;
}
