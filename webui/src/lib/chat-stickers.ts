// Store the actual files so a favourite survives expiry of the chat image URL.
async function openStickers(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open('catofficial.chat.stickers', 1);
    request.onupgradeneeded = () => request.result.createObjectStore('files', { autoIncrement: true });
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

async function withFiles<T>(mode: IDBTransactionMode, action: (files: IDBObjectStore) => IDBRequest<T>): Promise<T> {
  const db = await openStickers();
  try {
    return await new Promise<T>((resolve, reject) => {
      const tx = db.transaction('files', mode);
      const request = action(tx.objectStore('files'));
      tx.oncomplete = () => resolve(request.result);
      tx.onabort = () => reject(tx.error ?? request.error);
    });
  } finally { db.close(); }
}

export const saveSticker = (file: File) => withFiles('readwrite', (files) => files.add(file));
export const listStickers = (): Promise<File[]> => withFiles('readonly', (files) => files.getAll());
