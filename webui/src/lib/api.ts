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
  configured?: boolean; online: boolean; self_id: number; nickname: string; display_name?: string; avatar?: string; listening?: boolean;
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
  mode?: 'websocket' | 'webhook';
  subscribe_group_member_events?: boolean;
  auto_ack_interactions?: boolean;
  connection?: 'reverse' | 'forward';
  host?: string;
  port?: number;
  url?: string;
  token?: string;
  token_set?: boolean;
  runtime?: OfficialRuntime;
}

/// 显示名：用户起的名字；没起时用机器人昵称，再没有用 AppID 或 id。
export function botLabel(b: Pick<Bot, 'name' | 'app_id' | 'id' | 'runtime'>): string {
  return b.name || b.runtime?.display_name || b.runtime?.nickname || b.app_id || b.id;
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

export interface MessageImage { url: string; src: string }
export type MessagePart =
  | { type: 'text'; text: string }
  | { type: 'markdown'; text: string }
  | { type: 'image'; url: string; src: string }
  | { type: 'reply'; id: number };

export interface MessageRow {
  id: number; time: number; self: boolean; scene: 'private' | 'group' | 'channel'; bot: string; bot_name?: string;
  content: string; text: string; markdown: string; images: MessageImage[]; parts: MessagePart[];
  peer: number; peer_openid: string; peer_name: string;
  sender?: number; sender_openid?: string; sender_name?: string; sender_card?: string; avatar?: string;
}

export interface MessageQuery {
  page: number; size: number; order: 'asc' | 'desc'; q?: string; bot?: string; peer?: string; sender?: string;
  scene?: '' | 'private' | 'group'; from?: number; to?: number;
}

export interface MessageFilters {
  bots: { id: string; name: string }[];
  senders: { openid: string; number: number; name: string }[];
  conversations: { openid: string; number: number; name: string; scene: 'private' | 'group' }[];
}

export interface Conversation {
  peer: string; bot: string; scene: 'private' | 'group'; number: number; name: string; time: number; count: number;
  avatar?: string; last: MessageRow;
}

export type MatchType = 'keyword' | 'search' | 'prefix' | 'regex';
export interface ReplyRule {
  id: string; name: string; enabled: boolean; builtin: boolean; priority: number;
  conditions: { type: MatchType; content: string }[]; logic: 'or' | 'and';
  results: string[]; weights: number[]; format: 'plain' | 'markdown'; scope: 'all' | 'group' | 'private';
  bots: string[]; prob: number; cooldown_sec: number; pass_through: boolean;
}
export interface ReplyVariable { name: string; desc: string }

export interface Material { image_id: string; url: string; file_name?: string; preview_url?: string; created_at?: number; expires_at?: number }

export interface DiskInfo { path: string; mount: string; label: string; fs: string; model: string; total_gb: number; used_gb: number; load: number }
export interface SystemInfo {
  os: string; os_id: string; cpu_model: string; cpu_cores: number; cpu_physical: number; cpu_mhz: number; cpu_load: number;
  mem_total_mb: number; mem_used_mb: number; mem_load: number; mem_speed_mhz: number; proc_mem_mb: number; disks: DiskInfo[];
}

export interface LogEntry { seq: number; time: number; level: 'debug' | 'info' | 'warn' | 'error'; message: string }

export interface Settings {
  webhook: { host?: string; port?: number; path?: string; standalone?: boolean };
  smtp: { enabled?: boolean; host?: string; port?: number; security?: 'ssl' | 'starttls' | 'none'; username?: string; password?: string; from?: string; notify_emails?: string[]; notify_errors?: boolean };
  masters?: { bot_id: string; openid: string }[];
  notify_masters?: boolean;
  smtp_password_set?: boolean;
  bind: { enabled?: boolean; command?: string; window_seconds?: number; group_confirm_users?: number; email_enabled?: boolean; code_ttl_minutes?: number };
  webui: { enabled?: boolean; host?: string; port?: number; root?: string };
  log_level?: string;
  message_retention_days?: number;
  image_cache_days?: number;
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
  testSmtp: (smtp: Settings['smtp']) => request<{ ok: boolean }>('POST', 'smtp/test', { smtp }),

  qrStart: () => request<{ session_id: string; url: string }>('POST', 'qr/start'),
  qrPoll: (sessionId: string, bot: Partial<Bot>, botId?: string) =>
    request<{ status: 'pending' | 'expired' | 'completed'; bot_id?: string; app_id?: string }>('POST', `qr/${sessionId}/poll`, { bot, bot_id: botId }),

  logs: (after: number) => request<LogEntry[]>('GET', `logs?after=${after}&limit=1000`),

  bindings: () => request<Bindings>('GET', 'bindings'),
  bind: (body: { type: string; user_id?: number; group_id?: number; openid?: string; qq: number }) => request<{ ok: boolean }>('POST', 'bindings', body),
  unbind: (type: string, openid: string) => request<{ ok: boolean; number: number }>('DELETE', 'bindings', { type, openid }),
  identities: (q: string) => request<Identity[]>('GET', `identities?q=${encodeURIComponent(q)}`),
  messages: (q: MessageQuery) => {
    const params = new URLSearchParams();
    for (const [key, value] of Object.entries(q)) if (value !== undefined && value !== '' && value !== 0) params.set(key, String(value));
    return request<{ total: number; items: MessageRow[] }>('GET', `messages?${params}`);
  },
  messageFilters: () => request<MessageFilters>('GET', 'message-filters'),
  imageCache: () => request<{ files: number; bytes: number; days: number }>('GET', 'image-cache'),
  clearImageCache: () => request<{ removed: number }>('POST', 'image-cache/clear'),

  conversations: (bot = '') => request<Conversation[]>('GET', `chat/conversations?bot=${encodeURIComponent(bot)}`),
  chatMessages: (peer: string, opts: { before?: number; after?: number; limit?: number } = {}) =>
    request<MessageRow[]>('GET', `chat/messages?peer=${encodeURIComponent(peer)}&before=${opts.before ?? 0}&after=${opts.after ?? 0}&limit=${opts.limit ?? 50}`),
  chatSend: (body: { bot: string; peer: string; scene: string; text?: string; markdown?: string; image?: { name: string; base64: string }; reply_to?: number }) =>
    request<{ ok: boolean; message_id: number }>('POST', 'chat/send', body),

  replies: () => request<{ rules: ReplyRule[]; variables: ReplyVariable[] }>('GET', 'replies'),
  saveReply: (rule: Partial<ReplyRule>) => (rule.id ? request<ReplyRule>('PUT', `replies/${rule.id}`, rule) : request<ReplyRule>('POST', 'replies', rule)),
  deleteReply: (id: string) => request<{ ok: boolean }>('DELETE', `replies/${id}`),
  resetReply: (id: string) => request<ReplyRule>('POST', `replies/${id}/reset`),
  testReply: (body: { message?: string; template?: string; bot?: string; scene?: string }) =>
    request<{ matched?: boolean; rule_id?: string; rule_name?: string; format?: string; text?: string }>('POST', 'replies-test', body),

  materials: (bot: string, cursor = '') =>
    request<{ images?: Material[]; next_cursor?: string; total?: number }>('GET', `materials?bot=${encodeURIComponent(bot)}&cursor=${encodeURIComponent(cursor)}`),
  uploadMaterial: (bot: string, fileName: string, base64: string) => request<Material>('POST', 'materials', { bot, file_name: fileName, base64 }),
  deleteMaterial: (bot: string, id: string) => request<{ ok: boolean }>('DELETE', `materials/${encodeURIComponent(id)}?bot=${encodeURIComponent(bot)}`),

  sysinfo: (since: number) => request<SystemInfo & { history?: { interval: number; window: number; samples: [number, number, number][] } }>('GET', `sysinfo?since=${since}`),
};

/// 读取文件为 base64（不含 data: 前缀）。
export const fileToBase64 = (file: File) => new Promise<string>((resolve, reject) => {
  const reader = new FileReader();
  reader.onload = () => resolve(String(reader.result).split(',')[1] || '');
  reader.onerror = () => reject(reader.error);
  reader.readAsDataURL(file);
});
