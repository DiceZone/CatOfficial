import React from 'react';
import { useTranslation } from 'react-i18next';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { ChevronDown, Globe, MoreHorizontal, Pencil, Plug, PlugZap, Plus, Send, Server, Trash2 } from 'lucide-react';
import { toast } from 'sonner';
import { Card, CardContent } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Switch } from '@/components/ui/switch';
import { Badge } from '@/components/ui/badge';
import { Dialog, DialogBody, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuSeparator, DropdownMenuTrigger } from '@/components/ui/dropdown-menu';
import { PageHeader } from '@/components/page-header';
import { CardGrid, Choice, EmptyState, Field, StatusDot, Toggle, digits, errorText } from '@/components/form';
import { api, type Bot, type Connection, type ConnectionType } from '@/lib/api';
import { cn } from '@/lib/utils';
import { currentLocale } from '@/i18n';
import { formatTime } from '@/lib/utils';

const TYPES: { type: ConnectionType; icon: React.ComponentType<{ className?: string }> }[] = [
  { type: 'http_server', icon: Server },
  { type: 'ws_server', icon: Plug },
  { type: 'http_client', icon: Send },
  { type: 'ws_client', icon: Globe },
];
const iconOf = (type: ConnectionType) => TYPES.find((x) => x.type === type)?.icon ?? Plug;
const isServer = (type?: ConnectionType) => type === 'http_server' || type === 'ws_server';

function defaults(type: ConnectionType, existing: Connection[]): Partial<Connection> {
  const used = new Set(existing.map((c) => c.port));
  const port = (base: number) => { let p = base; while (used.has(p)) p++; return p; };
  const base: Partial<Connection> = { type, enabled: true, token: '', message_format: 'array', report_self: false, bots: [] };
  if (type === 'http_server') return { ...base, name: 'HTTP 服务端', host: '127.0.0.1', port: port(5700) };
  if (type === 'ws_server') return { ...base, name: 'WebSocket 服务端', host: '127.0.0.1', port: port(6700), heartbeat_interval: 30000 };
  if (type === 'http_client') return { ...base, name: 'HTTP 上报', url: 'http://127.0.0.1:8080/', timeout: 5 };
  return { ...base, name: 'WebSocket 反向连接', url: 'ws://127.0.0.1:8080/onebot/v11/ws', heartbeat_interval: 30000, reconnect_interval: 3000 };
}

function ConnectionCard({ conn, bots, onEdit, onDelete, onToggle }: {
  conn: Connection; bots: Bot[]; onEdit: () => void; onDelete: () => void; onToggle: (v: boolean) => void;
}) {
  const { t } = useTranslation();
  const locale = currentLocale();
  const Icon = iconOf(conn.type);
  const rt = conn.runtime ?? {};
  const officials = bots.filter((b) => b.type === 'official');
  const served = conn.bots && conn.bots.length ? officials.filter((b) => conn.bots!.includes(b.id)) : null;

  let tone: 'ok' | 'warn' | 'off' = 'off';
  let status = t('status.disabled');
  if (conn.enabled) {
    if (isServer(conn.type)) {
      if (rt.listening) { tone = (rt.clients ?? 0) > 0 || conn.type === 'http_server' ? 'ok' : 'warn'; status = conn.type === 'ws_server' ? t('connections.clients', { count: rt.clients ?? 0 }) : t('connections.listening'); }
      else { tone = 'warn'; status = t('status.pending'); }
    } else if (conn.type === 'ws_client') {
      const states = Object.values(rt.reverse ?? {});
      const up = states.filter((s) => s.connected).length;
      tone = states.length && up === states.length ? 'ok' : 'warn';
      status = states.length ? `${t('connections.connected')} ${up}/${states.length}` : t('status.pending');
    } else {
      const last = rt.last_post;
      tone = !last ? 'warn' : last.ok ? 'ok' : 'warn';
      status = !last ? t('connections.no_report') : last.ok ? t('connections.last_ok', { status: last.status }) : t('connections.last_failed', { error: last.error || `HTTP ${last.status}` });
    }
  }
  const address = isServer(conn.type) ? `${conn.host}:${conn.port}` : conn.url;
  const reverseErrors = conn.type === 'ws_client' ? Object.entries(rt.reverse ?? {}).filter(([, s]) => !s.connected && s.error) : [];

  return (
    <Card className="flex h-full min-w-0 flex-col transition-colors hover:border-input">
      <CardContent className="flex flex-1 flex-col gap-4 p-5">
        <div className="flex min-w-0 items-start justify-between gap-4">
          <div className={cn('flex min-w-0 flex-1 items-center gap-3', !conn.enabled && 'opacity-50')}>
            <div className="relative shrink-0">
              <div className="flex h-10 w-10 items-center justify-center rounded-full bg-primary/10"><Icon className="h-5 w-5 text-primary" /></div>
              <span className="absolute -bottom-0.5 -right-0.5"><StatusDot tone={tone} /></span>
            </div>
            <div className="min-w-0 flex-1">
              <h3 className="truncate font-semibold" title={conn.name}>{conn.name || t(`connections.type_${conn.type}`)}</h3>
              <p className={cn('truncate text-xs', tone === 'ok' ? 'text-emerald-600 dark:text-emerald-400' : 'text-muted-foreground')} title={status}>{status}</p>
            </div>
          </div>
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button variant="ghost" size="icon" className="h-8 w-8" aria-label={t('common.more')}><MoreHorizontal className="h-4 w-4" /></Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent align="end">
              <DropdownMenuItem onClick={onEdit}><Pencil className="mr-2 h-4 w-4" />{t('common.edit')}</DropdownMenuItem>
              <DropdownMenuSeparator />
              <DropdownMenuItem className="text-destructive" onClick={onDelete}><Trash2 className="mr-2 h-4 w-4" />{t('common.delete')}</DropdownMenuItem>
            </DropdownMenuContent>
          </DropdownMenu>
        </div>

        <dl className="grid min-w-0 grid-cols-[auto_minmax(0,1fr)] items-center gap-x-3 gap-y-2 text-xs">
          <dt className="text-muted-foreground">{t('connections.type')}</dt><dd className="text-right">{t(`connections.type_${conn.type}`)}</dd>
          <dt className="text-muted-foreground">{t(isServer(conn.type) ? 'connections.host' : 'connections.url')}</dt>
          <dd className="truncate text-right font-mono" title={address}>{address || '—'}</dd>
          <dt className="text-muted-foreground">{t(conn.type === 'http_client' ? 'connections.secret' : 'connections.token')}</dt>
          <dd className="text-right font-mono">{conn.token ? '••••••' : '—'}</dd>
          <dt className="text-muted-foreground">{t('connections.message_format')}</dt><dd className="text-right font-mono">{conn.message_format ?? 'array'}</dd>
          {conn.type === 'http_client' && rt.last_post && <>
            <dt className="text-muted-foreground">{t('messages.time')}</dt><dd className="text-right">{formatTime(rt.last_post.at, locale)}</dd>
          </>}
        </dl>
        {reverseErrors.length > 0 && (
          <p className="break-all rounded-md bg-muted/60 px-3 py-2 text-xs text-muted-foreground">
            {reverseErrors.map(([id, s]) => `${officials.find((b) => b.id === id)?.name ?? id}：${s.error}`).join('；')}
          </p>
        )}

        <div className="mt-auto flex items-center justify-between gap-3 border-t pt-4">
          <div className="flex min-w-0 flex-wrap gap-1.5">
            {served ? served.map((b) => <Badge key={b.id} variant="neutral">{b.name}</Badge>) : <Badge variant="info">{t('connections.bots_all')}</Badge>}
            {conn.report_self && <Badge variant="neutral">message_sent</Badge>}
          </div>
          <label className="flex shrink-0 items-center gap-2 text-sm text-muted-foreground">
            {t('common.enabled')}<Switch checked={conn.enabled} onCheckedChange={onToggle} aria-label={t('common.enabled')} />
          </label>
        </div>
      </CardContent>
    </Card>
  );
}

function ConnectionForm({ draft, onChange, bots }: { draft: Partial<Connection>; onChange: (c: Partial<Connection>) => void; bots: Bot[] }) {
  const { t } = useTranslation();
  const set = (patch: Partial<Connection>) => onChange({ ...draft, ...patch });
  const type = draft.type!;
  const officials = bots.filter((b) => b.type === 'official');
  const selected = new Set(draft.bots ?? []);
  const toggleBot = (id: string) => {
    const next = new Set(selected);
    if (next.has(id)) next.delete(id); else next.add(id);
    set({ bots: officials.map((b) => b.id).filter((x) => next.has(x)) });
  };
  return (
    <div className="space-y-4">
      <p className="rounded-md bg-muted/60 px-3 py-2 text-xs leading-relaxed text-muted-foreground">{t(`connections.hint_${type}`)}</p>
      <Field label={t('connections.name')} id="conn-name"><Input id="conn-name" value={draft.name ?? ''} onChange={(e) => set({ name: e.target.value })} /></Field>
      {isServer(type) ? (
        <div className="grid gap-4 sm:grid-cols-[1fr_8rem]">
          <Field label={t('connections.host')} id="conn-host"><Input id="conn-host" value={draft.host ?? ''} onChange={(e) => set({ host: e.target.value.trim() })} /></Field>
          <Field label={t('connections.port')} id="conn-port"><Input id="conn-port" inputMode="numeric" value={draft.port ?? ''} onChange={(e) => set({ port: digits(e.target.value) })} /></Field>
        </div>
      ) : (
        <Field label={t('connections.url')} id="conn-url"><Input id="conn-url" value={draft.url ?? ''} onChange={(e) => set({ url: e.target.value.trim() })} /></Field>
      )}
      <div className="grid gap-4 sm:grid-cols-2">
        <Field label={t(type === 'http_client' ? 'connections.secret' : 'connections.token')} id="conn-token"
          hint={t(type === 'http_client' ? 'connections.secret_hint' : 'connections.token_hint')}>
          <Input id="conn-token" value={draft.token ?? ''} onChange={(e) => set({ token: e.target.value.trim() })} />
        </Field>
        <Field label={t('connections.message_format')}>
          <Choice value={draft.message_format ?? 'array'} onChange={(v) => set({ message_format: v as Connection['message_format'] })}
            options={[{ value: 'array', label: t('connections.format_array') }, { value: 'string', label: t('connections.format_string') }]} />
        </Field>
      </div>
      <div className="grid gap-4 sm:grid-cols-2">
        {(type === 'ws_server' || type === 'ws_client') && (
          <Field label={t('connections.heartbeat')} id="conn-hb"><Input id="conn-hb" inputMode="numeric" value={draft.heartbeat_interval ?? 30000} onChange={(e) => set({ heartbeat_interval: digits(e.target.value) })} /></Field>
        )}
        {type === 'ws_client' && (
          <Field label={t('connections.reconnect')} id="conn-rc"><Input id="conn-rc" inputMode="numeric" value={draft.reconnect_interval ?? 3000} onChange={(e) => set({ reconnect_interval: digits(e.target.value) })} /></Field>
        )}
        {type === 'http_client' && (
          <Field label={t('connections.timeout')} id="conn-to"><Input id="conn-to" inputMode="numeric" value={draft.timeout ?? 5} onChange={(e) => set({ timeout: digits(e.target.value) })} /></Field>
        )}
      </div>
      <Toggle label={t('connections.report_self')} hint={t('connections.report_self_hint')} checked={!!draft.report_self} onChange={(v) => set({ report_self: v })} />
      <Field label={t('connections.bots')} hint={t('connections.bots_hint')}>
        <div className="flex flex-wrap gap-2">
          {officials.length === 0 && <span className="text-sm text-muted-foreground">—</span>}
          {officials.map((b) => (
            <button key={b.id} type="button" onClick={() => toggleBot(b.id)} aria-pressed={selected.has(b.id)}
              className={cn('rounded-full border px-3 py-1 text-sm transition-colors', selected.has(b.id) ? 'border-primary bg-primary/10 text-primary' : 'text-muted-foreground hover:bg-accent')}>
              {b.name}
            </button>
          ))}
        </div>
      </Field>
    </div>
  );
}

export function ConnectionsPage() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const conns = useQuery({ queryKey: ['connections'], queryFn: api.connections, refetchInterval: 5000 });
  const bots = useQuery({ queryKey: ['bots'], queryFn: api.bots });
  const [draft, setDraft] = React.useState<Partial<Connection> | null>(null);
  const [removing, setRemoving] = React.useState<Connection | null>(null);

  const refresh = () => { void client.invalidateQueries({ queryKey: ['connections'] }); void client.invalidateQueries({ queryKey: ['status'] }); };
  const save = useMutation({
    mutationFn: (c: Partial<Connection>) => api.saveConnection(c),
    onSuccess: () => { toast.success(t('common.saved')); setDraft(null); refresh(); },
    onError: (e) => toast.error(t('common.failed', { error: errorText(e) })),
  });
  const remove = useMutation({
    mutationFn: (id: string) => api.deleteConnection(id),
    onSuccess: () => { toast.success(t('common.deleted')); setRemoving(null); refresh(); },
    onError: (e) => toast.error(t('common.failed', { error: errorText(e) })),
  });
  const list = conns.data ?? [];
  const strip = (c: Connection): Partial<Connection> => { const { runtime: _r, ...rest } = c; return rest; };

  const addMenu = (
    <DropdownMenu>
      <DropdownMenuTrigger asChild><Button><Plus className="mr-2 h-4 w-4" />{t('connections.add')}<ChevronDown className="ml-2 h-4 w-4" /></Button></DropdownMenuTrigger>
      <DropdownMenuContent align="end">
        {TYPES.map(({ type, icon: Icon }) => (
          <DropdownMenuItem key={type} onClick={() => setDraft(defaults(type, list))}><Icon className="mr-2 h-4 w-4" />{t(`connections.type_${type}`)}</DropdownMenuItem>
        ))}
      </DropdownMenuContent>
    </DropdownMenu>
  );

  return (
    <>
      <PageHeader title={t('connections.title')} description={t('connections.description')} actions={addMenu} />
      {conns.isLoading ? null : list.length === 0
        ? <EmptyState icon={PlugZap} title={t('connections.empty')} hint={t('connections.empty_hint')} action={addMenu} />
        : <CardGrid>{list.map((c) => (
            <ConnectionCard key={c.id} conn={c} bots={bots.data ?? []} onEdit={() => setDraft(strip(c))} onDelete={() => setRemoving(c)}
              onToggle={(v) => save.mutate({ ...strip(c), enabled: v })} />
          ))}</CardGrid>}

      <Dialog open={draft !== null} onOpenChange={(v) => { if (!v) setDraft(null); }}>
        <DialogContent className="max-w-lg">
          <DialogHeader>
            <DialogTitle>{draft?.id ? t('connections.edit_title') : t('connections.add_title', { type: draft?.type ? t(`connections.type_${draft.type}`) : '' })}</DialogTitle>
          </DialogHeader>
          {draft && <DialogBody><ConnectionForm draft={draft} onChange={setDraft} bots={bots.data ?? []} /></DialogBody>}
          <DialogFooter>
            <Button variant="outline" onClick={() => setDraft(null)}>{t('common.cancel')}</Button>
            <Button disabled={save.isPending} onClick={() => draft && save.mutate(draft)}>{t('common.save')}</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <Dialog open={removing !== null} onOpenChange={(v) => { if (!v) setRemoving(null); }}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>{t('common.delete')}</DialogTitle>
            <DialogDescription>{removing && t('connections.delete_confirm', { name: removing.name })}</DialogDescription>
          </DialogHeader>
          <DialogFooter>
            <Button variant="outline" onClick={() => setRemoving(null)}>{t('common.cancel')}</Button>
            <Button variant="destructive" disabled={remove.isPending} onClick={() => removing && remove.mutate(removing.id)}>{t('common.delete')}</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  );
}
