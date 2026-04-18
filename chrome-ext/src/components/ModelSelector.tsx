import React, { useState, useRef, useEffect } from 'react';
import { MODELS, type ModelInfo } from '../types';

interface ModelSelectorProps {
  selectedModel: string;
  onModelChange: (modelId: string) => void;
}

export function ModelSelector({ selectedModel, onModelChange }: ModelSelectorProps) {
  const [open, setOpen] = useState(false);
  const ref = useRef<HTMLDivElement>(null);
  const selected = MODELS.find((m) => m.id === selectedModel);

  useEffect(() => {
    function handleClickOutside(e: MouseEvent) {
      if (ref.current && !ref.current.contains(e.target as Node)) setOpen(false);
    }
    if (open) document.addEventListener('mousedown', handleClickOutside);
    return () => document.removeEventListener('mousedown', handleClickOutside);
  }, [open]);

  return (
    <div ref={ref} className="relative">
      <button
        onClick={() => setOpen((o) => !o)}
        className="w-full flex items-center justify-between bg-daemon-surface border border-daemon-border rounded-lg px-3 py-2 text-sm text-daemon-text hover:border-daemon-accent/40 focus:outline-none focus:border-daemon-accent/50 transition-colors cursor-pointer"
      >
        <span className="truncate">
          {selected ? selected.name : 'Select model'}
        </span>
        <svg
          width="12" height="12" viewBox="0 0 12 12" fill="none"
          className={`flex-shrink-0 ml-2 text-daemon-dim transition-transform ${open ? 'rotate-180' : ''}`}
        >
          <path d="M3 4.5L6 7.5L9 4.5" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
        </svg>
      </button>

      {open && (
        <div className="absolute left-0 right-0 top-full mt-1 z-[9999] bg-daemon-bg border border-daemon-border rounded-xl shadow-2xl shadow-black/50 overflow-hidden">
          <div className="max-h-72 overflow-y-auto py-1 scrollbar-hide">
            {MODELS.map((model) => {
              const active = model.id === selectedModel;
              return (
                <button
                  key={model.id}
                  onClick={() => { onModelChange(model.id); setOpen(false); }}
                  className={`w-full text-left px-3 py-2 transition-colors ${
                    active
                      ? 'bg-daemon-accent/10 text-daemon-accent'
                      : 'text-daemon-text hover:bg-daemon-surface'
                  }`}
                >
                  <div className="flex items-center justify-between">
                    <span className="text-xs font-medium truncate">{model.name}</span>
                    <span className="text-[10px] text-daemon-dim font-mono flex-shrink-0 ml-2">
                      ~${formatEstimate(model)}/msg
                    </span>
                  </div>
                  <div className="text-[10px] text-daemon-dim mt-0.5">
                    {model.provider} · ${model.inputPrice}/M in · ${model.outputPrice}/M out
                  </div>
                </button>
              );
            })}
          </div>
        </div>
      )}
    </div>
  );
}

function formatEstimate(model: ModelInfo): string {
  return ((model.inputPrice * 500 + model.outputPrice * 500) / 1_000_000).toFixed(4);
}
