import type { Message } from '../types';

const DB_NAME = 'daemon_chats';
const DB_VERSION = 1;
const CONV_STORE = 'conversations';
const MSG_STORE = 'messages';

export interface Conversation {
  id: string;
  title: string;
  model: string;
  createdAt: number;
  updatedAt: number;
  messageCount: number;
}

function open(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(CONV_STORE)) {
        const cs = db.createObjectStore(CONV_STORE, { keyPath: 'id' });
        cs.createIndex('updatedAt', 'updatedAt');
      }
      if (!db.objectStoreNames.contains(MSG_STORE)) {
        const ms = db.createObjectStore(MSG_STORE, { keyPath: 'id' });
        ms.createIndex('conversationId', 'conversationId');
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

function tx(
  db: IDBDatabase,
  stores: string | string[],
  mode: IDBTransactionMode,
): IDBTransaction {
  return db.transaction(stores, mode);
}

export async function getAllConversations(): Promise<Conversation[]> {
  const db = await open();
  return new Promise((resolve, reject) => {
    const t = tx(db, CONV_STORE, 'readonly');
    const store = t.objectStore(CONV_STORE);
    const idx = store.index('updatedAt');
    const req = idx.getAll();
    req.onsuccess = () => {
      const results = (req.result as Conversation[]).reverse();
      resolve(results);
    };
    req.onerror = () => reject(req.error);
  });
}

export async function getConversation(id: string): Promise<Conversation | undefined> {
  const db = await open();
  return new Promise((resolve, reject) => {
    const t = tx(db, CONV_STORE, 'readonly');
    const req = t.objectStore(CONV_STORE).get(id);
    req.onsuccess = () => resolve(req.result as Conversation | undefined);
    req.onerror = () => reject(req.error);
  });
}

export async function createConversation(model: string, title?: string): Promise<Conversation> {
  const conv: Conversation = {
    id: crypto.randomUUID(),
    title: title || 'New chat',
    model,
    createdAt: Date.now(),
    updatedAt: Date.now(),
    messageCount: 0,
  };
  const db = await open();
  return new Promise((resolve, reject) => {
    const t = tx(db, CONV_STORE, 'readwrite');
    const req = t.objectStore(CONV_STORE).put(conv);
    req.onsuccess = () => resolve(conv);
    req.onerror = () => reject(req.error);
  });
}

export async function updateConversation(
  id: string,
  patch: Partial<Pick<Conversation, 'title' | 'model' | 'updatedAt' | 'messageCount'>>,
): Promise<void> {
  const db = await open();
  const existing = await getConversation(id);
  if (!existing) return;
  const updated = { ...existing, ...patch, updatedAt: Date.now() };
  return new Promise((resolve, reject) => {
    const t = tx(db, CONV_STORE, 'readwrite');
    const req = t.objectStore(CONV_STORE).put(updated);
    req.onsuccess = () => resolve();
    req.onerror = () => reject(req.error);
  });
}

export async function deleteConversation(id: string): Promise<void> {
  const db = await open();
  const msgs = await getMessages(id);
  return new Promise((resolve, reject) => {
    const t = tx(db, [CONV_STORE, MSG_STORE], 'readwrite');
    t.objectStore(CONV_STORE).delete(id);
    const msgStore = t.objectStore(MSG_STORE);
    for (const m of msgs) msgStore.delete(m.id);
    t.oncomplete = () => resolve();
    t.onerror = () => reject(t.error);
  });
}

export interface StoredMessage extends Message {
  conversationId: string;
}

export async function getMessages(conversationId: string): Promise<StoredMessage[]> {
  const db = await open();
  return new Promise((resolve, reject) => {
    const t = tx(db, MSG_STORE, 'readonly');
    const idx = t.objectStore(MSG_STORE).index('conversationId');
    const req = idx.getAll(conversationId);
    req.onsuccess = () => {
      const msgs = (req.result as StoredMessage[]).sort((a, b) => a.timestamp - b.timestamp);
      resolve(msgs);
    };
    req.onerror = () => reject(req.error);
  });
}

export async function addMessage(conversationId: string, message: Message): Promise<void> {
  const db = await open();
  const stored: StoredMessage = { ...message, conversationId };
  return new Promise((resolve, reject) => {
    const t = tx(db, MSG_STORE, 'readwrite');
    const req = t.objectStore(MSG_STORE).put(stored);
    req.onsuccess = () => resolve();
    req.onerror = () => reject(req.error);
  });
}

export async function addMessages(conversationId: string, messages: Message[]): Promise<void> {
  if (messages.length === 0) return;
  const db = await open();
  return new Promise((resolve, reject) => {
    const t = tx(db, MSG_STORE, 'readwrite');
    const store = t.objectStore(MSG_STORE);
    for (const m of messages) {
      store.put({ ...m, conversationId });
    }
    t.oncomplete = () => resolve();
    t.onerror = () => reject(t.error);
  });
}

function deriveTitle(content: string): string {
  const cleaned = content.replace(/\n/g, ' ').trim();
  if (cleaned.length <= 40) return cleaned;
  return cleaned.slice(0, 40).trimEnd() + '...';
}

export async function saveMessagesAndUpdateConv(
  conversationId: string,
  newMessages: Message[],
): Promise<void> {
  await addMessages(conversationId, newMessages);
  const allMsgs = await getMessages(conversationId);
  const conv = await getConversation(conversationId);
  if (!conv) return;

  const firstUserMsg = allMsgs.find((m) => m.role === 'user');
  const title = conv.title === 'New chat' && firstUserMsg
    ? deriveTitle(firstUserMsg.content)
    : conv.title;

  await updateConversation(conversationId, {
    title,
    messageCount: allMsgs.length,
  });
}
