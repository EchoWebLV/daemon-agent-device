import React from 'react';

interface BalanceBadgeProps {
  usdcBalance: number;
  solBalance?: number;
  compact?: boolean;
}

export function BalanceBadge({ usdcBalance, solBalance, compact }: BalanceBadgeProps) {
  if (compact) {
    return (
      <div className="flex items-center gap-1.5 rounded-full bg-daemon-surface px-3 py-1 text-xs font-mono border border-daemon-border">
        <span className="text-daemon-accent font-medium">${usdcBalance.toFixed(2)}</span>
        <span className="text-daemon-dim">USDC</span>
      </div>
    );
  }

  return (
    <div className="space-y-2">
      <div className="flex items-center justify-between rounded-xl bg-daemon-surface p-4 border border-daemon-border">
        <div>
          <p className="text-xs text-daemon-dim uppercase tracking-wider">USDC Balance</p>
          <p className="text-2xl font-semibold font-mono text-daemon-accent mt-1">
            ${usdcBalance.toFixed(2)}
          </p>
        </div>
        <div className="w-10 h-10 rounded-full bg-daemon-accent/10 flex items-center justify-center">
          <span className="text-daemon-accent text-lg font-bold">$</span>
        </div>
      </div>
      {solBalance !== undefined && (
        <div className="flex items-center justify-between rounded-xl bg-daemon-surface p-4 border border-daemon-border">
          <div>
            <p className="text-xs text-daemon-dim uppercase tracking-wider">SOL Balance</p>
            <p className="text-lg font-semibold font-mono text-daemon-text mt-1">
              {solBalance.toFixed(4)} SOL
            </p>
          </div>
          <div className="w-10 h-10 rounded-full bg-purple-500/10 flex items-center justify-center">
            <span className="text-purple-400 text-sm font-bold">◎</span>
          </div>
        </div>
      )}
    </div>
  );
}
