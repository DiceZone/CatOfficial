// 管理面板接口。会话靠 HttpOnly Cookie，所有请求同源发出。

export class ApiError extends Error {
  constructor(public status: number, message: string) { super(message); }
}

/// features：启动参数关掉的功能（--disable-files 等）为 false，菜单里不显示。
export interface Session { authenticated: boolean; must_change_password?: boolean; passwordless?: boolean; os?: string; features?: { files: boolean; terminal: boolean; programs: boolean } }

/// 未登录（401）或必须先修改自动生成的密码（403）时回调，由外层切换到对应页面。
let onUnauthorized: ((session: Session) => void) | null = null;
export function setUnauthorizedHandler(handler: (session: Session) => void) { onUnauthorized = handler; }

/// 打开绑定页：有本机桥（应用内 WebView）时交给系统/QQ，否则新窗口。
export function openExternalUrl(url: string) {
  const bridge = (window as Window & { CatOfficial?: { openUrl?: (u: string) => void } }).CatOfficial;
  if (bridge?.openUrl) {
    bridge.openUrl(url);
    return;
  }
  window.open(url, '_blank', 'noopener,noreferrer');
}

/// 当前页面自己没法扫码时用跳转登录（手机浏览器、本机应用），桌面仍显示二维码。
export function prefersJumpLogin(): boolean {
  if (typeof window === 'undefined') return false;
  const w = window as Window & { CatOfficial?: { openUrl?: (u: string) => void } };
  if (w.CatOfficial?.openUrl) return true;
  if (/Android|iPhone|iPod|Mobile/i.test(navigator.userAgent || '')) return true;
  try {
    if (window.matchMedia('(pointer: coarse) and (max-width: 56rem)').matches) return true;
  } catch {
    /* ignore */
  }
  return false;
}

export async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
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
    if (response.status === 401 && path !== 'login') onUnauthorized?.({ authenticated: false });
    if (response.status === 403 && data && typeof data === 'object' && (data as { must_change_password?: boolean }).must_change_password)
      onUnauthorized?.({ authenticated: true, must_change_password: true });
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
  avatar?: string; member_count?: number; last: MessageRow;
}

export interface ChatMember { openid: string; number: number; name: string; card: string; role: string; avatar: string; last_seen: number }
export interface ChatPeer {
  scene: 'group' | 'private'; openid: string; number: number; name: string; avatar?: string; bound_number?: number;
  member_count?: number; memo?: string; bot_role?: string;
  members?: ChatMember[]; members_source?: 'official' | 'seen'; members_error?: string;
}
export type ChatManageAction = 'mute' | 'mute_list' | 'kick' | 'blacklist' | 'blacklist_add' | 'blacklist_del' | 'join_requests' | 'join_approve' | 'join_decline';

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
  tray?: { text?: string; start_hidden?: boolean };
  tray_supported?: boolean;
  tray_port?: number;
  new_password?: string;
}

type Saved = { ok: boolean; id?: string; restart_required?: boolean };

// 在线更新（与 Dice!Next 的 /api/system/update 相同的结构）。
export type UpdateAction = 'notify' | 'download' | 'install';
export type UpdateSource = 'auto' | 'direct' | 'mirror' | 'custom';
export interface UpdateSettings {
  autoCheck: boolean;
  intervalHours: number;
  autoAction: UpdateAction;
  scheduledInstall?: boolean;
  installTime?: string;
  source: UpdateSource;
  customMirror: string;
}
export interface UpdateStatus {
  current: { version: string; build: number; prerelease: boolean; tag: string };
  platform: { os: string; arch: string };
  latest: null | {
    tag: string; version: string; build: number; prerelease: boolean; publishedAt: string; releaseUrl: string; releaseNotes?: string;
    asset?: { name: string; size: number; sha256: string };
  };
  updateAvailable: boolean;
  phase: string;
  error: string;
  source: string;
  downloadedBytes: number;
  totalBytes: number;
  checkedAt: number;
  downloadSupported?: boolean;
  installSupported: boolean;
  scheduledInstallSupported?: boolean;
  scheduledInstallAt?: number;
  pendingTag?: string;
  timezoneMinutes?: number;
  cancelSupported?: boolean;
  canCancel?: boolean;
  selfUpdateBlockedReason?: string;
  runtime?: { container: boolean; containerType: string; containerDetection: string };
  pending: boolean;
  settings: UpdateSettings;
}

export const api = {
  session: () => request<Session>('GET', 'session'),
  changePassword: (password: string) => request<{ ok: boolean }>('POST', 'password', { password }),
  updateStatus: () => request<UpdateStatus>('GET', 'update'),
  saveUpdateSettings: (settings: UpdateSettings) => request<UpdateStatus>('PUT', 'update', settings),
  updateAction: (action: 'check' | 'download' | 'install' | 'cancel') => request<UpdateStatus>('POST', `update/${action}`),
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
  chatPeer: (bot: string, peer: string, scene: string) =>
    request<ChatPeer>('GET', `chat/peer?bot=${encodeURIComponent(bot)}&peer=${encodeURIComponent(peer)}&scene=${scene}`),
  chatManage: (body: { bot: string; peer: string; action: ChatManageAction; member?: string; seconds?: number; blacklist?: boolean; request_id?: string; reason?: string; cursor?: string }) =>
    request<{ ok: boolean; data: Record<string, unknown> }>('POST', 'chat/manage', body),
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
