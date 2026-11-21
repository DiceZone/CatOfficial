import React from 'react';
import { useTranslation } from 'react-i18next';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { Bot as BotIcon, ChevronDown, MoreHorizontal, Pencil, Plus, QrCode, RefreshCw, Trash2, Users } from 'lucide-react';
import { QRCodeSVG } from 'qrcode.react';
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
import { api, botLabel, type Bot } from '@/lib/api';
import { cn } from '@/lib/utils';

type Tone = 'ok' | 'warn' | 'off';

function botState(bot: Bot): { tone: Tone; key: string; detail?: string } {
  if (!bot.enabled) return { tone: 'off', key: 'status.disabled' };
  const rt = bot.runtime;
  if (!rt || !rt.running) return { tone: 'warn', key: 'status.pending' };
  if (rt.online) return { tone: 'ok', key: 'status.online' };
  return { tone: 'warn', key: rt.error ? 'status.offline' : 'status.connecting', detail: rt.error };
}

const reverseUrl = (b: Partial<Bot>) => `ws://${!b.host || b.host === '0.0.0.0' ? window.location.hostname : b.host}:${b.port ?? ''}/`;

function BotCard({ bot, onEdit, onDelete, onToggle, onQr }: {
  bot: Bot; onEdit: () => void; onDelete: () => void; onToggle: (v: boolean) => void; onQr: () => void;
}) {
  const { t } = useTranslation();
  const state = botState(bot);
  const rt = bot.runtime;
  const official = bot.type === 'official';
  const label = botLabel(bot);
  return (
    <Card className="flex h-full min-w-0 flex-col transition-colors hover:border-input">
      <CardContent className="flex flex-1 flex-col gap-4 p-5">
        <div className="flex min-w-0 items-start justify-between gap-4">
          <div className={cn('flex min-w-0 flex-1 items-center gap-3', !bot.enabled && 'opacity-50')}>
            <div className="relative shrink-0">
              {official && rt?.avatar ? <img src={rt.avatar} alt="" className="h-10 w-10 rounded-full object-cover" />
                : <div className="flex h-10 w-10 items-center justify-center rounded-full bg-primary/10">
                    {official ? <BotIcon className="h-5 w-5 text-primary" /> : <Users className="h-5 w-5 text-primary" />}
                  </div>}
              <span className="absolute -bottom-0.5 -right-0.5"><StatusDot tone={state.tone} /></span>
            </div>
            <div className="min-w-0 flex-1">
              <h3 className="truncate font-semibold" title={label}>{label}</h3>
              <p className={cn('text-xs', state.tone === 'ok' ? 'text-emerald-600 dark:text-emerald-400' : 'text-muted-foreground')}>{t(state.key)}</p>
            </div>
          </div>
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button variant="ghost" size="icon" className="h-8 w-8" aria-label={t('common.more')}><MoreHorizontal className="h-4 w-4" /></Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent align="end">
              <DropdownMenuItem onClick={onEdit}><Pencil className="mr-2 h-4 w-4" />{t('common.edit')}</DropdownMenuItem>
              {official && <DropdownMenuItem onClick={onQr}><QrCode className="mr-2 h-4 w-4" />{t('bots.rescan')}</DropdownMenuItem>}
              <DropdownMenuSeparator />
              <DropdownMenuItem className="text-destructive" onClick={onDelete}><Trash2 className="mr-2 h-4 w-4" />{t('common.delete')}</DropdownMenuItem>
            </DropdownMenuContent>
          </DropdownMenu>
        </div>

        <dl className="grid min-w-0 grid-cols-[auto_minmax(0,1fr)] items-center gap-x-3 gap-y-2 text-xs">
          {official ? <>
            <dt className="text-muted-foreground">{t('bots.app_id_label')}</dt><dd className="truncate text-right font-mono">{bot.app_id || '—'}</dd>
            <dt className="text-muted-foreground">{t('bots.mode')}</dt><dd className="text-right">{t(bot.mode === 'webhook' ? 'bots.mode_webhook' : 'bots.mode_websocket')}</dd>
          </> : <>
            <dt className="text-muted-foreground">{t('bots.connection')}</dt><dd className="text-right">{t(bot.connection === 'forward' ? 'bots.conn_forward' : 'bots.conn_reverse')}</dd>
            <dt className="text-muted-foreground">{t('bots.address')}</dt>
            <dd className="truncate text-right font-mono" title={bot.connection === 'forward' ? bot.url : reverseUrl(bot)}>{bot.connection === 'forward' ? bot.url : reverseUrl(bot)}</dd>
          </>}
          <dt className="text-muted-foreground">{t('bots.self_id')}</dt><dd className="truncate text-right font-mono">{rt?.self_id || '—'}</dd>
          {official && rt?.running && <>
            <dt className="text-muted-foreground">{t('bots.received')} / {t('bots.sent')}</dt><dd className="text-right font-mono">{rt.received ?? 0} / {rt.sent ?? 0}</dd>
          </>}
        </dl>
        {state.detail && <p className="break-all rounded-md bg-muted/60 px-3 py-2 text-xs text-muted-foreground">{state.detail}</p>}

        <div className="mt-auto flex items-center justify-between gap-3 border-t pt-4">
          <Badge variant={official ? 'info' : 'neutral'}>{t(official ? 'bots.type_official' : 'bots.type_onebot')}</Badge>
          <label className="flex items-center gap-2 text-sm text-muted-foreground">
            {t('common.enabled')}<Switch checked={bot.enabled} onCheckedChange={onToggle} aria-label={t('common.enabled')} />
          </label>
        </div>
      </CardContent>
    </Card>
  );
}

/// 官方机器人的接收方式与开关（扫码与手动输入共用）。
function OfficialOptions({ draft, set }: { draft: Partial<Bot>; set: (p: Partial<Bot>) => void }) {
  const { t } = useTranslation();
  return (
    <div className="space-y-4">
      <Field label={t('bots.mode')} hint={draft.mode === 'webhook' ? t('bots.webhook_hint') : undefined}>
        <Choice value={draft.mode === 'webhook' ? 'webhook' : 'websocket'} onChange={(v) => set({ mode: v as Bot['mode'] })}
          options={[{ value: 'websocket', label: t('bots.mode_websocket') }, { value: 'webhook', label: t('bots.mode_webhook') }]} />
      </Field>
      <Toggle label={t('bots.subscribe')} hint={t('bots.subscribe_hint')} checked={!!draft.subscribe_group_member_events}
        onChange={(v) => set({ subscribe_group_member_events: v })} />
      <Toggle label={t('bots.auto_ack')} hint={t('bots.auto_ack_hint')} checked={draft.auto_ack_interactions !== false}
        onChange={(v) => set({ auto_ack_interactions: v })} />
    </div>
  );
}

function OfficialKeyForm({ draft, set }: { draft: Partial<Bot>; set: (p: Partial<Bot>) => void }) {
  const { t } = useTranslation();
  return (
    <div className="space-y-4">
      <Field label={t('bots.name')} id="bot-name"><Input id="bot-name" placeholder={t('bots.name_placeholder')} value={draft.name ?? ''} onChange={(e) => set({ name: e.target.value })} /></Field>
      <div className="grid gap-4 sm:grid-cols-2">
        <Field label={t('bots.app_id')} id="bot-appid"><Input id="bot-appid" value={draft.app_id ?? ''} onChange={(e) => set({ app_id: e.target.value.trim() })} /></Field>
        <Field label={t('bots.app_secret')} hint={draft.app_secret_set ? t('bots.app_secret_keep') : undefined} id="bot-secret">
          <Input id="bot-secret" type="password" autoComplete="new-password" value={draft.app_secret ?? ''} onChange={(e) => set({ app_secret: e.target.value.trim() })} />
        </Field>
      </div>
      <OfficialOptions draft={draft} set={set} />
    </div>
  );
}

function OneBotForm({ draft, set }: { draft: Partial<Bot>; set: (p: Partial<Bot>) => void }) {
  const { t } = useTranslation();
  const forward = draft.connection === 'forward';
  return (
    <div className="space-y-4">
      <p className="rounded-md bg-muted/60 px-3 py-2 text-xs leading-relaxed text-muted-foreground">{t('bots.onebot_hint')}</p>
      <Field label={t('bots.name')} id="bot-name"><Input id="bot-name" placeholder={t('bots.name_placeholder')} value={draft.name ?? ''} onChange={(e) => set({ name: e.target.value })} /></Field>
      <Field label={t('bots.connection')} hint={t(forward ? 'bots.forward_hint' : 'bots.reverse_hint')}>
        <Choice value={forward ? 'forward' : 'reverse'} onChange={(v) => set({ connection: v as Bot['connection'] })}
          options={[{ value: 'reverse', label: t('bots.conn_reverse') }, { value: 'forward', label: t('bots.conn_forward') }]} />
      </Field>
      {forward ? (
        <Field label={t('bots.url')} hint={t('bots.url_hint')} id="bot-url"><Input id="bot-url" value={draft.url ?? ''} onChange={(e) => set({ url: e.target.value.trim() })} /></Field>
      ) : <>
        <div className="grid gap-4 sm:grid-cols-[1fr_8rem]">
          <Field label={t('bots.listen_host')} id="bot-host"><Input id="bot-host" value={draft.host ?? '127.0.0.1'} onChange={(e) => set({ host: e.target.value.trim() })} /></Field>
          <Field label={t('bots.listen_port')} id="bot-port"><Input id="bot-port" inputMode="numeric" value={draft.port ?? ''} onChange={(e) => set({ port: digits(e.target.value) })} /></Field>
        </div>
        <Field label={t('bots.reverse_url')}><Input readOnly className="font-mono" value={reverseUrl(draft)} onFocus={(e) => e.target.select()} /></Field>
      </>}
      <Field label={t('bots.token')} hint={draft.token_set ? t('bots.token_keep') : undefined} id="bot-token">
        <Input id="bot-token" type="password" autoComplete="new-password" value={draft.token ?? ''} onChange={(e) => set({ token: e.target.value.trim() })} />
      </Field>
    </div>
  );
}

/// 扫码面板：扫码完成后服务端直接保存凭据，浏览器拿不到 AppSecret。
function QrPanel({ options, botId, onDone }: { options: Partial<Bot>; botId?: string; onDone: () => void }) {
  const { t } = useTranslation();
  const [session, setSession] = React.useState<{ id: string; url: string } | null>(null);
  const [state, setState] = React.useState<'loading' | 'waiting' | 'expired' | 'error'>('loading');
  const [error, setError] = React.useState('');
  const optionsRef = React.useRef(options);
  optionsRef.current = options;

  const start = React.useCallback(async () => {
    setState('loading'); setSession(null);
    try {
      const r = await api.qrStart();
      setSession({ id: r.session_id, url: r.url });
      setState('waiting');
    } catch (e) { setError(errorText(e)); setState('error'); }
  }, []);
  React.useEffect(() => { void start(); }, [start]);
  React.useEffect(() => {
    if (!session || state !== 'waiting') return;
    let cancelled = false;
    const timer = window.setInterval(async () => {
      try {
        const o = optionsRef.current;
        const r = await api.qrPoll(session.id, { name: o.name, mode: o.mode, subscribe_group_member_events: o.subscribe_group_member_events,
          auto_ack_interactions: o.auto_ack_interactions, enabled: o.enabled ?? true }, botId);
        if (cancelled) return;
        if (r.status === 'expired') setState('expired');
        if (r.status === 'completed') { toast.success(t('bots.qr_done')); onDone(); }
      } catch (e) { if (!cancelled) { setError(errorText(e)); setState('error'); } }
    }, 2000);
    return () => { cancelled = true; window.clearInterval(timer); };
  }, [session, state, botId, onDone, t]);

  return (
    <div className="flex flex-col items-center gap-3">
      <div className="flex h-56 w-56 items-center justify-center rounded-lg border bg-white p-3">
        {session && state !== 'loading' ? <QRCodeSVG value={session.url} size={200} className={cn(state !== 'waiting' && 'opacity-20')} />
          : <RefreshCw className="h-6 w-6 animate-spin text-muted-foreground" />}
      </div>
      <p className="text-center text-sm text-muted-foreground">
        {state === 'waiting' && t('bots.qr_hint')}
        {state === 'expired' && t('bots.qr_expired')}
        {state === 'error' && t('common.failed', { error })}
      </p>
      {(state === 'expired' || state === 'error') && <Button size="sm" variant="outline" onClick={start}><RefreshCw className="mr-2 h-4 w-4" />{t('bots.qr_refresh')}</Button>}
    </div>
  );
}

type Adding = { kind: 'official'; step: 'qr' | 'manual'; draft: Partial<Bot> } | { kind: 'onebot'; draft: Partial<Bot> };

export function BotsPage() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const bots = useQuery({ queryKey: ['bots'], queryFn: api.bots, refetchInterval: 5000 });
  const [adding, setAdding] = React.useState<Adding | null>(null);
  const [editing, setEditing] = React.useState<Partial<Bot> | null>(null);
  const [rescan, setRescan] = React.useState<Partial<Bot> | null>(null);
  const [removing, setRemoving] = React.useState<Bot | null>(null);

  const refresh = React.useCallback(() => { void client.invalidateQueries({ queryKey: ['bots'] }); void client.invalidateQueries({ queryKey: ['status'] }); }, [client]);
  const save = useMutation({
    mutationFn: (b: Partial<Bot>) => api.saveBot(b),
    onSuccess: () => { toast.success(t('common.saved')); setEditing(null); setAdding(null); refresh(); },
    onError: (e) => toast.error(t('common.failed', { error: errorText(e) })),
  });
  const remove = useMutation({
    mutationFn: (id: string) => api.deleteBot(id),
    onSuccess: () => { toast.success(t('common.deleted')); setRemoving(null); refresh(); },
    onError: (e) => toast.error(t('common.failed', { error: errorText(e) })),
  });

  const list = bots.data ?? [];
  const nextPort = () => { const used = new Set(list.map((b) => b.port)); let p = 6800; while (used.has(p)) p++; return p; };
  const editable = (b: Bot): Partial<Bot> => { const { runtime: _r, ...rest } = b; return { ...rest, app_secret: '', token: '' }; };
  const qrDone = React.useCallback(() => { setAdding(null); setRescan(null); refresh(); }, [refresh]);

  const addMenu = (
    <DropdownMenu>
      <DropdownMenuTrigger asChild><Button><Plus className="mr-2 h-4 w-4" />{t('bots.add')}<ChevronDown className="ml-2 h-4 w-4" /></Button></DropdownMenuTrigger>
      <DropdownMenuContent align="end">
        <DropdownMenuItem onClick={() => setAdding({ kind: 'official', step: 'qr', draft: { type: 'official', enabled: true, name: '', mode: 'websocket', auto_ack_interactions: true, subscribe_group_member_events: false } })}>
          <BotIcon className="mr-2 h-4 w-4" />{t('bots.add_official')}
        </DropdownMenuItem>
        <DropdownMenuItem onClick={() => setAdding({ kind: 'onebot', draft: { type: 'onebot', enabled: true, name: '', connection: 'reverse', host: '127.0.0.1', port: nextPort(), url: 'ws://127.0.0.1:3001', token: '' } })}>
          <Users className="mr-2 h-4 w-4" />{t('bots.add_onebot')}
        </DropdownMenuItem>
      </DropdownMenuContent>
    </DropdownMenu>
  );

  const setAddDraft = (p: Partial<Bot>) => setAdding((a) => (a ? { ...a, draft: { ...a.draft, ...p } } as Adding : a));
  const setEditDraft = (p: Partial<Bot>) => setEditing((d) => (d ? { ...d, ...p } : d));

  return (
    <>
      <PageHeader title={t('bots.title')} description={t('bots.description')} actions={addMenu} />
      {bots.isLoading ? null : list.length === 0
        ? <EmptyState icon={BotIcon} title={t('bots.empty')} hint={t('bots.empty_hint')} action={addMenu} />
        : <CardGrid>{list.map((b) => (
            <BotCard key={b.id} bot={b} onEdit={() => setEditing(editable(b))} onDelete={() => setRemoving(b)}
              onToggle={(v) => save.mutate({ ...editable(b), enabled: v })} onQr={() => setRescan(editable(b))} />
          ))}</CardGrid>}

      {/* 添加：官方机器人默认扫码，OneBot 机器人选连接方式 */}
      <Dialog open={adding !== null} onOpenChange={(v) => { if (!v) setAdding(null); }}>
        <DialogContent className="max-w-lg">
          <DialogHeader><DialogTitle>{t('bots.add_title')} · {t(adding?.kind === 'onebot' ? 'bots.type_onebot' : 'bots.type_official')}</DialogTitle></DialogHeader>
          {adding?.kind === 'official' && adding.step === 'qr' && (
            <DialogBody className="space-y-5">
              <QrPanel options={adding.draft} onDone={qrDone} />
              <p className="text-center"><button type="button" className="text-xs text-primary hover:underline"
                onClick={() => setAdding({ ...adding, step: 'manual' })}>{t('bots.manual_login')}</button></p>
              <div className="border-t pt-5"><OfficialOptions draft={adding.draft} set={setAddDraft} /></div>
            </DialogBody>
          )}
          {adding?.kind === 'official' && adding.step === 'manual' && (
            <DialogBody className="space-y-4">
              <OfficialKeyForm draft={adding.draft} set={setAddDraft} />
              <p><button type="button" className="text-xs text-primary hover:underline" onClick={() => setAdding({ ...adding, step: 'qr' })}>{t('bots.back_to_qr')}</button></p>
            </DialogBody>
          )}
          {adding?.kind === 'onebot' && <DialogBody><OneBotForm draft={adding.draft} set={setAddDraft} /></DialogBody>}
          {adding && !(adding.kind === 'official' && adding.step === 'qr') && (
            <DialogFooter>
              <Button variant="outline" onClick={() => setAdding(null)}>{t('common.cancel')}</Button>
              <Button disabled={save.isPending} onClick={() => save.mutate(adding.draft)}>{t('common.save')}</Button>
            </DialogFooter>
          )}
        </DialogContent>
      </Dialog>

      {/* 编辑：不再出现扫码，要换凭据用卡片菜单里的「重新扫码登录」 */}
      <Dialog open={editing !== null} onOpenChange={(v) => { if (!v) setEditing(null); }}>
        <DialogContent className="max-w-lg">
          <DialogHeader><DialogTitle>{t('bots.edit_title')} · {t(editing?.type === 'onebot' ? 'bots.type_onebot' : 'bots.type_official')}</DialogTitle></DialogHeader>
          {editing && <DialogBody>{editing.type === 'onebot' ? <OneBotForm draft={editing} set={setEditDraft} /> : <OfficialKeyForm draft={editing} set={setEditDraft} />}</DialogBody>}
          <DialogFooter>
            <Button variant="outline" onClick={() => setEditing(null)}>{t('common.cancel')}</Button>
            <Button disabled={save.isPending} onClick={() => editing && save.mutate(editing)}>{t('common.save')}</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <Dialog open={rescan !== null} onOpenChange={(v) => { if (!v) setRescan(null); }}>
        <DialogContent className="max-w-sm">
          <DialogHeader><DialogTitle>{t('bots.qr_title')}</DialogTitle><DialogDescription>{rescan && botLabel(rescan as Bot)}</DialogDescription></DialogHeader>
          {rescan && <DialogBody><QrPanel options={rescan} botId={rescan.id} onDone={qrDone} /></DialogBody>}
        </DialogContent>
      </Dialog>

      <Dialog open={removing !== null} onOpenChange={(v) => { if (!v) setRemoving(null); }}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>{t('common.delete')}</DialogTitle>
            <DialogDescription>{removing && t('bots.delete_confirm', { name: botLabel(removing) })}</DialogDescription>
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
