import React from 'react';
import { useTranslation } from 'react-i18next';
import { keepPreviousData, useQuery } from '@tanstack/react-query';
import { ArrowDown, ArrowUp, ExternalLink, RefreshCw, Search, X } from 'lucide-react';
import { Card } from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Dialog, DialogContent, DialogTitle } from '@/components/ui/dialog';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import { PaginationBar } from '@/components/ui/pagination-bar';
import { PageHeader } from '@/components/page-header';
import { MarkdownView } from '@/components/markdown/markdown-view';
import { api, type MessageQuery } from '@/lib/api';
import { currentLocale } from '@/i18n';
import { formatTime } from '@/lib/utils';

const SIZES = [20, 50, 100, 200];
const SIZE_KEY = 'catofficial.messages.size';
const ALL = '__all__';

const readSize = () => {
  try { const n = Number(localStorage.getItem(SIZE_KEY)); return SIZES.includes(n) ? n : 50; } catch { return 50; }
};
/// datetime-local 的值 ↔ Unix 秒。
const toSeconds = (value: string) => (value ? Math.floor(new Date(value).getTime() / 1000) : 0);

export function MessagesPage() {
  const { t } = useTranslation();
  const locale = currentLocale();
  const [page, setPage] = React.useState(1);
  const [size, setSize] = React.useState(readSize);
  const [order, setOrder] = React.useState<'asc' | 'desc'>('desc');
  const [search, setSearch] = React.useState('');
  const [q, setQ] = React.useState('');
  const [scene, setScene] = React.useState<'' | 'private' | 'group'>('');
  const [sender, setSender] = React.useState('');
  const [peer, setPeer] = React.useState('');
  const [bot, setBot] = React.useState('');
  const [from, setFrom] = React.useState('');
  const [to, setTo] = React.useState('');
  const [preview, setPreview] = React.useState<string | null>(null);

  // 搜索框停止输入 300ms 后再查。
  React.useEffect(() => { const timer = setTimeout(() => { setQ(search.trim()); setPage(1); }, 300); return () => clearTimeout(timer); }, [search]);

  const params: MessageQuery = { page, size, order, q, scene, sender, peer, bot, from: toSeconds(from), to: toSeconds(to) };
  const query = useQuery({ queryKey: ['messages', params], queryFn: () => api.messages(params), placeholderData: keepPreviousData });
  const filters = useQuery({ queryKey: ['message-filters'], queryFn: api.messageFilters, staleTime: 30_000 });
  const rows = query.data?.items ?? [];
  const total = query.data?.total ?? 0;
  const label = (name: string | undefined, id: number | undefined) => (name ? `${name} (${id})` : String(id ?? '—'));
  const filtered = !!(q || scene || sender || peer || bot || from || to);
  const reset = () => { setSearch(''); setQ(''); setScene(''); setSender(''); setPeer(''); setBot(''); setFrom(''); setTo(''); setPage(1); };
  const change = <T,>(setter: (v: T) => void) => (v: T) => { setter(v); setPage(1); };
  const pick = (value: string) => (value === ALL ? '' : value);

  return (
    <>
      <PageHeader title={t('messages.title')} description={t('messages.description')}
        actions={<Button variant="outline" size="sm" onClick={() => { void query.refetch(); void filters.refetch(); }}><RefreshCw className="mr-2 h-4 w-4" />{t('common.refresh')}</Button>} />

      <Card className="space-y-3 p-4">
        <div className="grid gap-3 md:grid-cols-[minmax(0,2fr)_repeat(3,minmax(0,1fr))]">
          <div className="relative min-w-0">
            <Search className="pointer-events-none absolute left-3 top-1/2 h-4 w-4 -translate-y-1/2 text-muted-foreground" />
            <Input value={search} onChange={(e) => setSearch(e.target.value)} placeholder={t('messages.search')} aria-label={t('messages.search')} className="pl-9" />
          </div>
          <Select value={scene || ALL} onValueChange={(v) => change(setScene)(pick(v) as '' | 'private' | 'group')}>
            <SelectTrigger aria-label={t('messages.scene')}><SelectValue /></SelectTrigger>
            <SelectContent>
              <SelectItem value={ALL}>{t('messages.scene_all')}</SelectItem>
              <SelectItem value="private">{t('messages.private')}</SelectItem>
              <SelectItem value="group">{t('messages.group')}</SelectItem>
            </SelectContent>
          </Select>
          <Select value={peer || ALL} onValueChange={(v) => change(setPeer)(pick(v))}>
            <SelectTrigger aria-label={t('messages.conversation')}><SelectValue /></SelectTrigger>
            <SelectContent>
              <SelectItem value={ALL}>{t('messages.conversation_all')}</SelectItem>
              {(filters.data?.conversations ?? []).map((c) => (
                <SelectItem key={c.openid} value={c.openid}>{t(c.scene === 'group' ? 'messages.group' : 'messages.private')} · {label(c.name, c.number)}</SelectItem>
              ))}
            </SelectContent>
          </Select>
          <Select value={sender || ALL} onValueChange={(v) => change(setSender)(pick(v))}>
            <SelectTrigger aria-label={t('messages.sender')}><SelectValue /></SelectTrigger>
            <SelectContent>
              <SelectItem value={ALL}>{t('messages.sender_all')}</SelectItem>
              <SelectItem value="@self">{t('messages.self')}</SelectItem>
              {(filters.data?.senders ?? []).map((s) => <SelectItem key={s.openid} value={s.openid}>{label(s.name, s.number)}</SelectItem>)}
            </SelectContent>
          </Select>
        </div>
        <div className="flex flex-wrap items-end gap-3">
          <label className="grid gap-1 text-xs text-muted-foreground">{t('messages.from')}
            <Input type="datetime-local" value={from} onChange={(e) => change(setFrom)(e.target.value)} className="w-52" />
          </label>
          <label className="grid gap-1 text-xs text-muted-foreground">{t('messages.to')}
            <Input type="datetime-local" value={to} onChange={(e) => change(setTo)(e.target.value)} className="w-52" />
          </label>
          {(filters.data?.bots.length ?? 0) > 1 && (
            <label className="grid gap-1 text-xs text-muted-foreground">{t('messages.bot')}
              <Select value={bot || ALL} onValueChange={(v) => change(setBot)(pick(v))}>
                <SelectTrigger className="w-48"><SelectValue /></SelectTrigger>
                <SelectContent>
                  <SelectItem value={ALL}>{t('messages.bot_all')}</SelectItem>
                  {filters.data?.bots.map((b) => <SelectItem key={b.id} value={b.id}>{b.name}</SelectItem>)}
                </SelectContent>
              </Select>
            </label>
          )}
          {filtered && <Button variant="ghost" size="sm" onClick={reset}><X className="mr-1 h-4 w-4" />{t('messages.reset')}</Button>}
          <span className="ml-auto text-xs text-muted-foreground">{t('messages.total', { n: total })}</span>
        </div>
      </Card>

      <Card className="overflow-hidden">
        <div className="overflow-x-auto">
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead className="w-44">
                  <button type="button" onClick={() => { setOrder(order === 'desc' ? 'asc' : 'desc'); setPage(1); }}
                    className="inline-flex items-center gap-1 hover:text-foreground" aria-label={t(order === 'desc' ? 'messages.sort_asc' : 'messages.sort_desc')}>
                    {t('messages.time')}{order === 'desc' ? <ArrowDown className="h-3.5 w-3.5" /> : <ArrowUp className="h-3.5 w-3.5" />}
                  </button>
                </TableHead>
                <TableHead className="w-16">{t('messages.direction')}</TableHead>
                <TableHead className="w-20">{t('messages.scene')}</TableHead>
                <TableHead>{t('messages.conversation')}</TableHead>
                <TableHead>{t('messages.sender')}</TableHead>
                <TableHead className="min-w-[16rem]">{t('messages.content')}</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {rows.map((m) => (
                <TableRow key={m.id}>
                  <TableCell className="whitespace-nowrap font-mono text-xs text-muted-foreground">{formatTime(m.time, locale)}</TableCell>
                  <TableCell><Badge variant={m.self ? 'info' : 'neutral'}>{t(m.self ? 'messages.outgoing' : 'messages.incoming')}</Badge></TableCell>
                  <TableCell><Badge variant={m.scene === 'private' ? 'success' : 'warning'}>{t(m.scene === 'private' ? 'messages.private' : 'messages.group')}</Badge></TableCell>
                  <TableCell className="text-sm">
                    <button type="button" className="text-left hover:underline" title={t('messages.only_conversation')} onClick={() => change(setPeer)(m.peer_openid)}>{label(m.peer_name, m.peer)}</button>
                  </TableCell>
                  <TableCell className="text-sm">
                    {m.self ? (m.sender_name || t('messages.self'))
                      : <button type="button" className="text-left hover:underline" title={t('messages.only_sender')} onClick={() => m.sender_openid && change(setSender)(m.sender_openid)}>
                          {label(m.sender_card || m.sender_name, m.sender)}</button>}
                  </TableCell>
                  <TableCell className="max-w-xl text-sm" title={m.content}>
                    {m.markdown && <div className="rounded-md border bg-muted/30 p-2"><MarkdownView text={m.markdown} onImage={setPreview} /></div>}
                    {m.text && <p className="whitespace-pre-wrap break-all">{m.text}</p>}
                    {m.images.length > 0 && (
                      <div className="mt-1.5 flex flex-wrap gap-1.5">
                        {m.images.map((img) => (
                          <button key={img.url} type="button" onClick={() => setPreview(img.src)} aria-label={t('messages.preview')}
                            className="overflow-hidden rounded-md border bg-muted transition-opacity hover:opacity-80 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring">
                            <img src={img.src} alt="" loading="lazy" referrerPolicy="no-referrer" className="h-16 w-16 object-cover" />
                          </button>
                        ))}
                      </div>
                    )}
                    {!m.text && !m.markdown && m.images.length === 0 && <span className="break-all text-muted-foreground">{m.content}</span>}
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </div>
        {!query.isLoading && rows.length === 0 && <p className="p-6 text-center text-sm text-muted-foreground">{t(filtered ? 'messages.no_match' : 'messages.empty')}</p>}
        <div className="border-t px-4 pb-3">
          <PaginationBar total={total} page={page} pageSize={size} onPageChange={setPage} sizes={SIZES} disabled={query.isFetching}
            onPageSizeChange={(n) => { setSize(n); setPage(1); try { localStorage.setItem(SIZE_KEY, String(n)); } catch { /* 无痕模式 */ } }} />
        </div>
      </Card>

      <Dialog open={preview !== null} onOpenChange={(v) => { if (!v) setPreview(null); }}>
        <DialogContent className="max-w-4xl p-3">
          <DialogTitle className="sr-only">{t('messages.preview')}</DialogTitle>
          {preview && (
            <div className="space-y-2">
              <img src={preview} alt="" referrerPolicy="no-referrer" className="mx-auto max-h-[75vh] w-auto rounded-md object-contain" />
              <a href={preview} target="_blank" rel="noreferrer" className="inline-flex items-center gap-1 text-xs text-primary hover:underline">
                {t('messages.open_original')}<ExternalLink className="h-3 w-3" />
              </a>
            </div>
          )}
        </DialogContent>
      </Dialog>
    </>
  );
}
