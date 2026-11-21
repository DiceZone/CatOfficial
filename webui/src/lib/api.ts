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
  let data: unknown = null;
  try { data = text ? JSON.parse(text) : null; } catch { data = null; }
  if (!response.ok) {
    if (response.status === 401 && path !== 'login') onUnauthorized?.();
    const message = data && typeof data === 'object' && 'error' in data ? String((data as { error: unknown }).error) : response.statusText;
    throw new ApiError(response.status, message);
  }
  return data as T;
}

export interface OfficialRuntime {
  configured?: boolean; online: boolean; self_id: number; nickname: string; avatar?: string;
  app_id?: string; mode?: string; received?: number; sent?: number; error?: string; running?: boolean;
}

export interface Bot {
  id: string;
  name: string;
  enabled: boolean;
  type: 'official' | 'onebot';
  app_id?: string;
  app_secret?: string;
  app_secret_set?: boolean;
  mode?: 'gateway' | 'webhook';
  subscribe_group_member_events?: boolean;
  auto_ack_interactions?: boolean;
  url?: string;
  token?: string;
  token_set?: boolean;
  runtime?: OfficialRuntime;
}

export type ConnectionType = 'http_server' | 'ws_server' | 'http_client' | 'ws_client';

export interface ConnectionRuntime {
  listening?: boolean;
  clients?: number;
  reverse?: Record<string, { connected: boolean; since?: number; error?: string }>;
  last_post?: { ok: boolean; status: number; at: number; error?: string };
}

export interface Connection {
  id: string;
  name: string;
  enabled: boolean;
  type: ConnectionType;
  host?: string;
  port?: number;
  url?: string;
  token?: string;
  message_format?: 'array' | 'string';
  report_self?: boolean;
  heartbeat_interval?: number;
  reconnect_interval?: number;
  timeout?: number;
  bots?: string[];
  runtime?: ConnectionRuntime;
}

export interface Status {
  version: string;
  started_at: number;
  pending_restart: boolean;
  received: number;
  sent: number;
  bots: { id: string; name: string; type: 'official' | 'onebot'; enabled: boolean; runtime?: OfficialRuntime }[];
  connections: { id: string; name: string; type: ConnectionType; enabled: boolean; host: string; port: number; url: string; runtime: ConnectionRuntime }[];
  counts: { users: number; groups: number; bound: number; messages: number };
}

export interface Identity { type: 'user' | 'group'; openid: string; number: number; bound: boolean; direct: boolean; name: string }

export interface Bindings {
  bound: Identity[];
  pending_groups: { openid: string; number: number; name: string; real_group: number; votes: number }[];
  required_votes: number;
}

export interface MessageRow {
  id: number; time: number; self: boolean; scene: 'private' | 'group';
  content: string; text: string; peer: number; peer_name: string; sender?: number; sender_name?: string;
}

export interface LogEntry { seq: number; time: number; level: 'debug' | 'info' | 'warn' | 'error'; message: string }

export interface Settings {
  webhook: { host?: string; port?: number; path?: string };
  smtp: { enabled?: boolean; host?: string; port?: number; security?: 'ssl' | 'starttls' | 'none'; username?: string; password?: string; from?: string; owner_email?: string; notify_offline?: boolean };
  smtp_password_set?: boolean;
  bind: { enabled?: boolean; command?: string; window_seconds?: number; group_confirm_users?: number; email_enabled?: boolean; email_domain?: string; code_ttl_minutes?: number };
  webui: { enabled?: boolean; host?: string; port?: number; root?: string };
  data_dir?: string;
  log_level?: string;
  message_retention_days?: number;
  new_password?: string;
}

type Saved = { ok: boolean; id?: string; restart_required?: boolean };

export const api = {
  session: () => request<{ authenticated: boolean }>('GET', 'session'),
  login: (password: string) => request<{ ok: boolean }>('POST', 'login', { password }),
  logout: () => request<{ ok: boolean }>('POST', 'logout'),
  status: () => request<Status>('GET', 'status'),
  restart: () => request<{ ok: boolean }>('POST', 'restart'),

  bots: () => request<Bot[]>('GET', 'bots'),
  saveBot: (bot: Partial<Bot>) => (bot.id ? request<Saved>('PUT', `bots/${bot.id}`, bot) : request<Saved>('POST', 'bots', bot)),
  deleteBot: (id: string) => request<Saved>('DELETE', `bots/${id}`),

  connections: () => request<Connection[]>('GET', 'connections'),
  saveConnection: (c: Partial<Connection>) => (c.id ? request<Saved>('PUT', `connections/${c.id}`, c) : request<Saved>('POST', 'connections', c)),
  deleteConnection: (id: string) => request<Saved>('DELETE', `connections/${id}`),

  settings: () => request<Settings>('GET', 'settings'),
  saveSettings: (s: Settings) => request<Saved>('PUT', 'settings', s),
  testSmtp: (smtp: Settings['smtp'], to: string) => request<{ ok: boolean }>('POST', 'smtp/test', { smtp, to }),

  qrStart: () => request<{ session_id: string; url: string }>('POST', 'qr/start'),
  qrPoll: (sessionId: string, bot: Partial<Bot>, botId?: string) =>
    request<{ status: 'pending' | 'expired' | 'completed'; bot_id?: string; app_id?: string }>('POST', `qr/${sessionId}/poll`, { bot, bot_id: botId }),

  logs: (after: number) => request<LogEntry[]>('GET', `logs?after=${after}&limit=1000`),

  bindings: () => request<Bindings>('GET', 'bindings'),
  bind: (body: { type: string; user_id?: number; group_id?: number; openid?: string; qq: number }) => request<{ ok: boolean }>('POST', 'bindings', body),
  unbind: (type: string, openid: string) => request<{ ok: boolean; number: number }>('DELETE', 'bindings', { type, openid }),
  identities: (q: string) => request<Identity[]>('GET', `identities?q=${encodeURIComponent(q)}`),
  messages: (before = 0, limit = 50) => request<MessageRow[]>('GET', `messages?limit=${limit}&before=${before}`),
};
