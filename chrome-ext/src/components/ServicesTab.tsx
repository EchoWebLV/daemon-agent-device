import React, { useState, useEffect, useCallback } from 'react';
import {
  fetchServices,
  saveCustomService,
  removeCustomService,
  CATEGORY_META,
  type X402Service,
  type X402Category,
} from '../lib/x402-services';
import { parseSkillText, skillToService } from '../lib/skill-parser';
import { getStorageValue, setStorageValue, StorageKeys } from '../lib/storage';

interface Props {
  onEnabledChange?: (ids: string[]) => void;
}

export function ServicesTab({ onEnabledChange }: Props) {
  const [services, setServices] = useState<X402Service[]>([]);
  const [enabledIds, setEnabledIds] = useState<string[]>([]);
  const [search, setSearch] = useState('');
  const [activeFilter, setActiveFilter] = useState<X402Category | 'all'>('all');
  const [loading, setLoading] = useState(true);
  const [showAddModal, setShowAddModal] = useState(false);

  const reloadServices = useCallback(async () => {
    const svcs = await fetchServices();
    setServices(svcs);
  }, []);

  useEffect(() => {
    Promise.all([
      fetchServices(),
      getStorageValue<string[]>(StorageKeys.ENABLED_SERVICES),
    ]).then(([svcs, stored]) => {
      setServices(svcs);
      setEnabledIds(stored || []);
      setLoading(false);
    });
  }, []);

  const toggle = useCallback(async (id: string) => {
    setEnabledIds((prev) => {
      const next = prev.includes(id)
        ? prev.filter((x) => x !== id)
        : [...prev, id];
      setStorageValue(StorageKeys.ENABLED_SERVICES, next);
      onEnabledChange?.(next);
      return next;
    });
  }, [onEnabledChange]);

  const handleAddService = useCallback(async (service: X402Service) => {
    await saveCustomService(service);
    await reloadServices();
    const next = [...enabledIds, service.id];
    setEnabledIds(next);
    await setStorageValue(StorageKeys.ENABLED_SERVICES, next);
    onEnabledChange?.(next);
    setShowAddModal(false);
  }, [enabledIds, onEnabledChange, reloadServices]);

  const handleRemoveCustom = useCallback(async (id: string) => {
    await removeCustomService(id);
    await reloadServices();
    const next = enabledIds.filter((x) => x !== id);
    setEnabledIds(next);
    await setStorageValue(StorageKeys.ENABLED_SERVICES, next);
    onEnabledChange?.(next);
  }, [enabledIds, onEnabledChange, reloadServices]);

  const handleRenameCustom = useCallback(async (service: X402Service, newName: string) => {
    const updated: X402Service = {
      ...service,
      name: newName,
      endpoints: service.endpoints.map((ep) => ({
        ...ep,
        name: newName,
        toolDescription: ep.toolDescription.replace(service.name, newName),
      })),
    };
    await saveCustomService(updated);
    await reloadServices();
  }, [reloadServices]);

  const filtered = services.filter((s) => {
    if (activeFilter !== 'all' && s.category !== activeFilter) return false;
    if (search) {
      const q = search.toLowerCase();
      return (
        s.name.toLowerCase().includes(q) ||
        s.description.toLowerCase().includes(q) ||
        CATEGORY_META[s.category]?.label.toLowerCase().includes(q)
      );
    }
    return true;
  });

  const categories = Object.entries(CATEGORY_META) as [X402Category, { label: string; color: string }][];
  const enabledCount = enabledIds.length;

  if (loading) {
    return (
      <div className="flex items-center justify-center h-full">
        <div className="w-5 h-5 border-2 border-daemon-accent border-t-transparent rounded-full animate-spin" />
      </div>
    );
  }

  return (
    <div className="flex flex-col h-full">
      {showAddModal && (
        <AddServiceModal
          onAdd={handleAddService}
          onClose={() => setShowAddModal(false)}
        />
      )}

      <div className="px-4 pt-4 pb-2 space-y-3">
        <div className="flex items-center gap-2">
          <svg width="16" height="16" viewBox="0 0 16 16" fill="none" className="text-daemon-accent flex-shrink-0">
            <circle cx="8" cy="8" r="7" stroke="currentColor" strokeWidth="1.5" />
            <path d="M5 8h6M8 5v6" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
          </svg>
          <h2 className="text-sm font-semibold text-daemon-text">x402 Services</h2>
          <span className="text-[10px] font-mono ml-auto">
            {enabledCount > 0 ? (
              <span className="text-daemon-accent">{enabledCount} active</span>
            ) : (
              <span className="text-daemon-dim">none active</span>
            )}
          </span>
        </div>

        <p className="text-[11px] text-daemon-dim leading-relaxed">
          Toggle services on to give the AI access. Enabled services are called automatically from chat and paid with your USDC.
        </p>

        <div className="flex gap-2">
          <div className="relative flex-1">
            <svg width="14" height="14" viewBox="0 0 14 14" fill="none"
              className="absolute left-3 top-1/2 -translate-y-1/2 text-daemon-dim pointer-events-none">
              <circle cx="6" cy="6" r="4.5" stroke="currentColor" strokeWidth="1.3" />
              <path d="M9.5 9.5L13 13" stroke="currentColor" strokeWidth="1.3" strokeLinecap="round" />
            </svg>
            <input
              type="text"
              placeholder="Search services..."
              value={search}
              onChange={(e) => setSearch(e.target.value)}
              className="w-full bg-daemon-surface border border-daemon-border rounded-lg pl-9 pr-3 py-2 text-xs text-daemon-text placeholder-daemon-dim focus:outline-none focus:border-daemon-accent/50 transition-colors"
            />
          </div>
          <button
            onClick={() => setShowAddModal(true)}
            className="flex items-center gap-1.5 px-3 py-2 bg-daemon-accent text-daemon-bg rounded-lg text-xs font-medium hover:bg-daemon-accent/90 transition-colors flex-shrink-0"
            title="Add x402 service"
          >
            <svg width="12" height="12" viewBox="0 0 12 12" fill="none">
              <path d="M6 2v8M2 6h8" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
            </svg>
            Add
          </button>
        </div>

        <div className="flex gap-1.5 overflow-x-auto pb-1 -mx-4 px-4 scrollbar-hide">
          <button
            onClick={() => setActiveFilter('all')}
            className={`flex-shrink-0 px-2.5 py-1 rounded-full text-[10px] font-medium transition-colors ${
              activeFilter === 'all'
                ? 'bg-daemon-accent text-daemon-bg'
                : 'bg-daemon-surface text-daemon-dim hover:text-daemon-text border border-daemon-border'
            }`}
          >
            All
          </button>
          {categories.map(([id, meta]) => (
            <button
              key={id}
              onClick={() => setActiveFilter(activeFilter === id ? 'all' : id)}
              className={`flex-shrink-0 px-2.5 py-1 rounded-full text-[10px] font-medium transition-colors ${
                activeFilter === id
                  ? 'text-daemon-bg'
                  : 'bg-daemon-surface text-daemon-dim hover:text-daemon-text border border-daemon-border'
              }`}
              style={activeFilter === id ? { backgroundColor: meta.color } : undefined}
            >
              {meta.label}
            </button>
          ))}
        </div>
      </div>

      <div className="flex-1 overflow-y-auto px-4 pb-4 space-y-2">
        {filtered.length === 0 ? (
          <div className="flex flex-col items-center justify-center h-40 text-center">
            <p className="text-xs text-daemon-dim">No services match your search</p>
          </div>
        ) : (
          filtered.map((service) => (
            <ServiceCard
              key={service.id}
              service={service}
              enabled={enabledIds.includes(service.id)}
              onToggle={() => toggle(service.id)}
              onRemove={service.isCustom ? () => handleRemoveCustom(service.id) : undefined}
              onRename={service.isCustom ? (name) => handleRenameCustom(service, name) : undefined}
            />
          ))
        )}
      </div>
    </div>
  );
}

function ServiceCard({
  service,
  enabled,
  onToggle,
  onRemove,
  onRename,
}: {
  service: X402Service;
  enabled: boolean;
  onToggle: () => void;
  onRemove?: () => void;
  onRename?: (name: string) => void;
}) {
  const meta = CATEGORY_META[service.category] || { label: 'Custom', color: '#a78bfa' };
  const endpointCount = service.endpoints.length;
  const [editing, setEditing] = useState(false);
  const [editName, setEditName] = useState(service.name);

  const commitRename = () => {
    const trimmed = editName.trim();
    if (trimmed && trimmed !== service.name && onRename) {
      onRename(trimmed);
    } else {
      setEditName(service.name);
    }
    setEditing(false);
  };

  return (
    <div
      className={`bg-daemon-surface border rounded-xl p-3.5 transition-all ${
        enabled ? 'border-daemon-accent/40' : 'border-daemon-border'
      }`}
    >
      <div className="flex items-start gap-3">
        <button
          onClick={onToggle}
          className={`mt-0.5 flex-shrink-0 w-9 h-5 rounded-full transition-colors relative ${
            enabled ? 'bg-daemon-accent' : 'bg-daemon-border'
          }`}
        >
          <span
            className={`absolute top-0.5 w-4 h-4 rounded-full bg-white transition-transform ${
              enabled ? 'left-[18px]' : 'left-0.5'
            }`}
          />
        </button>

        <div className="flex-1 min-w-0">
          <div className="flex items-center gap-2 mb-1">
            <div className="w-2 h-2 rounded-full flex-shrink-0" style={{ backgroundColor: meta.color }} />
            {editing ? (
              <input
                autoFocus
                value={editName}
                onChange={(e) => setEditName(e.target.value)}
                onBlur={commitRename}
                onKeyDown={(e) => {
                  if (e.key === 'Enter') commitRename();
                  if (e.key === 'Escape') { setEditName(service.name); setEditing(false); }
                }}
                className="text-sm font-medium text-daemon-text bg-daemon-bg border border-daemon-accent/50 rounded px-1.5 py-0.5 outline-none w-full max-w-[180px]"
              />
            ) : (
              <h3
                className={`text-sm font-medium truncate transition-colors ${
                  enabled ? 'text-daemon-text' : 'text-daemon-dim'
                } ${onRename ? 'cursor-pointer hover:text-daemon-accent' : ''}`}
                onClick={onRename ? () => { setEditName(service.name); setEditing(true); } : undefined}
                title={onRename ? 'Click to rename' : undefined}
              >
                {service.name}
              </h3>
            )}
            {service.isCustom && !editing && (
              <span className="text-[9px] font-mono px-1.5 py-0.5 rounded bg-daemon-accent/15 text-daemon-accent">
                custom
              </span>
            )}
            {!editing && <StatusBadge status={service.status} />}
          </div>

          <p className="text-[11px] text-daemon-dim leading-relaxed mb-2 line-clamp-2">
            {service.description}
          </p>

          <div className="flex items-center gap-2 flex-wrap">
            <span
              className="text-[10px] font-medium px-1.5 py-0.5 rounded"
              style={{ backgroundColor: `${meta.color}18`, color: meta.color }}
            >
              {meta.label}
            </span>
            <span className="text-[10px] text-daemon-dim">
              {endpointCount} tool{endpointCount !== 1 ? 's' : ''}
            </span>
            {onRename && !editing && (
              <button
                onClick={(e) => { e.stopPropagation(); setEditName(service.name); setEditing(true); }}
                className="text-[10px] text-daemon-accent/70 hover:text-daemon-accent transition-colors"
                title="Rename service"
              >
                rename
              </button>
            )}
            {onRemove && (
              <button
                onClick={(e) => { e.stopPropagation(); onRemove(); }}
                className="text-[10px] text-red-400/70 hover:text-red-400 transition-colors"
                title="Remove custom service"
              >
                remove
              </button>
            )}
            <span className="text-[10px] text-daemon-dim font-mono ml-auto">
              {service.priceRange}
            </span>
          </div>
        </div>
      </div>
    </div>
  );
}

function StatusBadge({ status }: { status: X402Service['status'] }) {
  const config = {
    live: { dot: '#34d399', text: 'Live' },
    beta: { dot: '#fbbf24', text: 'Beta' },
    'coming-soon': { dot: '#888888', text: 'Soon' },
  }[status];

  return (
    <span className="flex items-center gap-1 flex-shrink-0">
      <span className="w-1.5 h-1.5 rounded-full" style={{ backgroundColor: config.dot }} />
      <span className="text-[10px] text-daemon-dim">{config.text}</span>
    </span>
  );
}

function AddServiceModal({
  onAdd,
  onClose,
}: {
  onAdd: (service: X402Service) => void;
  onClose: () => void;
}) {
  const [skillText, setSkillText] = useState('');
  const [preview, setPreview] = useState<X402Service | null>(null);
  const [customName, setCustomName] = useState('');
  const [error, setError] = useState('');

  const handleParse = useCallback(() => {
    setError('');
    setPreview(null);
    setCustomName('');
    try {
      const parsed = parseSkillText(skillText);
      const service = skillToService(parsed);
      setPreview(service);
      setCustomName(service.name);
    } catch (e: any) {
      setError(e.message || 'Failed to parse skill definition');
    }
  }, [skillText]);

  const handleDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    const file = e.dataTransfer.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = (ev) => {
      const text = ev.target?.result as string;
      if (text) setSkillText(text);
    };
    reader.readAsText(file);
  }, []);

  return (
    <div className="absolute inset-0 z-50 bg-daemon-bg flex flex-col">
      <div className="flex items-center justify-between px-4 py-3 border-b border-daemon-border">
        <h2 className="text-sm font-semibold text-daemon-text">Add x402 Service</h2>
        <button
          onClick={onClose}
          className="text-daemon-dim hover:text-daemon-text transition-colors p-1"
        >
          <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
            <path d="M3 3l8 8M11 3L3 11" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
          </svg>
        </button>
      </div>

      <div className="flex-1 overflow-y-auto px-4 py-4 space-y-4">
        <p className="text-[11px] text-daemon-dim leading-relaxed">
          Paste an x402 skill definition or drop a skill.md file. Any x402 endpoint works — Frames, custom APIs, or any service that supports HTTP 402.
        </p>

        <div
          onDragOver={(e) => e.preventDefault()}
          onDrop={handleDrop}
          className="relative"
        >
          <textarea
            value={skillText}
            onChange={(e) => { setSkillText(e.target.value); setPreview(null); setError(''); }}
            placeholder={`Endpoint: POST https://example.com/api/call\nPrice: $0.01 per call\n\nDescription of what this service does...`}
            rows={8}
            className="w-full bg-daemon-surface border border-daemon-border rounded-xl px-4 py-3 text-xs text-daemon-text placeholder-daemon-dim focus:outline-none focus:border-daemon-accent/50 resize-none font-mono transition-colors"
          />
          {!skillText && (
            <div className="absolute inset-0 flex items-center justify-center pointer-events-none">
              <p className="text-[10px] text-daemon-dim/40 mt-16">or drag & drop a .md file</p>
            </div>
          )}
        </div>

        <button
          onClick={handleParse}
          disabled={!skillText.trim()}
          className="w-full py-2.5 rounded-xl bg-daemon-surface border border-daemon-border text-xs font-medium text-daemon-text hover:border-daemon-accent/50 transition-colors disabled:opacity-30"
        >
          Parse Skill
        </button>

        {error && (
          <div className="px-3 py-2 rounded-lg bg-red-500/10 border border-red-500/20">
            <p className="text-xs text-red-400">{error}</p>
          </div>
        )}

        {preview && (
          <div className="space-y-3">
            <div className="px-3 py-2 rounded-lg bg-daemon-accent/10 border border-daemon-accent/20">
              <p className="text-xs text-daemon-accent font-medium mb-1">Preview</p>
            </div>

            <div className="bg-daemon-surface border border-daemon-border rounded-xl p-3.5 space-y-2">
              <div className="space-y-1.5">
                <label className="text-[10px] text-daemon-dim font-medium">Service Name</label>
                <input
                  value={customName}
                  onChange={(e) => setCustomName(e.target.value)}
                  className="w-full bg-daemon-bg border border-daemon-border rounded-lg px-3 py-1.5 text-sm font-medium text-daemon-text focus:outline-none focus:border-daemon-accent/50 transition-colors"
                  placeholder="Enter a name for this service"
                />
              </div>
              <p className="text-[11px] text-daemon-dim leading-relaxed">
                {preview.description}
              </p>
              <div className="space-y-1">
                {preview.endpoints.map((ep) => (
                  <div key={ep.id} className="text-[10px] font-mono text-daemon-dim bg-daemon-bg/50 rounded px-2 py-1.5">
                    <span className="text-daemon-accent">{ep.method}</span>{' '}
                    <span className="text-daemon-text">{preview.baseUrl}{ep.path}</span>
                    {ep.params.length > 0 && (
                      <div className="mt-1 text-daemon-dim">
                        Params: {ep.params.map((p) => `${p.name}${p.required ? '*' : ''}`).join(', ')}
                      </div>
                    )}
                  </div>
                ))}
              </div>
              <div className="flex items-center gap-2 text-[10px]">
                <span
                  className="font-medium px-1.5 py-0.5 rounded"
                  style={{
                    backgroundColor: `${(CATEGORY_META[preview.category] || { color: '#a78bfa' }).color}18`,
                    color: (CATEGORY_META[preview.category] || { color: '#a78bfa' }).color,
                  }}
                >
                  {(CATEGORY_META[preview.category] || { label: 'Custom' }).label}
                </span>
                <span className="text-daemon-dim font-mono">{preview.priceRange}</span>
              </div>
            </div>

            <button
              onClick={() => {
                const trimmed = customName.trim();
                const finalService = trimmed && trimmed !== preview.name
                  ? {
                      ...preview,
                      name: trimmed,
                      endpoints: preview.endpoints.map((ep) => ({ ...ep, name: trimmed })),
                    }
                  : preview;
                onAdd(finalService);
              }}
              disabled={!customName.trim()}
              className="w-full py-3 rounded-xl bg-daemon-accent text-daemon-bg font-semibold text-sm hover:bg-daemon-accent/90 transition-colors disabled:opacity-30"
            >
              Add & Enable Service
            </button>
          </div>
        )}
      </div>
    </div>
  );
}
