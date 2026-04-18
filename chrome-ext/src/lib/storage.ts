export const StorageKeys = {
  WALLET_ENCRYPTED: 'wallet_encrypted',
  WALLET_ADDRESS: 'wallet_address',
  WALLET_NETWORK: 'wallet_network',
  LAST_MODEL: 'last_model',
  CHAT_HISTORY: 'chat_history',
  SESSION_SPEND: 'session_spend',
  USDC_BALANCE: 'usdc_balance',
  SOL_BALANCE: 'sol_balance',
  CUSTOM_RPC: 'custom_rpc',
  ENABLED_SERVICES: 'enabled_services',
  CUSTOM_SERVICES: 'custom_services',
} as const;

type StorageKey = (typeof StorageKeys)[keyof typeof StorageKeys];

export async function getStorageValue<T>(key: StorageKey): Promise<T | null> {
  return new Promise((resolve) => {
    chrome.storage.local.get(key, (result) => {
      resolve((result[key] as T) ?? null);
    });
  });
}

export async function setStorageValue<T>(key: StorageKey, value: T): Promise<void> {
  return new Promise((resolve) => {
    chrome.storage.local.set({ [key]: value }, resolve);
  });
}

export async function removeStorageValue(key: StorageKey): Promise<void> {
  return new Promise((resolve) => {
    chrome.storage.local.remove(key, resolve);
  });
}

export function onStorageChanged(
  callback: (changes: { [key: string]: chrome.storage.StorageChange }) => void,
): () => void {
  const listener = (changes: { [key: string]: chrome.storage.StorageChange }, area: string) => {
    if (area === 'local') callback(changes);
  };
  chrome.storage.onChanged.addListener(listener);
  return () => chrome.storage.onChanged.removeListener(listener);
}

const SESSION_TIMEOUT_MS = 3 * 60 * 1000;
const SK_SESSION_KEY = '_session_key';
const SK_SESSION_TS = '_session_ts';

export async function saveSession(privateKeyBs58: string): Promise<void> {
  await chrome.storage.session.set({
    [SK_SESSION_KEY]: privateKeyBs58,
    [SK_SESSION_TS]: Date.now(),
  });
}

export async function getSession(): Promise<string | null> {
  const result = await chrome.storage.session.get([SK_SESSION_KEY, SK_SESSION_TS]);
  const key = result[SK_SESSION_KEY] as string | undefined;
  const ts = result[SK_SESSION_TS] as number | undefined;
  if (!key || !ts) return null;
  if (Date.now() - ts > SESSION_TIMEOUT_MS) {
    await clearSession();
    return null;
  }
  return key;
}

export async function clearSession(): Promise<void> {
  await chrome.storage.session.remove([SK_SESSION_KEY, SK_SESSION_TS]);
}
