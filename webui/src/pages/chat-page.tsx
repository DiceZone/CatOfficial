// 在线聊天：左边会话列表，右边消息流（版式照日志站的聊天视图：头像、名字、账号与时间、气泡；
// 同一个人五分钟内的连续发言合成一组；换了一天插一条日期；机器人自己的消息靠右）。
// 可以发文字、图片，或在弹窗里编辑 Markdown 发送。

import React from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { ArrowLeft, AtSign, ExternalLink, FileText, ImageOff, ImagePlus, Loader2, MessagesSquare, Quote, Search, Send, Users, X } from 'lucide-react';
import { toast } from 'sonner';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Textarea } from '@/components/ui/textarea';
import { Dialog, DialogContent, DialogTitle } from '@/components/ui/dialog';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { PageHeader } from '@/components/page-header';
import { errorText } from '@/components/form';
import { MarkdownView } from '@/components/markdown/markdown-view';
import { MarkdownEditorDialog, MarkdownInsertContext } from '@/components/markdown/markdown-editor-dialog';
import { api, botLabel, fileToBase64, type Conversation, type MessageRow } from '@/lib/api';
import { cn } from '@/lib/utils';

const GROUP_SECONDS = 5 * 60;
const ALL = '__all__';

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

/** 头像：有图用图，没有就用名字的第一个字。 */
function Avatar({ src, name, className }: { src?: string; name: string; className?: string }) {
  const [failed, setFailed] = React.useState(false);
  React.useEffect(() => setFailed(false), [src]);
  return src && !failed
    ? <img src={src} alt="" referrerPolicy="no-referrer" onError={() => setFailed(true)} className={cn('shrink-0 rounded-full bg-muted object-cover', className)} />
    : <span aria-hidden className={cn('flex shrink-0 items-center justify-center rounded-full bg-primary/15 font-semibold text-primary', className)}>{[...(name || '?')][0]}</span>;
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
      <div className={cn('group flex gap-2.5 px-4', continued ? 'pt-1' : 'pt-3', m.self && 'flex-row-reverse')}>
        {continued
          ? <span className="w-[38px] shrink-0 pt-1.5 text-center text-[11px] tabular-nums text-transparent group-hover:text-muted-foreground">{clock(m.time).slice(0, 5)}</span>
          : <Avatar src={m.avatar} name={name} className="mt-0.5 h-[38px] w-[38px] text-sm" />}
        <div className={cn('flex min-w-0 max-w-[min(42rem,85%)] flex-col', m.self && 'items-end')}>
          {!continued && (
            <div className={cn('mb-1 flex min-w-0 items-baseline gap-1.5 text-xs', m.self && 'flex-row-reverse')}>
              <span className={cn('truncate font-semibold', m.self ? 'text-primary' : 'text-foreground/80')}>{name}</span>
              {!m.self && m.sender ? <span className="font-mono text-muted-foreground">({m.sender})</span> : null}
              <span className="tabular-nums text-muted-foreground">{clock(m.time)}</span>
            </div>
          )}
          <div className="relative max-w-full">
            <div className={cn('max-w-full space-y-1.5 px-3 py-2 text-sm leading-relaxed [overflow-wrap:anywhere]',
              m.self ? 'rounded-[14px_4px_14px_14px] border border-primary/25 bg-primary/10' : 'rounded-[4px_14px_14px_14px] bg-muted',
              continued && 'rounded-[14px]')}>
              {replyPart && (
                <div className="border-l-2 border-primary/40 pl-2 text-xs text-muted-foreground">
                  {quoted ? <><span className="font-medium">{displayName(quoted)}：</span>{summary(quoted, t).slice(0, 80)}</> : t('chat.quoted_unknown')}
                </div>
              )}
              {m.parts.map((p, i) => {
                if (p.type === 'text') return p.text ? <span key={i} className="whitespace-pre-wrap">{p.text}</span> : null;
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

function ConversationList({ items, selected, onSelect, search, setSearch }: {
  items: Conversation[]; selected: string; onSelect: (c: Conversation) => void; search: string; setSearch: (v: string) => void;
}) {
  const { t } = useTranslation();
  const needle = search.trim().toLowerCase();
  const shown = needle ? items.filter((c) => `${c.name} ${c.number}`.toLowerCase().includes(needle)) : items;
  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <div className="relative px-3 pb-2">
        <Search className="pointer-events-none absolute left-6 top-1/2 h-4 w-4 -translate-y-1/2 text-muted-foreground" />
        <Input value={search} onChange={(e) => setSearch(e.target.value)} placeholder={t('chat.search')} aria-label={t('chat.search')} className="h-9 pl-9" />
      </div>
      <div className="min-h-0 flex-1 overflow-y-auto">
        {shown.length === 0 && <p className="px-4 py-8 text-center text-sm text-muted-foreground">{t(items.length ? 'chat.no_match' : 'chat.empty')}</p>}
        {shown.map((c) => {
          const name = c.name || String(c.number);
          return (
            <button key={c.peer} type="button" onClick={() => onSelect(c)} aria-current={c.peer === selected ? 'true' : undefined}
              className={cn('flex w-full items-center gap-3 px-3 py-2.5 text-left transition-colors hover:bg-accent', c.peer === selected && 'bg-accent')}>
              {c.scene === 'group' && !c.name
                ? <span aria-hidden className="flex h-10 w-10 shrink-0 items-center justify-center rounded-full bg-primary/15 text-primary"><Users className="h-5 w-5" /></span>
                : <Avatar src={c.scene === 'private' ? c.avatar : undefined} name={name} className="h-10 w-10 text-sm" />}
              <div className="min-w-0 flex-1">
                <div className="flex items-baseline justify-between gap-2">
                  <span className="truncate text-sm font-medium">{name}</span>
                  <span className="shrink-0 text-[11px] tabular-nums text-muted-foreground">{shortTime(c.time)}</span>
                </div>
                <p className="truncate text-xs text-muted-foreground">
                  <span className="mr-1">{t(c.scene === 'group' ? 'messages.group' : 'messages.private')}</span>
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

export function ChatPage() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const bots = useQuery({ queryKey: ['bots'], queryFn: api.bots });
  const officials = (bots.data ?? []).filter((b) => b.type === 'official');
  const [bot, setBot] = React.useState('');
  const [search, setSearch] = React.useState('');
  const [current, setCurrent] = React.useState<Conversation | null>(null);
  const [messages, setMessages] = React.useState<MessageRow[]>([]);
  const [olderDone, setOlderDone] = React.useState(false);
  const [loadingOlder, setLoadingOlder] = React.useState(false);
  const [text, setText] = React.useState('');
  const [quote, setQuote] = React.useState<MessageRow | null>(null);
  const [sending, setSending] = React.useState(false);
  const [mdOpen, setMdOpen] = React.useState(false);
  const [preview, setPreview] = React.useState<string | null>(null);
  const scroller = React.useRef<HTMLDivElement>(null);
  const fileInput = React.useRef<HTMLInputElement>(null);
  const stick = React.useRef(true);
  const latest = React.useRef<MessageRow[]>([]);
  latest.current = messages;

  const conversations = useQuery({ queryKey: ['conversations', bot], queryFn: () => api.conversations(bot), refetchInterval: 5000 });
  const byId = React.useMemo(() => new Map(messages.map((m) => [m.id, m])), [messages]);
  const sendBot = current?.bot || bot || officials[0]?.id || '';
  const sendBotInfo = officials.find((b) => b.id === sendBot);

  const scrollToEnd = () => requestAnimationFrame(() => { const el = scroller.current; if (el) el.scrollTop = el.scrollHeight; });

  // 打开会话：取最近 50 条。
  React.useEffect(() => {
    if (!current) return;
    let cancelled = false;
    setMessages([]); setOlderDone(false); setQuote(null); stick.current = true;
    api.chatMessages(current.peer, { limit: 50 }).then((list) => {
      if (cancelled) return;
      setMessages(list); setOlderDone(list.length < 50); scrollToEnd();
    }).catch((e) => toast.error(t('common.failed', { error: errorText(e) })));
    return () => { cancelled = true; };
  }, [current?.peer]);

  // 每 2 秒取新消息。
  React.useEffect(() => {
    if (!current) return;
    const timer = setInterval(() => {
      const list = latest.current;
      const after = list.length ? list[list.length - 1].id : 0;
      if (!after) return;
      api.chatMessages(current.peer, { after, limit: 200 }).then((fresh) => {
        if (!fresh.length) return;
        setMessages((now) => {
          const known = new Set(now.map((m) => m.id));
          return [...now, ...fresh.filter((m) => !known.has(m.id))];
        });
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
      const list = await api.chatMessages(current.peer, { before: messages[0].id, limit: 50 });
      setMessages((now) => [...list, ...now]);
      setOlderDone(list.length < 50);
      requestAnimationFrame(() => { if (el) el.scrollTop = el.scrollHeight - before; });
    } catch (e) { toast.error(t('common.failed', { error: errorText(e) })); }
    finally { setLoadingOlder(false); }
  };

  const send = async (body: { text?: string; markdown?: string; image?: { name: string; base64: string } }) => {
    if (!current || !sendBot) return false;
    setSending(true);
    try {
      await api.chatSend({ bot: sendBot, peer: current.peer, scene: current.scene, reply_to: quote?.id, ...body });
      setQuote(null); stick.current = true;
      const after = messages.length ? messages[messages.length - 1].id : 0;
      const fresh = await api.chatMessages(current.peer, after ? { after, limit: 200 } : { limit: 50 });
      setMessages((now) => { const known = new Set(now.map((m) => m.id)); return [...now, ...fresh.filter((m) => !known.has(m.id))]; });
      scrollToEnd();
      void client.invalidateQueries({ queryKey: ['conversations'] });
      return true;
    } catch (e) { toast.error(t('chat.send_failed', { error: errorText(e) })); return false; }
    finally { setSending(false); }
  };
  const sendText = async () => { if (text.trim() && await send({ text })) setText(''); };
  const sendImage = async (file?: File) => {
    if (!file) return;
    if (!file.type.startsWith('image/') || file.size > 20 * 1024 * 1024) { toast.error(t('chat.image_limit')); return; }
    await send({ image: { name: file.name, base64: await fileToBase64(file) } });
  };

  const list = conversations.data ?? [];
  const title = current ? current.name || String(current.number) : '';

  return (
    <>
      <PageHeader title={t('chat.title')} description={t('chat.description')} />
      <div className="flex h-[calc(100dvh-13rem)] min-h-[32rem] overflow-hidden rounded-lg border bg-card">
        <aside className={cn('flex w-full shrink-0 flex-col border-r pt-3 md:w-72', current && 'hidden md:flex')}>
          {officials.length > 1 && (
            <div className="px-3 pb-2">
              <Select value={bot || ALL} onValueChange={(v) => { setBot(v === ALL ? '' : v); setCurrent(null); }}>
                <SelectTrigger aria-label={t('chat.bot')} className="h-9"><SelectValue /></SelectTrigger>
                <SelectContent>
                  <SelectItem value={ALL}>{t('chat.bot_all')}</SelectItem>
                  {officials.map((b) => <SelectItem key={b.id} value={b.id}>{botLabel(b)}</SelectItem>)}
                </SelectContent>
              </Select>
            </div>
          )}
          <ConversationList items={list} selected={current?.peer ?? ''} onSelect={setCurrent} search={search} setSearch={setSearch} />
        </aside>

        <section className={cn('flex min-w-0 flex-1 flex-col', !current && 'hidden md:flex')}>
          {!current ? (
            <div className="flex flex-1 flex-col items-center justify-center gap-2 text-sm text-muted-foreground">
              <MessagesSquare className="h-8 w-8" />{t('chat.pick')}
            </div>
          ) : <>
            <header className="flex shrink-0 items-center gap-3 border-b px-4 py-2.5">
              <Button variant="ghost" size="icon" className="h-8 w-8 md:hidden" aria-label={t('common.back')} onClick={() => setCurrent(null)}><ArrowLeft className="h-4 w-4" /></Button>
              <div className="min-w-0 flex-1">
                <p className="truncate font-semibold">{title}</p>
                <p className="truncate text-xs text-muted-foreground">
                  {t(current.scene === 'group' ? 'messages.group' : 'messages.private')} · <span className="font-mono">{current.number}</span>
                  {sendBotInfo && <> · {t('chat.via', { name: botLabel(sendBotInfo) })}</>}
                </p>
              </div>
            </header>
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
            <footer className="shrink-0 border-t p-3">
              {quote && (
                <div className="mb-2 flex items-center gap-2 rounded-md bg-muted px-3 py-1.5 text-xs text-muted-foreground">
                  <Quote className="h-3.5 w-3.5 shrink-0" />
                  <span className="min-w-0 flex-1 truncate"><span className="font-medium">{displayName(quote)}：</span>{summary(quote, t)}</span>
                  <button type="button" aria-label={t('chat.cancel_quote')} onClick={() => setQuote(null)}><X className="h-3.5 w-3.5" /></button>
                </div>
              )}
              <div className="flex items-end gap-2">
                <Textarea value={text} onChange={(e) => setText(e.target.value)} rows={2} placeholder={t('chat.placeholder')} aria-label={t('chat.placeholder')}
                  className="max-h-40 min-h-[2.75rem] flex-1 resize-none" disabled={sending || !sendBot}
                  onKeyDown={(e) => { if (e.key === 'Enter' && !e.shiftKey && !e.nativeEvent.isComposing) { e.preventDefault(); void sendText(); } }} />
                <div className="flex shrink-0 gap-1">
                  <Button variant="outline" size="icon" aria-label={t('chat.send_image')} title={t('chat.send_image')} disabled={sending || !sendBot} onClick={() => fileInput.current?.click()}><ImagePlus className="h-4 w-4" /></Button>
                  <Button variant="outline" size="icon" aria-label={t('chat.send_markdown')} title={t('chat.send_markdown')} disabled={sending || !sendBot} onClick={() => setMdOpen(true)}><FileText className="h-4 w-4" /></Button>
                  <Button size="icon" aria-label={t('chat.send')} title={t('chat.send')} disabled={sending || !sendBot || !text.trim()} onClick={() => void sendText()}>
                    {sending ? <Loader2 className="h-4 w-4 animate-spin" /> : <Send className="h-4 w-4" />}
                  </Button>
                </div>
              </div>
              <p className="mt-1.5 text-[11px] text-muted-foreground">{t('chat.passive_hint')}</p>
              <input ref={fileInput} type="file" accept="image/*" className="hidden" onChange={(e) => { const f = e.target.files?.[0]; e.target.value = ''; void sendImage(f); }} />
            </footer>
          </>}
        </section>
      </div>

      <MarkdownEditorDialog open={mdOpen} onOpenChange={setMdOpen} title={t('chat.markdown_title')} description={t('chat.markdown_hint')}
        submitLabel={t('chat.send')} busy={sending}
        extra={current?.scene === 'group' ? <AtAllButton /> : undefined}
        onSubmit={async (markdown) => { if (await send({ markdown })) setMdOpen(false); }} />

      <Dialog open={preview !== null} onOpenChange={(v) => { if (!v) setPreview(null); }}>
        <DialogContent className="max-w-4xl p-3">
          <DialogTitle className="sr-only">{t('messages.preview')}</DialogTitle>
          {preview && <div className="space-y-2">
            <img src={preview} alt="" referrerPolicy="no-referrer" className="mx-auto max-h-[75vh] w-auto rounded-md object-contain" />
            <a href={preview} target="_blank" rel="noreferrer" className="inline-flex items-center gap-1 text-xs text-primary hover:underline">{t('messages.open_original')}<ExternalLink className="h-3 w-3" /></a>
          </div>}
        </DialogContent>
      </Dialog>
    </>
  );
}

/// 群聊里插入 @全体成员（官方：机器人须是群管理员，每个群每天 1 次）。
function AtAllButton() {
  const { t } = useTranslation();
  const insert = React.useContext(MarkdownInsertContext);
  return <Button type="button" variant="outline" size="sm" title={t('chat.at_all_hint')} onClick={() => insert('<qqbot-at-everyone />')}>
    <AtSign className="mr-1 h-3.5 w-3.5" />{t('chat.at_all')}
  </Button>;
}
