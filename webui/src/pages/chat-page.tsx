// 在线聊天：版式照 QQ 桌面版 ——
//   最左一栏：当前机器人的头像（点开切换机器人），下面是「全部 / 群聊 / 私聊」；
//   第二栏：会话列表；中间：消息（照日志站的聊天视图）与输入框；右边：群资料与群成员（私聊为用户资料）。
// 群与用户的管理都在右上角的「…」里，成员上点一下也能直接操作。

import React from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import {
  ArrowLeft, AtSign, Ban, Check, ExternalLink, FileText, ImageOff, ImagePlus, Link2, Loader2, MessageCircle, MessagesSquare,
  Smile, MoreHorizontal, PanelRight, Quote, RefreshCw, Search, ShieldOff, User, UserMinus, Users, VolumeX, X,
} from 'lucide-react';
import { toast } from 'sonner';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Badge } from '@/components/ui/badge';
import { Dialog, DialogBody, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import {
  DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuLabel, DropdownMenuSeparator, DropdownMenuTrigger,
} from '@/components/ui/dropdown-menu';
import { errorText } from '@/components/form';
import { useConfirm } from '@/components/confirm-dialog';
import { MarkdownView } from '@/components/markdown/markdown-view';
import { MarkdownEditorDialog, MarkdownInsertContext } from '@/components/markdown/markdown-editor-dialog';
import { api, botLabel, fileToBase64, type Bot, type ChatMember, type ChatPeer, type Conversation, type MessageRow } from '@/lib/api';
import { ChatText, Face, faceUrl, StickerPicker } from '@/components/chat-stickers';
import { saveSticker } from '@/lib/chat-stickers';
import { cn } from '@/lib/utils';

const GROUP_SECONDS = 5 * 60;
const BOT_KEY = 'catofficial.chat.bot';
type Filter = 'all' | 'group' | 'private';

const pad = (n: number) => String(n).padStart(2, '0');
const dayOf = (t: number) => { const d = new Date(t * 1000); return `${d.getFullYear()}/${pad(d.getMonth() + 1)}/${pad(d.getDate())}`; };
const clock = (t: number) => { const d = new Date(t * 1000); return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`; };
const shortTime = (t: number) => {
  const now = new Date(), d = new Date(t * 1000);
  return d.toDateString() === now.toDateString() ? `${pad(d.getHours())}:${pad(d.getMinutes())}` : `${pad(d.getMonth() + 1)}/${pad(d.getDate())}`;
};
const senderKey = (m: MessageRow) => (m.self ? `self:${m.bot}` : m.sender_openid ?? '');
const displayName = (m: MessageRow) => (m.self ? m.sender_name || m.bot_name || '' : m.sender_card || m.sender_name || String(m.sender ?? ''));
const summary = (m: MessageRow, t: (k: string) => string) =>
  m.text || (m.markdown ? m.markdown.replace(/[#>*`_~[\]()!|-]/g, '').trim() : '') || (m.images.length ? t('chat.image') : m.content);
const memberName = (m: ChatMember) => m.card || m.name || String(m.number);
const readBot = () => { try { return localStorage.getItem(BOT_KEY) || ''; } catch { return ''; } };

/** 头像：有图用图，失败或没有时用名字第一个字（群用群图标）。 */
function Avatar({ src, name, group, className }: { src?: string; name: string; group?: boolean; className?: string }) {
  const [failed, setFailed] = React.useState(false);
  React.useEffect(() => setFailed(false), [src]);
  if (src && !failed) return <img src={src} alt="" referrerPolicy="no-referrer" onError={() => setFailed(true)} className={cn('shrink-0 rounded-full bg-muted object-cover', className)} />;
  return <span aria-hidden className={cn('flex shrink-0 items-center justify-center rounded-full bg-primary/15 font-semibold text-primary', className)}>
    {group && !name ? <Users className="h-1/2 w-1/2" /> : [...(name || '?')][0]}
  </span>;
}

/** 消息里的图片；官方图片地址过期后加载失败，显示提示而不是空白。 */
function ChatImage({ src, onOpen }: { src: string; onOpen: (src: string) => void }) {
  const { t } = useTranslation();
  const [failed, setFailed] = React.useState(false);
  if (failed) return <span className="inline-flex items-center gap-1 text-xs text-muted-foreground"><ImageOff className="h-3.5 w-3.5" />{t('chat.image_expired')}</span>;
  return <button type="button" onClick={() => onOpen(src)} aria-label={t('messages.preview')} className="block overflow-hidden rounded-md">
    <img src={src} alt="" loading="lazy" referrerPolicy="no-referrer" onError={() => setFailed(true)} className="max-h-56 max-w-full object-contain" />
  </button>;
}

function MessageItem({ m, prev, onImage, onQuote, quoted }: {
  m: MessageRow; prev?: MessageRow; onImage: (src: string) => void; onQuote: (m: MessageRow) => void; quoted?: MessageRow;
}) {
  const { t } = useTranslation();
  const dateLabel = !prev || dayOf(prev.time) !== dayOf(m.time) ? dayOf(m.time) : '';
  const continued = !!prev && !dateLabel && senderKey(prev) === senderKey(m) && m.time - prev.time < GROUP_SECONDS && m.time >= prev.time;
  const name = displayName(m);
  const replyPart = m.parts.find((p) => p.type === 'reply');
  return (
    <div>
      {dateLabel && <div className="flex justify-center px-4 pb-1 pt-4"><span className="rounded-full bg-muted px-3 py-0.5 text-xs tabular-nums text-muted-foreground">{dateLabel}</span></div>}
      <div className={cn('group flex gap-2.5 px-5', continued ? 'pt-1' : 'pt-3', m.self && 'flex-row-reverse')}>
        {continued
          ? <span className="w-[38px] shrink-0 pt-1.5 text-center text-[11px] tabular-nums text-transparent group-hover:text-muted-foreground">{clock(m.time).slice(0, 5)}</span>
          : <Avatar src={m.avatar} name={name} className="mt-0.5 h-[38px] w-[38px] text-sm" />}
        <div className={cn('flex min-w-0 max-w-[min(42rem,80%)] flex-col', m.self && 'items-end')}>
          {!continued && (
            <div className={cn('mb-1 flex min-w-0 items-baseline gap-1.5 text-xs', m.self && 'flex-row-reverse')}>
              <span className="truncate text-muted-foreground">{name}</span>
              <span className="tabular-nums text-muted-foreground/70">{clock(m.time)}</span>
            </div>
          )}
          <div className="relative max-w-full">
            <div className={cn('max-w-full space-y-1.5 rounded-xl px-3 py-2 text-sm leading-relaxed [overflow-wrap:anywhere]',
              m.self ? 'bg-primary text-primary-foreground [&_a]:text-primary-foreground [&_code]:bg-black/15' : 'bg-muted')}>
              {replyPart && (
                <div className={cn('rounded-md px-2 py-1 text-xs', m.self ? 'bg-black/10' : 'bg-background/70 text-muted-foreground')}>
                  {quoted ? <><span className="font-medium">{displayName(quoted)}：</span>{summary(quoted, t).slice(0, 80)}</> : t('chat.quoted_unknown')}
                </div>
              )}
              {m.parts.map((p, i) => {
                if (p.type === 'text') return p.text ? <ChatText key={i} text={p.text} /> : null;
                if (p.type === 'markdown') return <MarkdownView key={i} text={p.text} onImage={onImage} />;
                if (p.type === 'image') return <ChatImage key={i} src={p.src} onOpen={onImage} />;
                return null;
              })}
            </div>
            <button type="button" onClick={() => onQuote(m)} aria-label={t('chat.quote')} title={t('chat.quote')}
              className={cn('absolute -bottom-2 flex h-6 w-6 items-center justify-center rounded-full border bg-card text-muted-foreground opacity-0 shadow-sm transition-opacity hover:text-foreground focus-visible:opacity-100 group-hover:opacity-100',
                m.self ? '-left-2' : '-right-2')}>
              <Quote className="h-3 w-3" />
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}

/** 头像位置的机器人切换。 */
function BotSwitcher({ bots, current, onPick }: { bots: Bot[]; current?: Bot; onPick: (id: string) => void }) {
  const { t } = useTranslation();
  const label = current ? botLabel(current) : t('chat.bot');
  return (
    <DropdownMenu>
      <DropdownMenuTrigger asChild>
        <button type="button" title={t('chat.switch_bot')} aria-label={`${t('chat.switch_bot')} · ${label}`}
          className="relative rounded-full focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring">
          <Avatar src={current?.runtime?.avatar} name={label} className="h-10 w-10 text-sm" />
          <span className={cn('absolute -bottom-0.5 -right-0.5 h-3 w-3 rounded-full border-2 border-card', current?.runtime?.online ? 'bg-emerald-500' : 'bg-muted-foreground')} />
        </button>
      </DropdownMenuTrigger>
      <DropdownMenuContent side="right" align="start" className="w-64">
        <DropdownMenuLabel>{t('chat.switch_bot')}</DropdownMenuLabel>
        {bots.map((b) => (
          <DropdownMenuItem key={b.id} onClick={() => onPick(b.id)} className="gap-2.5">
            <Avatar src={b.runtime?.avatar} name={botLabel(b)} className="h-7 w-7 text-xs" />
            <div className="min-w-0 flex-1">
              <p className="truncate text-sm">{botLabel(b)}</p>
              <p className="text-xs text-muted-foreground">{t(b.runtime?.online ? 'chat.online' : 'chat.offline')}</p>
            </div>
            {b.id === current?.id && <Check className="h-4 w-4 text-primary" />}
          </DropdownMenuItem>
        ))}
      </DropdownMenuContent>
    </DropdownMenu>
  );
}

function RailButton({ active, label, icon: Icon, onClick }: { active: boolean; label: string; icon: React.ComponentType<{ className?: string }>; onClick: () => void }) {
  return <button type="button" onClick={onClick} title={label} aria-label={label} aria-pressed={active}
    className={cn('flex h-10 w-10 items-center justify-center rounded-lg transition-colors hover:bg-accent focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring',
      active ? 'text-primary' : 'text-muted-foreground')}>
    <Icon className="h-5 w-5" />
  </button>;
}

function ConversationList({ items, selected, onSelect }: { items: Conversation[]; selected: string; onSelect: (c: Conversation) => void }) {
  const { t } = useTranslation();
  const [search, setSearch] = React.useState('');
  const needle = search.trim().toLowerCase();
  const shown = needle ? items.filter((c) => `${c.name} ${c.number}`.toLowerCase().includes(needle)) : items;
  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <div className="relative p-3">
        <Search className="pointer-events-none absolute left-6 top-1/2 h-4 w-4 -translate-y-1/2 text-muted-foreground" />
        <Input value={search} onChange={(e) => setSearch(e.target.value)} placeholder={t('chat.search')} aria-label={t('chat.search')} className="h-8 border-0 bg-muted pl-9" />
      </div>
      <div className="min-h-0 flex-1 overflow-y-auto">
        {shown.length === 0 && <p className="px-4 py-8 text-center text-sm text-muted-foreground">{t(items.length ? 'chat.no_match' : 'chat.empty')}</p>}
        {shown.map((c) => {
          const name = c.name || String(c.number);
          return (
            <button key={c.peer} type="button" onClick={() => onSelect(c)} aria-current={c.peer === selected ? 'true' : undefined}
              className={cn('flex w-full items-center gap-3 px-3 py-2.5 text-left transition-colors hover:bg-accent/60', c.peer === selected && 'bg-accent')}>
              <Avatar src={c.avatar} name={c.name} group={c.scene === 'group'} className="h-11 w-11 text-sm" />
              <div className="min-w-0 flex-1">
                <div className="flex items-baseline justify-between gap-2">
                  <span className="truncate text-sm">{name}</span>
                  <span className="shrink-0 text-[11px] tabular-nums text-muted-foreground">{shortTime(c.time)}</span>
                </div>
                <p className="mt-0.5 truncate text-xs text-muted-foreground">
                  {c.last.self ? `${t('chat.me')}：` : c.scene === 'group' ? `${displayName(c.last)}：` : ''}{summary(c.last, t)}
                </p>
              </div>
            </button>
          );
        })}
      </div>
    </div>
  );
}

type MemberAction = 'at' | 'copy' | 'mute_600' | 'mute_3600' | 'mute_86400' | 'unmute' | 'kick' | 'kick_block';

function MemberMenu({ member, onAction }: { member: ChatMember; onAction: (a: MemberAction) => void }) {
  const { t } = useTranslation();
  return (
    <DropdownMenuContent side="left" align="start" className="w-52">
      <DropdownMenuLabel className="truncate">{memberName(member)} <span className="font-mono text-xs font-normal text-muted-foreground">{member.number}</span></DropdownMenuLabel>
      <DropdownMenuItem onClick={() => onAction('at')}><AtSign className="mr-2 h-4 w-4" />{t('chat.at_member')}</DropdownMenuItem>
      <DropdownMenuItem onClick={() => onAction('copy')}><Link2 className="mr-2 h-4 w-4" />{t('chat.copy_openid')}</DropdownMenuItem>
      <DropdownMenuSeparator />
      <DropdownMenuItem onClick={() => onAction('mute_600')}><VolumeX className="mr-2 h-4 w-4" />{t('chat.mute_for', { time: t('chat.minutes', { n: 10 }) })}</DropdownMenuItem>
      <DropdownMenuItem onClick={() => onAction('mute_3600')}><VolumeX className="mr-2 h-4 w-4" />{t('chat.mute_for', { time: t('chat.hours', { n: 1 }) })}</DropdownMenuItem>
      <DropdownMenuItem onClick={() => onAction('mute_86400')}><VolumeX className="mr-2 h-4 w-4" />{t('chat.mute_for', { time: t('chat.days', { n: 1 }) })}</DropdownMenuItem>
      <DropdownMenuItem onClick={() => onAction('unmute')}><ShieldOff className="mr-2 h-4 w-4" />{t('chat.unmute')}</DropdownMenuItem>
      <DropdownMenuSeparator />
      <DropdownMenuItem className="text-destructive" onClick={() => onAction('kick')}><UserMinus className="mr-2 h-4 w-4" />{t('chat.kick')}</DropdownMenuItem>
      <DropdownMenuItem className="text-destructive" onClick={() => onAction('kick_block')}><Ban className="mr-2 h-4 w-4" />{t('chat.kick_block')}</DropdownMenuItem>
    </DropdownMenuContent>
  );
}

/** 右侧：群资料与群成员（私聊为用户资料）。 */
function SidePanel({ peer, loading, onMember, onBind }: { peer?: ChatPeer; loading: boolean; onMember: (m: ChatMember, action: MemberAction) => void; onBind: () => void }) {
  const { t } = useTranslation();
  const [search, setSearch] = React.useState('');
  const [searching, setSearching] = React.useState(false);
  if (!peer) return <aside className="w-64 shrink-0 border-l"><p className="p-4 text-sm text-muted-foreground">{loading ? t('common.loading') : ''}</p></aside>;
  const members = peer.members ?? [];
  const needle = search.trim().toLowerCase();
  const shown = needle ? members.filter((m) => `${memberName(m)} ${m.name} ${m.number}`.toLowerCase().includes(needle)) : members;
  const order = (r: string) => (r === 'owner' ? 0 : r === 'admin' ? 1 : 2);
  const bound = <button type="button" className="text-primary hover:underline" onClick={onBind}>{peer.bound_number ? <span className="font-mono">{peer.bound_number}</span> : t('chat.bind_now')}</button>;
  return (
    <aside className="flex w-64 shrink-0 flex-col border-l">
      {peer.scene === 'group' ? <>
        <section className="border-b p-4">
          <p className="mb-2 text-sm font-medium">{t('chat.group_info')}</p>
          <p className="line-clamp-4 whitespace-pre-wrap break-words text-xs text-muted-foreground">{peer.memo || t('chat.no_memo')}</p>
          <dl className="mt-3 grid grid-cols-[auto_1fr] gap-x-3 gap-y-1 text-xs">
            <dt className="text-muted-foreground">{t('chat.number')}</dt><dd className="truncate text-right font-mono">{peer.number}</dd>
            <dt className="text-muted-foreground">{t('chat.bound')}</dt><dd className="truncate text-right">{bound}</dd>
            <dt className="text-muted-foreground">{t('chat.bot_role')}</dt><dd className="text-right">{t(`chat.role_${peer.bot_role || 'unknown'}`)}</dd>
          </dl>
        </section>
        <section className="flex min-h-0 flex-1 flex-col">
          <div className="flex items-center justify-between px-4 pb-2 pt-3">
            <p className="text-sm font-medium">{t('chat.members')} {peer.member_count || members.length}</p>
            <button type="button" aria-label={t('chat.search_members')} onClick={() => setSearching((v) => !v)} className="text-muted-foreground hover:text-foreground"><Search className="h-4 w-4" /></button>
          </div>
          {searching && <div className="px-3 pb-2"><Input autoFocus value={search} onChange={(e) => setSearch(e.target.value)} placeholder={t('chat.search_members')} className="h-8" /></div>}
          {peer.members_source === 'seen' && <p className="px-4 pb-2 text-[11px] leading-snug text-muted-foreground">{t('chat.members_seen')}</p>}
          <div className="min-h-0 flex-1 overflow-y-auto pb-2">
            {[...shown].sort((a, b) => order(a.role) - order(b.role) || b.last_seen - a.last_seen).map((m) => (
              <DropdownMenu key={m.openid}>
                <DropdownMenuTrigger asChild>
                  <button type="button" className="flex w-full items-center gap-2.5 px-4 py-1.5 text-left hover:bg-accent/60">
                    <Avatar src={m.avatar} name={memberName(m)} className="h-7 w-7 text-xs" />
                    <span className="min-w-0 flex-1 truncate text-sm">{memberName(m)}</span>
                    {m.role === 'owner' && <Badge variant="warning" className="px-1.5 py-0 text-[10px]">{t('chat.role_owner')}</Badge>}
                    {m.role === 'admin' && <Badge variant="info" className="px-1.5 py-0 text-[10px]">{t('chat.role_admin')}</Badge>}
                  </button>
                </DropdownMenuTrigger>
                <MemberMenu member={m} onAction={(a) => onMember(m, a)} />
              </DropdownMenu>
            ))}
          </div>
        </section>
      </> : (
        <section className="p-4">
          <div className="flex flex-col items-center gap-2 pb-4">
            <Avatar src={peer.avatar} name={peer.name} className="h-16 w-16 text-xl" />
            <p className="max-w-full truncate font-medium">{peer.name || peer.number}</p>
          </div>
          <dl className="grid grid-cols-[auto_1fr] gap-x-3 gap-y-1.5 text-xs">
            <dt className="text-muted-foreground">{t('chat.number')}</dt><dd className="truncate text-right font-mono">{peer.number}</dd>
            <dt className="text-muted-foreground">OpenID</dt><dd className="truncate text-right font-mono" title={peer.openid}>{peer.openid}</dd>
            <dt className="text-muted-foreground">{t('chat.bound_qq')}</dt><dd className="truncate text-right">{bound}</dd>
          </dl>
        </section>
      )}
    </aside>
  );
}

type ListKind = 'join_requests' | 'mute_list' | 'blacklist';
interface Person { openid: string; number: number; name: string; card: string; avatar: string }

/** 入群申请、禁言列表、群黑名单：官方返回的列表，逐项给出操作。 */
function GroupListDialog({ kind, bot, peer, onClose }: { kind: ListKind | null; bot: string; peer: string; onClose: () => void }) {
  const { t } = useTranslation();
  const [rows, setRows] = React.useState<Record<string, unknown>[] | null>(null);
  const [error, setError] = React.useState('');
  const [busy, setBusy] = React.useState(false);
  const load = React.useCallback(async () => {
    if (!kind) return;
    setRows(null); setError('');
    try {
      const r = await api.chatManage({ bot, peer, action: kind });
      const field = kind === 'join_requests' ? 'list' : kind === 'blacklist' ? 'users' : 'members';
      const list = r.data[field];
      setRows(Array.isArray(list) ? list as Record<string, unknown>[] : []);
    } catch (e) { setError(errorText(e)); setRows([]); }
  }, [kind, bot, peer]);
  React.useEffect(() => { void load(); }, [load]);
  const act = async (body: Parameters<typeof api.chatManage>[0]) => {
    setBusy(true);
    try { await api.chatManage(body); toast.success(t('chat.done')); await load(); }
    catch (e) { toast.error(t('common.failed', { error: errorText(e) })); }
    finally { setBusy(false); }
  };
  return (
    <Dialog open={!!kind} onOpenChange={(v) => { if (!v) onClose(); }}>
      <DialogContent className="max-w-lg">
        <DialogHeader><DialogTitle>{kind ? t(`chat.list_${kind}`) : ''}</DialogTitle><DialogDescription>{t('chat.admin_required')}</DialogDescription></DialogHeader>
        <DialogBody className="space-y-2">
          {rows === null && <p className="text-sm text-muted-foreground">{t('common.loading')}</p>}
          {error && <p role="alert" className="text-sm text-destructive">{error}</p>}
          {rows && !error && rows.length === 0 && <p className="text-sm text-muted-foreground">{t('chat.list_empty')}</p>}
          {rows?.map((row, i) => {
            const person = (row._person ?? {}) as Person;
            const extra = kind === 'join_requests' ? String(row.comment ?? row.message ?? '') : kind === 'mute_list' ? String(row.mute_expire_at ?? '') : '';
            const requestId = String(row.join_request_id ?? '');
            return (
              <div key={person.openid || i} className="flex items-center gap-3 rounded-md border p-2.5">
                <Avatar src={person.avatar} name={person.card || person.name} className="h-8 w-8 text-xs" />
                <div className="min-w-0 flex-1">
                  <p className="truncate text-sm">{person.card || person.name || person.number}</p>
                  <p className="truncate text-xs text-muted-foreground"><span className="font-mono">{person.number}</span>{extra && ` · ${extra}`}</p>
                </div>
                {kind === 'join_requests' && <>
                  <Button size="sm" disabled={busy} onClick={() => void act({ bot, peer, action: 'join_approve', member: person.openid, request_id: requestId })}>{t('chat.approve')}</Button>
                  <Button size="sm" variant="outline" disabled={busy} onClick={() => void act({ bot, peer, action: 'join_decline', member: person.openid, request_id: requestId })}>{t('chat.decline')}</Button>
                </>}
                {kind === 'mute_list' && <Button size="sm" variant="outline" disabled={busy} onClick={() => void act({ bot, peer, action: 'mute', member: person.openid, seconds: 0 })}>{t('chat.unmute')}</Button>}
                {kind === 'blacklist' && <Button size="sm" variant="outline" disabled={busy} onClick={() => void act({ bot, peer, action: 'blacklist_del', member: person.openid })}>{t('chat.unblock')}</Button>}
              </div>
            );
          })}
        </DialogBody>
        <DialogFooter><Button variant="outline" onClick={onClose}>{t('common.close')}</Button></DialogFooter>
      </DialogContent>
    </Dialog>
  );
}

/** 官民绑定：把会话关联到真实 QQ 号 / 群号。 */
function BindDialog({ open, peer, onClose, onDone }: { open: boolean; peer?: ChatPeer; onClose: () => void; onDone: () => void }) {
  const { t } = useTranslation();
  const [value, setValue] = React.useState('');
  const [busy, setBusy] = React.useState(false);
  React.useEffect(() => { if (open) setValue(peer?.bound_number ? String(peer.bound_number) : ''); }, [open, peer?.bound_number]);
  if (!peer) return null;
  const type = peer.scene === 'group' ? 'group' : 'user';
  const run = async (job: () => Promise<unknown>) => {
    setBusy(true);
    try { await job(); toast.success(t('chat.done')); onDone(); onClose(); }
    catch (e) { toast.error(t('common.failed', { error: errorText(e) })); }
    finally { setBusy(false); }
  };
  return (
    <Dialog open={open} onOpenChange={(v) => { if (!v) onClose(); }}>
      <DialogContent className="max-w-md">
        <DialogHeader>
          <DialogTitle>{t(type === 'group' ? 'chat.bind_group_title' : 'chat.bind_user_title')}</DialogTitle>
          <DialogDescription>{t('chat.bind_hint')}</DialogDescription>
        </DialogHeader>
        <dl className="grid grid-cols-[auto_1fr] gap-x-3 gap-y-1 text-sm">
          <dt className="text-muted-foreground">{t('chat.number')}</dt><dd className="font-mono">{peer.number}</dd>
          <dt className="text-muted-foreground">OpenID</dt><dd className="break-all font-mono text-xs">{peer.openid}</dd>
        </dl>
        <Input inputMode="numeric" value={value} onChange={(e) => setValue(e.target.value.replace(/\D/g, ''))}
          placeholder={t(type === 'group' ? 'chat.real_group' : 'chat.real_qq')} aria-label={t(type === 'group' ? 'chat.real_group' : 'chat.real_qq')} />
        <DialogFooter>
          {peer.bound_number && <Button variant="outline" className="mr-auto text-destructive" disabled={busy} onClick={() => void run(() => api.unbind(type, peer.openid))}>{t('chat.unbind')}</Button>}
          <Button variant="outline" onClick={onClose} disabled={busy}>{t('common.cancel')}</Button>
          <Button disabled={busy || !value} onClick={() => void run(() => api.bind({ type, openid: peer.openid, qq: Number(value) }))}>{t('chat.bind')}</Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}

export function ChatPage() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const { confirm, node } = useConfirm();
  const bots = useQuery({ queryKey: ['bots'], queryFn: api.bots, refetchInterval: 15000 });
  const officials = (bots.data ?? []).filter((b) => b.type === 'official' && b.enabled);
  const [botId, setBotId] = React.useState(readBot);
  const bot = officials.find((b) => b.id === botId) ?? officials[0];
  const [filter, setFilter] = React.useState<Filter>('all');
  const [current, setCurrent] = React.useState<Conversation | null>(null);
  const [messages, setMessages] = React.useState<MessageRow[]>([]);
  const [olderDone, setOlderDone] = React.useState(false);
  const [loadingOlder, setLoadingOlder] = React.useState(false);
  const [text, setText] = React.useState('');
  const [quote, setQuote] = React.useState<MessageRow | null>(null);
  const [sending, setSending] = React.useState(false);
  const [draftFace, setDraftFace] = React.useState<string | null>(null);
  const [pickerOpen, setPickerOpen] = React.useState(false);
  const [savingSticker, setSavingSticker] = React.useState(false);
  const [mdOpen, setMdOpen] = React.useState(false);
  const [preview, setPreview] = React.useState<string | null>(null);
  const [sideOpen, setSideOpen] = React.useState(true);
  const [listKind, setListKind] = React.useState<ListKind | null>(null);
  const [bindOpen, setBindOpen] = React.useState(false);
  const scroller = React.useRef<HTMLDivElement>(null);
  const input = React.useRef<HTMLTextAreaElement>(null);
  const fileInput = React.useRef<HTMLInputElement>(null);
  const stick = React.useRef(true);
  const latest = React.useRef<MessageRow[]>([]);
  latest.current = messages;

  const pickBot = (id: string) => { setBotId(id); setCurrent(null); try { localStorage.setItem(BOT_KEY, id); } catch { /* 无痕模式 */ } };
  const conversations = useQuery({ queryKey: ['conversations', bot?.id], queryFn: () => api.conversations(bot?.id ?? ''), enabled: !!bot, refetchInterval: 5000 });
  const peerQuery = useQuery({
    queryKey: ['chat-peer', bot?.id, current?.peer], enabled: !!bot && !!current,
    queryFn: () => api.chatPeer(bot!.id, current!.peer, current!.scene), staleTime: 60_000,
  });
  const peer = peerQuery.data;
  const byId = React.useMemo(() => new Map(messages.map((m) => [m.id, m])), [messages]);
  const list = (conversations.data ?? []).filter((c) => filter === 'all' || c.scene === filter);
  const scrollToEnd = () => requestAnimationFrame(() => { const el = scroller.current; if (el) el.scrollTop = el.scrollHeight; });

  React.useEffect(() => {
    if (!current) return;
    let cancelled = false;
    setDraftFace(null); setText(''); setMessages([]); setOlderDone(false); setQuote(null); stick.current = true;
    api.chatMessages(current.peer, { limit: 50 }).then((rows) => {
      if (cancelled) return;
      setMessages(rows); setOlderDone(rows.length < 50); scrollToEnd();
    }).catch((e) => toast.error(t('common.failed', { error: errorText(e) })));
    return () => { cancelled = true; };
  }, [current?.peer]);

  React.useEffect(() => {
    if (!current) return;
    const timer = setInterval(() => {
      const rows = latest.current;
      const after = rows.length ? rows[rows.length - 1].id : 0;
      if (!after) return;
      api.chatMessages(current.peer, { after, limit: 200 }).then((fresh) => {
        if (!fresh.length) return;
        setMessages((now) => { const known = new Set(now.map((m) => m.id)); return [...now, ...fresh.filter((m) => !known.has(m.id))]; });
        if (stick.current) scrollToEnd();
      }).catch(() => {});
    }, 2000);
    return () => clearInterval(timer);
  }, [current?.peer]);

  const loadOlder = async () => {
    if (!current || !messages.length) return;
    setLoadingOlder(true);
    const el = scroller.current, before = el ? el.scrollHeight - el.scrollTop : 0;
    try {
      const rows = await api.chatMessages(current.peer, { before: messages[0].id, limit: 50 });
      setMessages((now) => [...rows, ...now]);
      setOlderDone(rows.length < 50);
      requestAnimationFrame(() => { if (el) el.scrollTop = el.scrollHeight - before; });
    } catch (e) { toast.error(t('common.failed', { error: errorText(e) })); }
    finally { setLoadingOlder(false); }
  };

  const send = async (body: { text?: string; markdown?: string; image?: { name: string; base64: string } }) => {
    if (!current || !bot) return false;
    setSending(true);
    try {
      await api.chatSend({ bot: bot.id, peer: current.peer, scene: current.scene, reply_to: quote?.id, ...body });
      setQuote(null); stick.current = true;
      const rows = latest.current;
      const after = rows.length ? rows[rows.length - 1].id : 0;
      const fresh = await api.chatMessages(current.peer, after ? { after, limit: 200 } : { limit: 50 });
      setMessages((now) => { const known = new Set(now.map((m) => m.id)); return [...now, ...fresh.filter((m) => !known.has(m.id))]; });
      scrollToEnd();
      void client.invalidateQueries({ queryKey: ['conversations'] });
      return true;
    } catch (e) { toast.error(t('chat.send_failed', { error: errorText(e) })); return false; }
    finally { setSending(false); }
  };
  const sendText = async () => {
    if (sending || (!text.trim() && draftFace === null)) return;
    if (draftFace === null) {
      if (await send({ text })) setText('');
      return;
    }
    setSending(true);
    try {
      const response = await fetch(faceUrl(draftFace));
      if (!response.ok) throw new Error(t('chat.image_expired'));
      const blob = await response.blob();
      if (blob.type !== 'image/gif') throw new Error(t('chat.image_expired'));
      const file = new File([blob], `qq-face-${draftFace}.gif`, { type: blob.type });
      if (await send({ text, image: { name: file.name, base64: await fileToBase64(file) } })) {
        setDraftFace(null);
        setText('');
      }
    } catch (e) { toast.error(t('chat.send_failed', { error: errorText(e) })); }
    finally { setSending(false); }
  };
  const sendImage = async (file?: File) => {
    if (!file) return;
    if (!file.type.startsWith('image/') || file.size > 20 * 1024 * 1024) { toast.error(t('chat.image_limit')); return; }
    try { await send({ image: { name: file.name, base64: await fileToBase64(file) } }); }
    catch (e) { toast.error(t('chat.send_failed', { error: errorText(e) })); }
  };
  const addSticker = async (source: File | string) => {
    setSavingSticker(true);
    try {
      let file: File;
      if (typeof source === 'string') {
        const response = await fetch(source, { referrerPolicy: 'no-referrer' });
        if (!response.ok) throw new Error(t('chat.image_expired'));
        const blob = await response.blob();
        const extension = blob.type.split('/')[1]?.replace(/[^a-z0-9]/gi, '') || 'png';
        file = new File([blob], `sticker.${extension}`, { type: blob.type });
      } else file = source;
      if (!file.type.startsWith('image/') || !file.size || file.size > 20 * 1024 * 1024)
        throw new Error(t('chat.image_limit'));
      await saveSticker(file);
      void client.invalidateQueries({ queryKey: ['chat-stickers'] });
      toast.success(t('chat.sticker_saved'));
    } catch (e) { toast.error(t('common.failed', { error: errorText(e) })); }
    finally { setSavingSticker(false); }
  };
  const insertText = (snippet: string) => {
    const area = input.current;
    const start = area?.selectionStart ?? text.length, end = area?.selectionEnd ?? text.length;
    setText(text.slice(0, start) + snippet + text.slice(end));
    requestAnimationFrame(() => { area?.focus(); area?.setSelectionRange(start + snippet.length, start + snippet.length); });
  };

  const manage = async (body: Omit<Parameters<typeof api.chatManage>[0], 'bot' | 'peer'>) => {
    if (!bot || !current) return;
    try { await api.chatManage({ bot: bot.id, peer: current.peer, ...body }); toast.success(t('chat.done')); void peerQuery.refetch(); }
    catch (e) { toast.error(t('common.failed', { error: errorText(e) })); }
  };
  const onMember = async (m: ChatMember, action: MemberAction) => {
    const name = memberName(m);
    if (action === 'at') { insertText(`<qqbot-at-user id="${m.openid}" /> `); return; }
    if (action === 'copy') { await navigator.clipboard.writeText(m.openid); toast.success(t('materials.copied')); return; }
    if (action === 'unmute') { await manage({ action: 'mute', member: m.openid, seconds: 0 }); return; }
    if (action.startsWith('mute_')) { await manage({ action: 'mute', member: m.openid, seconds: Number(action.slice(5)) }); return; }
    const block = action === 'kick_block';
    if (!await confirm({ title: t(block ? 'chat.kick_block' : 'chat.kick'), description: t(block ? 'chat.kick_block_confirm' : 'chat.kick_confirm', { name }), confirmLabel: t('chat.kick'), destructive: true })) return;
    await manage({ action: 'kick', member: m.openid, blacklist: block });
  };

  const group = current?.scene === 'group';
  const title = current ? (peer?.name || current.name || String(current.number)) : '';
  const count = group ? (peer?.member_count || current?.member_count || 0) : 0;

  return (
    <div className="flex h-[calc(100dvh-7.5rem)] min-h-[34rem] overflow-hidden rounded-lg border bg-card sm:h-[calc(100dvh-9rem)] xl:h-[calc(100dvh-10.5rem)]">
      {node}
      {/* 最左一栏：机器人头像与筛选 */}
      <nav aria-label={t('chat.title')} className="flex w-16 shrink-0 flex-col items-center gap-3 border-r bg-muted/30 py-4">
        {officials.length > 0 && <BotSwitcher bots={officials} current={bot} onPick={pickBot} />}
        <div className="mt-2 flex flex-col items-center gap-1">
          <RailButton active={filter === 'all'} label={t('chat.filter_all')} icon={MessageCircle} onClick={() => setFilter('all')} />
          <RailButton active={filter === 'group'} label={t('messages.group')} icon={Users} onClick={() => setFilter('group')} />
          <RailButton active={filter === 'private'} label={t('messages.private')} icon={User} onClick={() => setFilter('private')} />
        </div>
      </nav>

      {/* 会话列表 */}
      <aside className={cn('flex w-full shrink-0 flex-col border-r md:w-72', current && 'hidden md:flex')}>
        {officials.length === 0
          ? <p className="p-6 text-sm text-muted-foreground">{t('materials.no_bot_hint')}</p>
          : <ConversationList items={list} selected={current?.peer ?? ''} onSelect={setCurrent} />}
      </aside>

      {/* 消息 */}
      <section className={cn('flex min-w-0 flex-1 flex-col', !current && 'hidden md:flex')}>
        {!current ? (
          <div className="flex flex-1 flex-col items-center justify-center gap-2 text-sm text-muted-foreground">
            <MessagesSquare className="h-8 w-8" />{t('chat.pick')}
          </div>
        ) : <>
          <header className="flex h-14 shrink-0 items-center gap-2 border-b px-4">
            <Button variant="ghost" size="icon" className="h-8 w-8 md:hidden" aria-label={t('common.back')} onClick={() => setCurrent(null)}><ArrowLeft className="h-4 w-4" /></Button>
            <h2 className="min-w-0 flex-1 truncate text-base font-semibold">{title}{group && count > 0 && <span className="ml-1 font-normal">({count})</span>}</h2>
            <Button variant="ghost" size="icon" className="hidden h-8 w-8 xl:inline-flex" aria-label={t('chat.toggle_side')} title={t('chat.toggle_side')} aria-pressed={sideOpen} onClick={() => setSideOpen((v) => !v)}>
              <PanelRight className="h-4 w-4" />
            </Button>
            <DropdownMenu>
              <DropdownMenuTrigger asChild><Button variant="ghost" size="icon" className="h-8 w-8" aria-label={t('chat.manage')} title={t('chat.manage')}><MoreHorizontal className="h-4 w-4" /></Button></DropdownMenuTrigger>
              <DropdownMenuContent align="end" className="w-52">
                <DropdownMenuLabel>{t(group ? 'chat.manage_group' : 'chat.manage_user')}</DropdownMenuLabel>
                <DropdownMenuItem onClick={() => setBindOpen(true)}><Link2 className="mr-2 h-4 w-4" />{t(group ? 'chat.bind_group_title' : 'chat.bind_user_title')}</DropdownMenuItem>
                <DropdownMenuItem onClick={() => { void peerQuery.refetch(); }}><RefreshCw className="mr-2 h-4 w-4" />{t('chat.refresh_info')}</DropdownMenuItem>
                <DropdownMenuItem onClick={async () => { await navigator.clipboard.writeText(current.peer); toast.success(t('materials.copied')); }}><FileText className="mr-2 h-4 w-4" />{t('chat.copy_openid')}</DropdownMenuItem>
                {group && <>
                  <DropdownMenuSeparator />
                  <DropdownMenuItem onClick={() => setListKind('join_requests')}><Users className="mr-2 h-4 w-4" />{t('chat.list_join_requests')}</DropdownMenuItem>
                  <DropdownMenuItem onClick={() => setListKind('mute_list')}><VolumeX className="mr-2 h-4 w-4" />{t('chat.list_mute_list')}</DropdownMenuItem>
                  <DropdownMenuItem onClick={() => setListKind('blacklist')}><Ban className="mr-2 h-4 w-4" />{t('chat.list_blacklist')}</DropdownMenuItem>
                </>}
              </DropdownMenuContent>
            </DropdownMenu>
          </header>

          <div className="flex min-h-0 flex-1">
            <div className="flex min-w-0 flex-1 flex-col">
              <div ref={scroller} className="min-h-0 flex-1 overflow-y-auto pb-4"
                onScroll={(e) => { const el = e.currentTarget; stick.current = el.scrollHeight - el.scrollTop - el.clientHeight < 80; }}>
                {!olderDone && messages.length > 0 && (
                  <div className="flex justify-center pt-3">
                    <Button variant="ghost" size="sm" disabled={loadingOlder} onClick={() => void loadOlder()}>
                      {loadingOlder && <Loader2 className="mr-1 h-3.5 w-3.5 animate-spin" />}{t('chat.load_older')}
                    </Button>
                  </div>
                )}
                {messages.map((m, i) => {
                  const reply = m.parts.find((p) => p.type === 'reply');
                  return <MessageItem key={m.id} m={m} prev={messages[i - 1]} onImage={setPreview} onQuote={setQuote}
                    quoted={reply && reply.type === 'reply' ? byId.get(reply.id) : undefined} />;
                })}
              </div>

              {/* 输入区：上面一排工具，下面是输入框，发送在右下角 */}
              <footer className="flex h-44 shrink-0 flex-col border-t">
                <div className="flex items-center gap-1 px-3 pt-2">
                  <Button variant="ghost" size="icon" className="h-8 w-8" aria-label={t('chat.faces')} title={t('chat.faces')} disabled={sending || !bot} onClick={() => setPickerOpen(true)}><Smile className="h-[18px] w-[18px]" /></Button>
                  <Button variant="ghost" size="icon" className="h-8 w-8" aria-label={t('chat.send_image')} title={t('chat.send_image')} disabled={sending || !bot} onClick={() => fileInput.current?.click()}><ImagePlus className="h-[18px] w-[18px]" /></Button>
                  <Button variant="ghost" size="icon" className="h-8 w-8" aria-label={t('chat.send_markdown')} title={t('chat.send_markdown')} disabled={sending || !bot} onClick={() => setMdOpen(true)}><FileText className="h-[18px] w-[18px]" /></Button>
                  {group && <Button variant="ghost" size="icon" className="h-8 w-8" aria-label={t('chat.at_all')} title={`${t('chat.at_all')}：${t('chat.at_all_hint')}`} disabled={sending || !bot} onClick={() => insertText('<qqbot-at-everyone /> ')}><AtSign className="h-[18px] w-[18px]" /></Button>}
                  <span className="ml-auto truncate pl-3 text-[11px] text-muted-foreground" title={t('chat.passive_hint')}>{t('chat.passive_short')}</span>
                </div>
                {quote && (
                  <div className="mx-3 mt-1 flex items-center gap-2 rounded-md bg-muted px-3 py-1 text-xs text-muted-foreground">
                    <Quote className="h-3.5 w-3.5 shrink-0" />
                    <span className="min-w-0 flex-1 truncate"><span className="font-medium">{displayName(quote)}：</span>{summary(quote, t)}</span>
                    <button type="button" aria-label={t('chat.cancel_quote')} onClick={() => setQuote(null)}><X className="h-3.5 w-3.5" /></button>
                  </div>
                )}
                {draftFace !== null && <div className="mx-4 flex items-center gap-1">
                  <Face id={draftFace} />
                  <button type="button" disabled={sending} aria-label={t('common.remove')} onClick={() => setDraftFace(null)}><X className="h-4 w-4" /></button>
                </div>}
                <textarea ref={input} value={text} onChange={(e) => setText(e.target.value)} aria-label={t('chat.placeholder')} placeholder={t('chat.placeholder')}
                  disabled={sending || !bot} spellCheck={false}
                  className="min-h-0 flex-1 resize-none bg-transparent px-4 py-2 text-sm outline-none placeholder:text-muted-foreground/70"
                  onKeyDown={(e) => { if (e.key === 'Enter' && !e.shiftKey && !e.nativeEvent.isComposing) { e.preventDefault(); void sendText(); } }} />
                <div className="flex justify-end px-4 pb-3">
                  <Button size="sm" className="min-w-20" disabled={sending || !bot || (!text.trim() && draftFace === null)} onClick={() => void sendText()}>
                    {sending && <Loader2 className="mr-1.5 h-3.5 w-3.5 animate-spin" />}{t('chat.send')}
                  </Button>
                </div>
                <input ref={fileInput} type="file" accept="image/*" className="hidden" onChange={(e) => { const f = e.target.files?.[0]; e.target.value = ''; void sendImage(f); }} />
              </footer>
            </div>
            {sideOpen && <div className="hidden xl:flex"><SidePanel peer={peer} loading={peerQuery.isFetching} onMember={(m, a) => void onMember(m, a)} onBind={() => setBindOpen(true)} /></div>}
          </div>
        </>}
      </section>

      {current && <StickerPicker open={pickerOpen} onOpenChange={setPickerOpen} onFace={(id) => {
        if (current.peer.startsWith('channel:')) insertText(`<emoji:${id}>`);
        else setDraftFace(id);
      }}
        onSticker={(file) => void sendImage(file)} onUpload={(file) => void addSticker(file)} />}
      <MarkdownEditorDialog open={mdOpen} onOpenChange={setMdOpen} title={t('chat.markdown_title')} description={t('chat.markdown_hint')}
        submitLabel={t('chat.send')} busy={sending} extra={group ? <AtAllButton /> : undefined}
        onSubmit={async (markdown) => { if (await send({ markdown })) setMdOpen(false); }} />
      {bot && current && <GroupListDialog kind={listKind} bot={bot.id} peer={current.peer} onClose={() => setListKind(null)} />}
      <BindDialog open={bindOpen} peer={peer} onClose={() => setBindOpen(false)} onDone={() => { void peerQuery.refetch(); void client.invalidateQueries({ queryKey: ['conversations'] }); }} />
      <Dialog open={preview !== null} onOpenChange={(v) => { if (!v) setPreview(null); }}>
        <DialogContent className="max-w-4xl p-3">
          <DialogTitle className="sr-only">{t('messages.preview')}</DialogTitle>
          {preview && <div className="space-y-2">
            <img src={preview} alt="" referrerPolicy="no-referrer" className="mx-auto max-h-[75vh] w-auto rounded-md object-contain" />
            <Button variant="outline" size="sm" className="mr-3" disabled={savingSticker} onClick={() => void addSticker(preview)}>{t('chat.save_sticker')}</Button>
            <a href={preview} target="_blank" rel="noreferrer" className="inline-flex items-center gap-1 text-xs text-primary hover:underline">{t('messages.open_original')}<ExternalLink className="h-3 w-3" /></a>
          </div>}
        </DialogContent>
      </Dialog>
    </div>
  );
}

/// Markdown 弹窗里插入 @全体成员（官方：机器人须是群管理员，每个群每天 1 次）。
function AtAllButton() {
  const { t } = useTranslation();
  const insert = React.useContext(MarkdownInsertContext);
  return <Button type="button" variant="outline" size="sm" title={t('chat.at_all_hint')} onClick={() => insert('<qqbot-at-everyone />')}>
    <AtSign className="mr-1 h-3.5 w-3.5" />{t('chat.at_all')}
  </Button>;
}
