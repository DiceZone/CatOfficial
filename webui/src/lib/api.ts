// 管理面板接口。会话靠 HttpOnly Cookie，所有请求同源发出。

export class ApiError extends Error {
  constructor(public status: number, message: string) { super(message); }
}

let onUnauthorized: (() => void) | null = null;
export function setUnauthorizedHandler(handler: () => void) { onUnauthorized = handler; }

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const response = await fetch(`./api/webui/${path}`, {
    method,
    credentials: 'same-origin',
    headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await response.text();
  const data = text ? JSON.parse(text) : null;
  if (!response.ok) {
    if (response.status === 401 && path !== 'login') onUnauthorized?.();
    throw new ApiError(response.status, (data && data.error) || response.statusText);
  }
  return data as T;
}

export interface Listener { type: 'http' | 'ws'; host: string; port: number }
export interface PostState { url: string; last?: { ok: boolean; status: number; at: number; error?: string } }
export interface ReverseState { url: string; connected: boolean; since?: number; error?: string }
export interface Status {
  configured: boolean;
  online: boolean;
  self_id: number;
  nickname: string;
  avatar: string;
  app_id: string;
  mode: 'gateway' | 'webhook';
  version: string;
  started_at: number;
  received: number;
  sent: number;
  connections: { listeners: Listener[]; ws_clients: number; http_post: PostState[]; ws_reverse: ReverseState[] };
  counts: { users: number; groups: number; bound: number; messages: number };
  webhook: { host: string; port: number; path: string } | null;
}

export interface Identity { type: 'user' | 'group'; openid: string; number: number; bound: boolean; direct: boolean; name: string }

export interface MessageRow {
  id: number; time: number; self: boolean; scene: 'private' | 'group';
  content: string; text: string; peer: number; peer_name: string; sender?: number; sender_name?: string;
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type ConfigJson = Record<string, any>;

export const api = {
  session: () => request<{ authenticated: boolean }>('GET', 'session'),
  login: (password: string) => request<{ ok: boolean }>('POST', 'login', { password }),
  logout: () => request<{ ok: boolean }>('POST', 'logout'),
  status: () => request<Status>('GET', 'status'),
  config: () => request<{ config: ConfigJson; app_secret_set: boolean }>('GET', 'config'),
  saveConfig: (config: ConfigJson, newPassword: string) =>
    request<{ ok: boolean; restart_required: boolean }>('PUT', 'config', { config, new_password: newPassword }),
  restart: () => request<{ ok: boolean }>('POST', 'restart'),
  bindings: () => request<Identity[]>('GET', 'bindings'),
  bind: (body: { type: string; user_id?: number; group_id?: number; openid?: string; qq: number }) =>
    request<{ ok: boolean }>('POST', 'bindings', body),
  unbind: (type: string, openid: string) => request<{ ok: boolean; number: number }>('DELETE', 'bindings', { type, openid }),
  identities: (q: string) => request<Identity[]>('GET', `identities?q=${encodeURIComponent(q)}`),
  messages: (before = 0, limit = 50) => request<MessageRow[]>('GET', `messages?limit=${limit}&before=${before}`),
};
