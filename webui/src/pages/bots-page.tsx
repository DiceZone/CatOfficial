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
import { CardGrid, Choice, EmptyState, Field, StatusDot, Toggle, errorText } from '@/components/form';
import { api, type Bot } from '@/lib/api';
import { cn } from '@/lib/utils';

type Tone = 'ok' | 'warn' | 'off';

function botState(bot: Bot): { tone: Tone; key: string; detail?: string } {
  if (!bot.enabled) return { tone: 'off', key: 'status.disabled' };
  const rt = bot.runtime;
  if (!rt || !rt.running) return { tone: 'warn', key: 'status.pending' };
  if (rt.online) return { tone: 'ok', key: 'status.online' };
  return { tone: 'warn', key: rt.error ? 'status.offline' : 'status.connecting', detail: rt.error };
}

function BotCard({ bot, onEdit, onDelete, onToggle, onQr }: {
  bot: Bot; onEdit: () => void; onDelete: () => void; onToggle: (v: boolean) => void; onQr: () => void;
}) {
  const { t } = useTranslation();
  const state = botState(bot);
  const rt = bot.runtime;
  const official = bot.type === 'official';
  const displayName = rt?.nickname || bot.name;
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
              <h3 className="truncate font-semibold" title={displayName}>{displayName}</h3>
              <p className={cn('text-xs', state.tone === 'ok' ? 'text-emerald-600 dark:text-emerald-400' : 'text-muted-foreground')}>{t(state.key)}</p>
              {displayName !== bot.name && <p className="truncate text-xs text-muted-foreground">{bot.name}</p>}
            </div>
          </div>
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button variant="ghost" size="icon" className="h-8 w-8" aria-label={t('common.more')}><MoreHorizontal className="h-4 w-4" /></Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent align="end">
              <DropdownMenuItem onClick={onEdit}><Pencil className="mr-2 h-4 w-4" />{t('common.edit')}</DropdownMenuItem>
              {official && <DropdownMenuItem onClick={onQr}><QrCode className="mr-2 h-4 w-4" />{t('bots.qr_login')}</DropdownMenuItem>}
              <DropdownMenuSeparator />
              <DropdownMenuItem className="text-destructive" onClick={onDelete}><Trash2 className="mr-2 h-4 w-4" />{t('common.delete')}</DropdownMenuItem>
            </DropdownMenuContent>
          </DropdownMenu>
        </div>

        <dl className="grid min-w-0 grid-cols-[auto_minmax(0,1fr)] items-center gap-x-3 gap-y-2 text-xs">
          <dt className="text-muted-foreground">{t('connections.type')}</dt>
          <dd className="text-right">{t(official ? 'bots.type_official' : 'bots.type_onebot')}</dd>
          {official ? <>
            <dt className="text-muted-foreground">{t('bots.app_id_label')}</dt><dd className="truncate text-right font-mono">{bot.app_id || '—'}</dd>
            <dt className="text-muted-foreground">{t('bots.mode')}</dt><dd className="text-right">{t(bot.mode === 'webhook' ? 'bots.mode_webhook' : 'bots.mode_gateway')}</dd>
          </> : <>
            <dt className="text-muted-foreground">{t('bots.address')}</dt><dd className="truncate text-right font-mono" title={bot.url}>{bot.url || '—'}</dd>
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

function BotForm({ draft, onChange, onQr }: { draft: Partial<Bot>; onChange: (b: Partial<Bot>) => void; onQr: () => void }) {
  const { t } = useTranslation();
  const set = (patch: Partial<Bot>) => onChange({ ...draft, ...patch });
  if (draft.type === 'onebot') return (
    <div className="space-y-4">
      <p className="rounded-md bg-muted/60 px-3 py-2 text-xs leading-relaxed text-muted-foreground">{t('bots.onebot_hint')}</p>
      <Field label={t('bots.name')} id="bot-name"><Input id="bot-name" placeholder={t('bots.name_placeholder')} value={draft.name ?? ''} onChange={(e) => set({ name: e.target.value })} /></Field>
      <Field label={t('bots.url')} hint={t('bots.url_hint')} id="bot-url"><Input id="bot-url" value={draft.url ?? ''} onChange={(e) => set({ url: e.target.value.trim() })} /></Field>
      <Field label={t('bots.token')} hint={draft.token_set ? t('bots.token_keep') : undefined} id="bot-token">
        <Input id="bot-token" type="password" autoComplete="new-password" value={draft.token ?? ''} onChange={(e) => set({ token: e.target.value.trim() })} />
      </Field>
    </div>
  );
  return (
    <div className="space-y-4">
      <Button type="button" variant="outline" className="w-full" onClick={onQr}><QrCode className="mr-2 h-4 w-4" />{t('bots.qr_login')}</Button>
      <Field label={t('bots.name')} id="bot-name"><Input id="bot-name" placeholder={t('bots.name_placeholder')} value={draft.name ?? ''} onChange={(e) => set({ name: e.target.value })} /></Field>
      <div className="grid gap-4 sm:grid-cols-2">
        <Field label={t('bots.app_id')} id="bot-appid"><Input id="bot-appid" value={draft.app_id ?? ''} onChange={(e) => set({ app_id: e.target.value.trim() })} /></Field>
        <Field label={t('bots.app_secret')} hint={draft.app_secret_set ? t('bots.app_secret_keep') : undefined} id="bot-secret">
          <Input id="bot-secret" type="password" autoComplete="new-password" value={draft.app_secret ?? ''} onChange={(e) => set({ app_secret: e.target.value.trim() })} />
        </Field>
      </div>
      <Field label={t('bots.mode')} hint={draft.mode === 'webhook' ? t('bots.webhook_hint') : undefined}>
        <Choice value={draft.mode ?? 'gateway'} onChange={(v) => set({ mode: v as Bot['mode'] })}
          options={[{ value: 'gateway', label: t('bots.mode_gateway') }, { value: 'webhook', label: t('bots.mode_webhook') }]} />
      </Field>
      <Toggle label={t('bots.subscribe')} hint={t('bots.subscribe_hint')} checked={!!draft.subscribe_group_member_events}
        onChange={(v) => set({ subscribe_group_member_events: v })} />
      <Toggle label={t('bots.auto_ack')} hint={t('bots.auto_ack_hint')} checked={draft.auto_ack_interactions !== false}
        onChange={(v) => set({ auto_ack_interactions: v })} />
    </div>
  );
}

/// 扫码登录：扫码完成后服务端直接保存凭据，浏览器拿不到 AppSecret。
function QrDialog({ open, bot, onClose }: { open: boolean; bot: Partial<Bot> | null; onClose: (saved: boolean) => void }) {
  const { t } = useTranslation();
  const [session, setSession] = React.useState<{ id: string; url: string } | null>(null);
  const [state, setState] = React.useState<'loading' | 'waiting' | 'expired' | 'error'>('loading');
  const [error, setError] = React.useState('');

  const start = React.useCallback(async () => {
    setState('loading'); setSession(null);
    try {
      const r = await api.qrStart();
      setSession({ id: r.session_id, url: r.url });
      setState('waiting');
    } catch (e) { setError(errorText(e)); setState('error'); }
  }, []);

  React.useEffect(() => { if (open) void start(); }, [open, start]);
  React.useEffect(() => {
    if (!open || !session || state !== 'waiting') return;
    let cancelled = false;
    const timer = window.setInterval(async () => {
      try {
        const r = await api.qrPoll(session.id, { name: bot?.name, mode: bot?.mode, subscribe_group_member_events: bot?.subscribe_group_member_events,
          auto_ack_interactions: bot?.auto_ack_interactions, enabled: bot?.enabled ?? true }, bot?.id);
        if (cancelled) return;
        if (r.status === 'expired') setState('expired');
        if (r.status === 'completed') { toast.success(t('bots.qr_done')); onClose(true); }
      } catch (e) { if (!cancelled) { setError(errorText(e)); setState('error'); } }
    }, 2000);
    return () => { cancelled = true; window.clearInterval(timer); };
  }, [open, session, state, bot, onClose, t]);

  return (
    <Dialog open={open} onOpenChange={(v) => { if (!v) onClose(false); }}>
      <DialogContent className="max-w-sm">
        <DialogHeader>
          <DialogTitle>{t('bots.qr_title')}</DialogTitle>
          <DialogDescription>{t('bots.qr_hint')}</DialogDescription>
        </DialogHeader>
        <DialogBody className="flex flex-col items-center gap-3 py-2">
          <div className="flex h-56 w-56 items-center justify-center rounded-lg border bg-white p-3">
            {session && state !== 'loading' ? <QRCodeSVG value={session.url} size={200} className={cn(state !== 'waiting' && 'opacity-20')} />
              : <RefreshCw className="h-6 w-6 animate-spin text-muted-foreground" />}
          </div>
          <p className="text-sm text-muted-foreground">
            {state === 'waiting' && t('bots.qr_waiting')}
            {state === 'expired' && t('bots.qr_expired')}
            {state === 'error' && t('common.failed', { error })}
          </p>
          {(state === 'expired' || state === 'error') && <Button size="sm" variant="outline" onClick={start}><RefreshCw className="mr-2 h-4 w-4" />{t('bots.qr_refresh')}</Button>}
        </DialogBody>
      </DialogContent>
    </Dialog>
  );
}

export function BotsPage() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const bots = useQuery({ queryKey: ['bots'], queryFn: api.bots, refetchInterval: 5000 });
  const [draft, setDraft] = React.useState<Partial<Bot> | null>(null);
  const [qrFor, setQrFor] = React.useState<Partial<Bot> | null>(null);
  const [removing, setRemoving] = React.useState<Bot | null>(null);

  const refresh = () => { void client.invalidateQueries({ queryKey: ['bots'] }); void client.invalidateQueries({ queryKey: ['status'] }); };
  const save = useMutation({
    mutationFn: (b: Partial<Bot>) => api.saveBot(b),
    onSuccess: () => { toast.success(t('common.saved')); setDraft(null); refresh(); },
    onError: (e) => toast.error(t('common.failed', { error: errorText(e) })),
  });
  const remove = useMutation({
    mutationFn: (id: string) => api.deleteBot(id),
    onSuccess: () => { toast.success(t('common.deleted')); setRemoving(null); refresh(); },
    onError: (e) => toast.error(t('common.failed', { error: errorText(e) })),
  });

  const newBot = (type: Bot['type']) => setDraft(type === 'official'
    ? { type, enabled: true, name: '', mode: 'gateway', auto_ack_interactions: true, subscribe_group_member_events: false }
    : { type, enabled: true, name: '', url: 'ws://127.0.0.1:3001' });
  const editable = (b: Bot): Partial<Bot> => { const { runtime: _r, ...rest } = b; return { ...rest, app_secret: '', token: '' }; };

  const addMenu = (
    <DropdownMenu>
      <DropdownMenuTrigger asChild><Button><Plus className="mr-2 h-4 w-4" />{t('bots.add')}<ChevronDown className="ml-2 h-4 w-4" /></Button></DropdownMenuTrigger>
      <DropdownMenuContent align="end">
        <DropdownMenuItem onClick={() => newBot('official')}><BotIcon className="mr-2 h-4 w-4" />{t('bots.add_official')}</DropdownMenuItem>
        <DropdownMenuItem onClick={() => setQrFor({ type: 'official', enabled: true })}><QrCode className="mr-2 h-4 w-4" />{t('bots.qr_login')}</DropdownMenuItem>
        <DropdownMenuSeparator />
        <DropdownMenuItem onClick={() => newBot('onebot')}><Users className="mr-2 h-4 w-4" />{t('bots.add_onebot')}</DropdownMenuItem>
      </DropdownMenuContent>
    </DropdownMenu>
  );

  const list = bots.data ?? [];
  return (
    <>
      <PageHeader title={t('bots.title')} description={t('bots.description')} actions={addMenu} />
      {bots.isLoading ? null : list.length === 0
        ? <EmptyState icon={BotIcon} title={t('bots.empty')} hint={t('bots.empty_hint')} action={addMenu} />
        : <CardGrid>{list.map((b) => (
            <BotCard key={b.id} bot={b} onEdit={() => setDraft(editable(b))} onDelete={() => setRemoving(b)}
              onToggle={(v) => save.mutate({ ...editable(b), enabled: v })} onQr={() => setQrFor(editable(b))} />
          ))}</CardGrid>}

      <Dialog open={draft !== null} onOpenChange={(v) => { if (!v) setDraft(null); }}>
        <DialogContent className="max-w-lg">
          <DialogHeader>
            <DialogTitle>{t(draft?.id ? 'bots.edit_title' : 'bots.add_title')} · {t(draft?.type === 'onebot' ? 'bots.type_onebot' : 'bots.type_official')}</DialogTitle>
          </DialogHeader>
          {draft && <DialogBody><BotForm draft={draft} onChange={setDraft} onQr={() => { setQrFor(draft); setDraft(null); }} /></DialogBody>}
          <DialogFooter>
            <Button variant="outline" onClick={() => setDraft(null)}>{t('common.cancel')}</Button>
            <Button disabled={save.isPending || !draft?.name?.trim()} onClick={() => draft && save.mutate(draft)}>{t('common.save')}</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <QrDialog open={qrFor !== null} bot={qrFor} onClose={(saved) => { setQrFor(null); if (saved) refresh(); }} />

      <Dialog open={removing !== null} onOpenChange={(v) => { if (!v) setRemoving(null); }}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>{t('common.delete')}</DialogTitle>
            <DialogDescription>{removing && t('bots.delete_confirm', { name: removing.name })}</DialogDescription>
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
