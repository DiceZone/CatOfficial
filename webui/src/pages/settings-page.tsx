import React from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery } from '@tanstack/react-query';
import { Plus, RotateCw, Save, Trash2, TriangleAlert } from 'lucide-react';
import { toast } from 'sonner';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { PageHeader } from '@/components/page-header';
import { api, type ConfigJson } from '@/lib/api';

function Field({ label, hint, children, id }: { label: string; hint?: string; children: React.ReactNode; id?: string }) {
  return (
    <div className="space-y-2">
      <Label htmlFor={id}>{label}</Label>
      {children}
      {hint && <p className="text-xs leading-relaxed text-muted-foreground">{hint}</p>}
    </div>
  );
}

function Toggle({ label, hint, checked, onChange }: { label: string; hint?: string; checked: boolean; onChange: (v: boolean) => void }) {
  return (
    <div className="flex items-start justify-between gap-4">
      <div>
        <p className="text-sm font-medium">{label}</p>
        {hint && <p className="mt-1 text-xs leading-relaxed text-muted-foreground">{hint}</p>}
      </div>
      <Switch checked={checked} onCheckedChange={onChange} aria-label={label} />
    </div>
  );
}

function Choice({ value, onChange, options }: { value: string; onChange: (v: string) => void; options: { value: string; label: string }[] }) {
  return (
    <Select value={value} onValueChange={onChange}>
      <SelectTrigger><SelectValue /></SelectTrigger>
      <SelectContent>{options.map((o) => <SelectItem key={o.value} value={o.value}>{o.label}</SelectItem>)}</SelectContent>
    </Select>
  );
}

const num = (value: string) => (value === '' ? 0 : Number(value.replace(/\D/g, '')));

async function waitUntilBack(port: number) {
  const target = `${window.location.protocol}//${window.location.hostname}:${port}/`;
  const samePort = String(port) === (window.location.port || (window.location.protocol === 'https:' ? '443' : '80'));
  for (let i = 0; i < 40; i++) {
    await new Promise((r) => setTimeout(r, 1000));
    if (!samePort) continue;
    try { await api.session(); return true; } catch { /* 还没起来 */ }
  }
  if (!samePort) { window.location.href = target; return true; }
  return false;
}

export function SettingsPage() {
  const { t } = useTranslation();
  const loaded = useQuery({ queryKey: ['config'], queryFn: api.config });
  const [config, setConfig] = React.useState<ConfigJson | null>(null);
  const [newPassword, setNewPassword] = React.useState('');
  const [saving, setSaving] = React.useState(false);
  const [needRestart, setNeedRestart] = React.useState(false);
  const [restarting, setRestarting] = React.useState(false);

  React.useEffect(() => { if (loaded.data && !config) setConfig(structuredClone(loaded.data.config)); }, [loaded.data, config]);
  if (!config) return <PageHeader title={t('settings.title')} description={t('common.loading')} />;

  const bot = config.bot ?? {};
  const webhook = config.webhook ?? {};
  const onebot = config.onebot ?? {};
  const webui = config.webui ?? {};
  const servers: { type: string; host: string; port: number }[] = onebot.servers ?? [];
  const posts: { url: string; secret?: string }[] = onebot.http_post ?? [];
  const reverse: { url: string; reconnect_interval?: number }[] = onebot.ws_reverse ?? [];

  const patch = (section: string | null, key: string, value: unknown) => setConfig((prev) => {
    const next = structuredClone(prev ?? {});
    if (section) next[section] = { ...(next[section] ?? {}), [key]: value };
    else next[key] = value;
    return next;
  });
  const patchList = <T,>(key: string, list: T[]) => patch('onebot', key, list);

  const save = async () => {
    setSaving(true);
    try {
      await api.saveConfig(config, newPassword);
      setNewPassword('');
      setNeedRestart(true);
      toast.success(t('settings.saved'));
    } catch (e) {
      toast.error(t('common.failed', { error: e instanceof Error ? e.message : String(e) }));
    } finally {
      setSaving(false);
    }
  };
  const restart = async () => {
    setRestarting(true);
    try {
      await api.restart();
      if (await waitUntilBack(Number(webui.port) || 6099)) { toast.success(t('settings.restart_done')); setNeedRestart(false); }
    } catch (e) {
      toast.error(t('common.failed', { error: e instanceof Error ? e.message : String(e) }));
    } finally {
      setRestarting(false);
    }
  };
  const publicHosts = [webui.host, ...servers.map((s) => s.host)].includes('0.0.0.0');

  return (
    <>
      <PageHeader title={t('settings.title')} description={t('settings.description')}
        actions={<>
          {needRestart && <Button variant="outline" size="sm" disabled={restarting} onClick={restart}>
            <RotateCw className={restarting ? 'mr-2 h-4 w-4 animate-spin' : 'mr-2 h-4 w-4'} />{t(restarting ? 'settings.restarting' : 'settings.restart')}</Button>}
          <Button size="sm" disabled={saving} onClick={save}><Save className="mr-2 h-4 w-4" />{t('common.save')}</Button>
        </>} />

      {publicHosts && (
        <Card className="border-amber-500/40 bg-amber-500/5">
          <CardContent className="flex items-start gap-3 text-sm"><TriangleAlert className="mt-0.5 h-4 w-4 shrink-0 text-amber-600" />{t('settings.public_warning')}</CardContent>
        </Card>
      )}

      <Card>
        <CardHeader><CardTitle>{t('settings.bot')}</CardTitle></CardHeader>
        <CardContent className="grid gap-5 md:grid-cols-2">
          <Field label={t('settings.app_id')} id="app-id"><Input id="app-id" value={bot.app_id ?? ''} onChange={(e) => patch('bot', 'app_id', e.target.value.trim())} /></Field>
          <Field label={t('settings.app_secret')} id="app-secret" hint={loaded.data?.app_secret_set ? t('settings.app_secret_keep') : undefined}>
            <Input id="app-secret" type="password" autoComplete="new-password" value={bot.app_secret ?? ''} onChange={(e) => patch('bot', 'app_secret', e.target.value.trim())} />
          </Field>
          <Field label={t('settings.mode')}>
            <Choice value={bot.mode ?? 'gateway'} onChange={(v) => patch('bot', 'mode', v)}
              options={[{ value: 'gateway', label: t('dashboard.mode_gateway') }, { value: 'webhook', label: t('dashboard.mode_webhook') }]} />
          </Field>
          <div className="space-y-4 md:col-span-2">
            <Toggle label={t('settings.subscribe')} hint={t('settings.subscribe_hint')} checked={!!bot.subscribe_group_member_events}
              onChange={(v) => patch('bot', 'subscribe_group_member_events', v)} />
            <Toggle label={t('settings.auto_ack')} hint={t('settings.auto_ack_hint')} checked={bot.auto_ack_interactions !== false}
              onChange={(v) => patch('bot', 'auto_ack_interactions', v)} />
          </div>
        </CardContent>
      </Card>

      {bot.mode === 'webhook' && (
        <Card>
          <CardHeader><CardTitle>{t('settings.webhook')}</CardTitle><CardDescription>{t('settings.webhook_hint')}</CardDescription></CardHeader>
          <CardContent className="grid gap-5 md:grid-cols-3">
            <Field label={t('settings.host')}><Input value={webhook.host ?? ''} onChange={(e) => patch('webhook', 'host', e.target.value.trim())} /></Field>
            <Field label={t('settings.port')}><Input inputMode="numeric" value={webhook.port ?? ''} onChange={(e) => patch('webhook', 'port', num(e.target.value))} /></Field>
            <Field label={t('settings.path')}><Input value={webhook.path ?? ''} onChange={(e) => patch('webhook', 'path', e.target.value.trim())} /></Field>
          </CardContent>
        </Card>
      )}

      <Card>
        <CardHeader><CardTitle>{t('settings.onebot')}</CardTitle></CardHeader>
        <CardContent className="space-y-6">
          <div className="grid gap-5 md:grid-cols-3">
            <Field label={t('settings.access_token')} hint={t('settings.access_token_hint')}>
              <Input value={onebot.access_token ?? ''} onChange={(e) => patch('onebot', 'access_token', e.target.value.trim())} />
            </Field>
            <Field label={t('settings.message_format')}>
              <Choice value={onebot.message_format ?? 'array'} onChange={(v) => patch('onebot', 'message_format', v)}
                options={[{ value: 'array', label: t('settings.format_array') }, { value: 'string', label: t('settings.format_string') }]} />
            </Field>
            <Field label={t('settings.heartbeat')}>
              <Input inputMode="numeric" value={onebot.heartbeat_interval ?? 30000} onChange={(e) => patch('onebot', 'heartbeat_interval', num(e.target.value))} />
            </Field>
          </div>

          <ListSection title={t('settings.servers')} addLabel={t('settings.add_server')}
            onAdd={() => patchList('servers', [...servers, { type: 'ws', host: '127.0.0.1', port: 6700 }])}>
            {servers.map((s, i) => (
              <div key={i} className="grid items-end gap-3 sm:grid-cols-[9rem_1fr_7rem_auto]">
                <Field label={t('settings.server_type')}>
                  <Choice value={s.type} onChange={(v) => patchList('servers', servers.map((x, j) => (j === i ? { ...x, type: v } : x)))}
                    options={[{ value: 'http', label: t('dashboard.http_api') }, { value: 'ws', label: t('dashboard.ws_server') }]} />
                </Field>
                <Field label={t('settings.host')}><Input value={s.host} onChange={(e) => patchList('servers', servers.map((x, j) => (j === i ? { ...x, host: e.target.value.trim() } : x)))} /></Field>
                <Field label={t('settings.port')}><Input inputMode="numeric" value={s.port} onChange={(e) => patchList('servers', servers.map((x, j) => (j === i ? { ...x, port: num(e.target.value) } : x)))} /></Field>
                <RemoveButton label={t('common.remove')} onClick={() => patchList('servers', servers.filter((_, j) => j !== i))} />
              </div>
            ))}
          </ListSection>

          <ListSection title={t('settings.http_post')} addLabel={t('settings.add_post')}
            onAdd={() => patchList('http_post', [...posts, { url: 'http://127.0.0.1:8080/', secret: '' }])}>
            {posts.map((p, i) => (
              <div key={i} className="grid items-end gap-3 sm:grid-cols-[1fr_14rem_auto]">
                <Field label={t('settings.url')}><Input value={p.url} onChange={(e) => patchList('http_post', posts.map((x, j) => (j === i ? { ...x, url: e.target.value.trim() } : x)))} /></Field>
                <Field label={t('settings.secret')}><Input value={p.secret ?? ''} onChange={(e) => patchList('http_post', posts.map((x, j) => (j === i ? { ...x, secret: e.target.value } : x)))} /></Field>
                <RemoveButton label={t('common.remove')} onClick={() => patchList('http_post', posts.filter((_, j) => j !== i))} />
              </div>
            ))}
          </ListSection>

          <ListSection title={t('settings.ws_reverse')} addLabel={t('settings.add_reverse')}
            onAdd={() => patchList('ws_reverse', [...reverse, { url: 'ws://127.0.0.1:8080/onebot/v11/ws', reconnect_interval: 3000 }])}>
            {reverse.map((r, i) => (
              <div key={i} className="grid items-end gap-3 sm:grid-cols-[1fr_10rem_auto]">
                <Field label={t('settings.url')}><Input value={r.url} onChange={(e) => patchList('ws_reverse', reverse.map((x, j) => (j === i ? { ...x, url: e.target.value.trim() } : x)))} /></Field>
                <Field label={t('settings.reconnect')}><Input inputMode="numeric" value={r.reconnect_interval ?? 3000} onChange={(e) => patchList('ws_reverse', reverse.map((x, j) => (j === i ? { ...x, reconnect_interval: num(e.target.value) } : x)))} /></Field>
                <RemoveButton label={t('common.remove')} onClick={() => patchList('ws_reverse', reverse.filter((_, j) => j !== i))} />
              </div>
            ))}
          </ListSection>
        </CardContent>
      </Card>

      <Card>
        <CardHeader><CardTitle>{t('settings.webui')}</CardTitle></CardHeader>
        <CardContent className="grid gap-5 md:grid-cols-3">
          <Field label={t('settings.host')}><Input value={webui.host ?? '127.0.0.1'} onChange={(e) => patch('webui', 'host', e.target.value.trim())} /></Field>
          <Field label={t('settings.port')}><Input inputMode="numeric" value={webui.port ?? 6099} onChange={(e) => patch('webui', 'port', num(e.target.value))} /></Field>
          <Field label={t('settings.new_password')} hint={t('settings.new_password_hint')} id="new-password">
            <Input id="new-password" type="password" autoComplete="new-password" value={newPassword} onChange={(e) => setNewPassword(e.target.value)} />
          </Field>
        </CardContent>
      </Card>

      <Card>
        <CardHeader><CardTitle>{t('settings.other')}</CardTitle></CardHeader>
        <CardContent className="grid gap-5 md:grid-cols-3">
          <Field label={t('settings.data_dir')}><Input value={config.data_dir ?? 'data'} onChange={(e) => patch(null, 'data_dir', e.target.value)} /></Field>
          <Field label={t('settings.log_level')}>
            <Choice value={config.log_level ?? 'info'} onChange={(v) => patch(null, 'log_level', v)}
              options={['debug', 'info', 'warn', 'error'].map((v) => ({ value: v, label: v }))} />
          </Field>
          <Field label={t('settings.retention')}>
            <Input inputMode="numeric" value={config.message_retention_days ?? 7} onChange={(e) => patch(null, 'message_retention_days', num(e.target.value))} />
          </Field>
        </CardContent>
      </Card>
    </>
  );
}

function ListSection({ title, addLabel, onAdd, children }: { title: string; addLabel: string; onAdd: () => void; children: React.ReactNode }) {
  return (
    <section className="space-y-3 border-t pt-5">
      <div className="flex items-center justify-between gap-3">
        <h4 className="text-sm font-semibold">{title}</h4>
        <Button variant="outline" size="sm" onClick={onAdd}><Plus className="mr-1 h-4 w-4" />{addLabel}</Button>
      </div>
      <div className="space-y-3">{children}</div>
    </section>
  );
}

function RemoveButton({ label, onClick }: { label: string; onClick: () => void }) {
  return <Button variant="ghost" size="icon" aria-label={label} title={label} onClick={onClick}><Trash2 className="h-4 w-4" /></Button>;
}
