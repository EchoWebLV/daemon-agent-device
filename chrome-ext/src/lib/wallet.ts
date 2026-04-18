import { Keypair } from '@solana/web3.js';
import bs58 from 'bs58';
import type { EncryptedWallet } from '../types';
import { getStorageValue, setStorageValue, StorageKeys } from './storage';

const encoder = new TextEncoder();
const decoder = new TextDecoder();

function toBase64(buffer: ArrayBuffer | Uint8Array): string {
  const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
  return btoa(String.fromCharCode(...bytes));
}

function fromBase64(base64: string): Uint8Array {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

async function deriveKey(password: string, salt: Uint8Array, usage: KeyUsage[]): Promise<CryptoKey> {
  const keyMaterial = await crypto.subtle.importKey('raw', encoder.encode(password), 'PBKDF2', false, [
    'deriveKey',
  ]);
  return crypto.subtle.deriveKey(
    { name: 'PBKDF2', salt: salt.buffer as ArrayBuffer, iterations: 100_000, hash: 'SHA-256' },
    keyMaterial,
    { name: 'AES-GCM', length: 256 },
    false,
    usage,
  );
}

export async function encryptPrivateKey(privateKeyBs58: string, password: string): Promise<EncryptedWallet> {
  const salt = crypto.getRandomValues(new Uint8Array(32));
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const aesKey = await deriveKey(password, salt, ['encrypt']);
  const encrypted = await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, aesKey, encoder.encode(privateKeyBs58));
  return {
    encryptedKey: toBase64(encrypted),
    iv: toBase64(iv),
    salt: toBase64(salt),
  };
}

export async function decryptPrivateKey(wallet: EncryptedWallet, password: string): Promise<string> {
  const salt = fromBase64(wallet.salt);
  const iv = fromBase64(wallet.iv);
  const encryptedData = fromBase64(wallet.encryptedKey);
  const aesKey = await deriveKey(password, salt, ['decrypt']);
  const decrypted = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: iv.buffer as ArrayBuffer }, aesKey, encryptedData.buffer as ArrayBuffer);
  return decoder.decode(decrypted);
}

export function generateKeypair(): { keypair: Keypair; privateKeyBs58: string; publicKeyBs58: string } {
  const keypair = Keypair.generate();
  return {
    keypair,
    privateKeyBs58: bs58.encode(keypair.secretKey),
    publicKeyBs58: keypair.publicKey.toBase58(),
  };
}

export function keypairFromBs58(privateKeyBs58: string): Keypair {
  return Keypair.fromSecretKey(bs58.decode(privateKeyBs58));
}

export async function createWallet(password: string): Promise<{ address: string; backupKey: string }> {
  const { privateKeyBs58, publicKeyBs58 } = generateKeypair();
  const encrypted = await encryptPrivateKey(privateKeyBs58, password);
  await setStorageValue(StorageKeys.WALLET_ENCRYPTED, encrypted);
  await setStorageValue(StorageKeys.WALLET_ADDRESS, publicKeyBs58);
  await setStorageValue(StorageKeys.WALLET_NETWORK, 'mainnet-beta');
  return { address: publicKeyBs58, backupKey: privateKeyBs58 };
}

export async function importWallet(
  privateKeyBs58: string,
  password: string,
): Promise<{ address: string }> {
  const keypair = keypairFromBs58(privateKeyBs58);
  const publicKeyBs58 = keypair.publicKey.toBase58();
  const encrypted = await encryptPrivateKey(privateKeyBs58, password);
  await setStorageValue(StorageKeys.WALLET_ENCRYPTED, encrypted);
  await setStorageValue(StorageKeys.WALLET_ADDRESS, publicKeyBs58);
  await setStorageValue(StorageKeys.WALLET_NETWORK, 'mainnet-beta');
  return { address: publicKeyBs58 };
}

export async function unlockWallet(password: string): Promise<string> {
  const encrypted = await getStorageValue<EncryptedWallet>(StorageKeys.WALLET_ENCRYPTED);
  if (!encrypted) throw new Error('No wallet found');
  return decryptPrivateKey(encrypted, password);
}

export async function hasWallet(): Promise<boolean> {
  const encrypted = await getStorageValue<EncryptedWallet>(StorageKeys.WALLET_ENCRYPTED);
  return encrypted !== null;
}
