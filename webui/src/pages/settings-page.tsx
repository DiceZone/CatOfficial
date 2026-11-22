import React from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { MailCheck, Plus, Save, Trash2, TriangleAlert } from 'lucide-react';
import { toast } from 'sonner';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Switch } from '@/components/ui/switch';
import { Label } from '@/components/ui/label';
import { PageHeader } from '@/components/page-header';
import { Choice, Field, TagInput, Toggle, digits, errorText } from '@/components/form';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { api, botLabel, type Settings } from '@/lib/api';

const isEmail = (v: string) => /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(v);
const TRAY_TEXT_LIMIT = 10;
const trayLength = (v: string) => [...v.trim()].length;
const trayInvalid = (v: string) => /[\u0000-\u001f\u007f-\u009f\u2028\u2029]/.test(v.trim());

export function SettingsPage() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const loaded = useQuery({ queryKey: ['settings'], queryFn: api.settings });
  const bots = useQuery({ queryKey: ['bots'], queryFn: api.bots });
  const [s, setS] = React.useState<Settings | null>(null);
  const [newPassword, setNewPassword] = React.useState('');
  const [busy, setBusy] = React.useState(false);

  React.useEffect(() => { if (loaded.data && !s) setS(structuredClone(loaded.data)); }, [loaded.data, s]);
  if (!s) return <PageHeader title={t('settings.title')} description={t('common.loading')} />;

  const officials = (bots.data ?? []).filter((b) => b.type === 'official');
  const patch = <K extends 'webhook' | 'smtp' | 'bind' | 'webui'>(section: K, value: Partial<Settings[K]>) =>
    setS((prev) => (prev ? { ...prev, [section]: { ...prev[section], ...value } } : prev));
  const set = (value: Partial<Settings>) => setS((prev) => (prev ? { ...prev, ...value } : prev));
  const masters = s.masters ?? [];
  const tray = { text: s.tray?.text ?? '', start_hidden: s.tray?.start_hidden ?? true };
  const setTray = (value: Partial<typeof tray>) => set({ tray: { ...tray, ...value } });
  const trayError = trayLength(tray.text) > TRAY_TEXT_LIMIT ? 'limit' : trayInvalid(tray.text) ? 'invalid' : '';
  const setMasters = (list: NonNullable<Settings['masters']>) => set({ masters: list });

  const save = async () => {
    setBusy(true);
    try {
      if (masters.some((m) => m.openid.trim() && !m.bot_id)) { toast.error(t('settings.master_need_bot')); return; }
      if (trayError) { toast.error(t(`settings.tray_${trayError}`)); return; }
      const r = await api.saveSettings({ ...s, masters: masters.filter((m) => m.openid.trim()), new_password: newPassword });
      setNewPassword('');
      toast.success(t('common.saved'));
      if (r.restart_required) void client.invalidateQueries({ queryKey: ['status'] });
    } catch (e) { toast.error(t('common.failed', { error: errorText(e) })); }
    finally { setBusy(false); }
  };
  const testMail = async () => {
    try { await api.testSmtp(s.smtp); toast.success(t('settings.smtp_test_sent')); }
    catch (e) { toast.error(t('common.failed', { error: errorText(e) })); }
  };

  return (
    <>
      <PageHeader title={t('settings.title')} description={t('settings.description')}
        actions={<Button size="sm" disabled={busy} onClick={save}><Save className="mr-2 h-4 w-4" />{t('common.save')}</Button>} />

      {s.webui.host === '0.0.0.0' && (
        <Card className="border-amber-500/40 bg-amber-500/5">
          <CardContent className="flex items-start gap-3 text-sm"><TriangleAlert className="mt-0.5 h-4 w-4 shrink-0 text-amber-600" />{t('settings.public_warning')}</CardContent>
        </Card>
      )}

      <Card>
        <CardHeader><CardTitle>{t('settings.masters')}</CardTitle><CardDescription>{t('settings.masters_hint')}</CardDescription></CardHeader>
        <CardContent className="space-y-3">
          {masters.length === 0 && <p className="text-sm text-muted-foreground">{t('settings.master_empty')}</p>}
          {masters.map((m, i) => (
            <div key={i} className="grid items-end gap-3 sm:grid-cols-[14rem_1fr_auto]">
              <Field label={t('settings.master_bot')}>
                <Select value={m.bot_id || undefined} onValueChange={(v) => setMasters(masters.map((x, j) => (j === i ? { ...x, bot_id: v } : x)))}>
                  <SelectTrigger aria-invalid={!m.bot_id && !!m.openid}><SelectValue placeholder={t('settings.master_pick_bot')} /></SelectTrigger>
                  <SelectContent>{officials.map((b) => <SelectItem key={b.id} value={b.id}>{botLabel(b)}</SelectItem>)}</SelectContent>
                </Select>
              </Field>
              <Field label={t('settings.master_openid')}>
                <Input className="font-mono" value={m.openid} onChange={(e) => setMasters(masters.map((x, j) => (j === i ? { ...x, openid: e.target.value.trim() } : x)))} />
              </Field>
              <Button variant="ghost" size="icon" aria-label={t('common.remove')} onClick={() => setMasters(masters.filter((_, j) => j !== i))}><Trash2 className="h-4 w-4" /></Button>
            </div>
          ))}
          <div className="flex flex-wrap items-center justify-between gap-3 pt-2">
            <Button variant="outline" size="sm" disabled={officials.length === 0} onClick={() => setMasters([...masters, { bot_id: officials.length === 1 ? officials[0].id : '', openid: '' }])}><Plus className="mr-1 h-4 w-4" />{t('settings.master_add')}</Button>
            <label className="flex items-center gap-2 text-sm">{t('settings.notify_masters')}<Switch checked={s.notify_masters !== false} onCheckedChange={(v) => set({ notify_masters: v })} /></label>
          </div>
        </CardContent>
      </Card>

      <Card>
        <CardHeader><CardTitle>{t('settings.smtp')}</CardTitle><CardDescription>{t('settings.smtp_hint')}</CardDescription></CardHeader>
        <CardContent className="space-y-5">
          <Toggle label={t('settings.smtp_enabled')} checked={!!s.smtp.enabled} onChange={(v) => patch('smtp', { enabled: v })} />
          <div className="grid gap-5 md:grid-cols-3">
            <Field label={t('settings.smtp_host')}><Input value={s.smtp.host ?? ''} placeholder="smtp.qq.com" onChange={(e) => patch('smtp', { host: e.target.value.trim() })} /></Field>
            <Field label={t('settings.port')}><Input inputMode="numeric" value={s.smtp.port ?? 465} onChange={(e) => patch('smtp', { port: digits(e.target.value) })} /></Field>
            <Field label={t('settings.smtp_security')}>
              <Choice value={s.smtp.security ?? 'ssl'} onChange={(v) => patch('smtp', { security: v as 'ssl' })} options={[
                { value: 'ssl', label: t('settings.security_ssl') }, { value: 'starttls', label: t('settings.security_starttls') }, { value: 'none', label: t('settings.security_none') }]} />
            </Field>
            <Field label={t('settings.smtp_username')}><Input value={s.smtp.username ?? ''} onChange={(e) => patch('smtp', { username: e.target.value.trim() })} /></Field>
            <Field label={t('settings.smtp_password')} hint={s.smtp_password_set ? t('settings.smtp_password_keep') : undefined}>
              <Input type="password" autoComplete="new-password" value={s.smtp.password ?? ''} onChange={(e) => patch('smtp', { password: e.target.value })} />
            </Field>
            <Field label={t('settings.smtp_from')}><Input value={s.smtp.from ?? ''} placeholder="bot@example.com" onChange={(e) => patch('smtp', { from: e.target.value.trim() })} /></Field>
          </div>
          <Toggle label={t('settings.notify_errors')} checked={s.smtp.notify_errors !== false} onChange={(v) => patch('smtp', { notify_errors: v })} />
          <Field label={t('settings.notify_emails')} hint={t('settings.notify_emails_hint')}>
            <TagInput values={s.smtp.notify_emails ?? []} onChange={(v) => patch('smtp', { notify_emails: v })} validate={isEmail} placeholder="name@example.com" />
          </Field>
          <div className="flex flex-wrap items-center gap-3 border-t pt-5">
            <Button variant="outline" onClick={testMail}><MailCheck className="mr-2 h-4 w-4" />{t('settings.smtp_test')}</Button>
            <span className="text-xs text-muted-foreground">{t('settings.smtp_test_hint')}</span>
          </div>
        </CardContent>
      </Card>

      <Card>
        <CardHeader><CardTitle>{t('settings.bind')}</CardTitle><CardDescription>{t('settings.bind_hint')}</CardDescription></CardHeader>
        <CardContent className="space-y-5">
          <Toggle label={t('settings.bind_enabled')} checked={s.bind.enabled !== false} onChange={(v) => patch('bind', { enabled: v })} />
          <div className="grid gap-5 md:grid-cols-3">
            <Field label={t('settings.bind_command')}><Input value={s.bind.command ?? '.bind'} onChange={(e) => patch('bind', { command: e.target.value.trim() })} /></Field>
            <Field label={t('settings.bind_window')}><Input inputMode="numeric" value={s.bind.window_seconds ?? 5} onChange={(e) => patch('bind', { window_seconds: digits(e.target.value) })} /></Field>
            <Field label={t('settings.bind_confirm')} hint={t('settings.bind_confirm_hint')}>
              <Input inputMode="numeric" value={s.bind.group_confirm_users ?? 3} onChange={(e) => patch('bind', { group_confirm_users: digits(e.target.value) })} />
            </Field>
          </div>
          <Toggle label={t('settings.bind_email')} hint={t('settings.bind_email_hint', { command: s.bind.command || '.bind' })} checked={s.bind.email_enabled !== false} onChange={(v) => patch('bind', { email_enabled: v })} />
          <div className="grid gap-5 md:grid-cols-3">
            <Field label={t('settings.bind_ttl')}><Input inputMode="numeric" value={s.bind.code_ttl_minutes ?? 10} onChange={(e) => patch('bind', { code_ttl_minutes: digits(e.target.value) })} /></Field>
          </div>
        </CardContent>
      </Card>

      <Card>
        <CardHeader><CardTitle>{t('settings.webhook')}</CardTitle><CardDescription>{t('settings.webhook_hint')}</CardDescription></CardHeader>
        <CardContent className="grid items-end gap-5 md:grid-cols-[1fr_auto_10rem]">
          <Field label={t('settings.path')}><Input value={s.webhook.path ?? '/webhook/qqbot'} onChange={(e) => patch('webhook', { path: e.target.value.trim() })} /></Field>
          <div className="flex h-10 items-center gap-2">
            <Switch id="webhook-standalone" checked={!!s.webhook.standalone} onCheckedChange={(v) => patch('webhook', { standalone: v })} />
            <Label htmlFor="webhook-standalone">{t('settings.webhook_standalone')}</Label>
          </div>
          {s.webhook.standalone
            ? <Field label={t('settings.port')}><Input inputMode="numeric" value={s.webhook.port ?? 8443} onChange={(e) => patch('webhook', { port: digits(e.target.value) })} /></Field>
            : <div />}
        </CardContent>
      </Card>

      <Card>
        <CardHeader><CardTitle>{t('settings.webui')}</CardTitle></CardHeader>
        <CardContent className="grid gap-5 md:grid-cols-3">
          <Field label={t('settings.host')}><Input value={s.webui.host ?? '127.0.0.1'} onChange={(e) => patch('webui', { host: e.target.value.trim() })} /></Field>
          <Field label={t('settings.port')}><Input inputMode="numeric" value={s.webui.port ?? 18082} onChange={(e) => patch('webui', { port: digits(e.target.value) })} /></Field>
          <Field label={t('settings.new_password')} hint={t('settings.new_password_hint')} id="new-password">
            <Input id="new-password" type="password" autoComplete="new-password" value={newPassword} onChange={(e) => setNewPassword(e.target.value)} />
          </Field>
        </CardContent>
      </Card>

      <Card>
        <CardHeader><CardTitle>{t('settings.tray_title')}</CardTitle><CardDescription>{t('settings.tray_desc')}</CardDescription></CardHeader>
        <CardContent className="space-y-5">
          {!s.tray_supported && <p className="text-sm text-muted-foreground">{t('settings.tray_unsupported')}</p>}
          <div className="grid gap-5 md:grid-cols-3">
            <Field label={t('settings.tray_label')} id="tray-text">
              <Input id="tray-text" value={tray.text} placeholder="CatOfficial" disabled={!s.tray_supported} aria-invalid={!!trayError} onChange={(e) => setTray({ text: e.target.value })} />
              <div className="flex items-start justify-between gap-3 text-xs leading-relaxed text-muted-foreground">
                <p>{t('settings.tray_hint')}</p><span className="shrink-0">{trayLength(tray.text)}/{TRAY_TEXT_LIMIT}</span>
              </div>
              {trayError && <p role="alert" className="text-sm text-destructive">{t(`settings.tray_${trayError}`)}</p>}
              <p className="text-sm break-words">{t('settings.tray_preview')} <code>{`${tray.text.trim() || 'CatOfficial'}(${s.tray_port ?? s.webui.port ?? 18082})`}</code></p>
            </Field>
            <div className="md:col-span-2">
              <Toggle label={t('settings.tray_start_hidden')} hint={t('settings.tray_start_hidden_hint')} checked={tray.start_hidden} onChange={(v) => setTray({ start_hidden: v })} />
            </div>
          </div>
        </CardContent>
      </Card>

      <Card>
        <CardHeader><CardTitle>{t('settings.other')}</CardTitle></CardHeader>
        <CardContent className="grid gap-5 md:grid-cols-3">
          <Field label={t('settings.log_level')}>
            <Choice value={s.log_level ?? 'info'} onChange={(v) => set({ log_level: v })} options={['debug', 'info', 'warn', 'error'].map((v) => ({ value: v, label: v }))} />
          </Field>
          <Field label={t('settings.retention')} hint={t('settings.retention_hint')}><Input inputMode="numeric" value={s.message_retention_days ?? 365} onChange={(e) => set({ message_retention_days: digits(e.target.value) })} /></Field>
          <Field label={t('settings.image_cache_days')} hint={<ImageCacheUsage />}><Input inputMode="numeric" value={s.image_cache_days ?? 7} onChange={(e) => set({ image_cache_days: digits(e.target.value) })} /></Field>
        </CardContent>
      </Card>
    </>
  );
}

/// 图片缓存目前的占用，以及清空按钮。
function ImageCacheUsage() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const usage = useQuery({ queryKey: ['image-cache'], queryFn: api.imageCache });
  const [busy, setBusy] = React.useState(false);
  const size = usage.data ? (usage.data.bytes >= 1 << 20 ? `${(usage.data.bytes / (1 << 20)).toFixed(1)} MB` : `${Math.ceil(usage.data.bytes / 1024)} KB`) : '—';
  return <span className="flex flex-wrap items-center gap-2">
    {t('settings.image_cache_usage', { files: usage.data?.files ?? 0, size })}
    <button type="button" disabled={busy || !usage.data?.files} className="text-primary hover:underline disabled:pointer-events-none disabled:opacity-50" onClick={async () => {
      setBusy(true);
      try { const r = await api.clearImageCache(); toast.success(t('settings.image_cache_cleared', { n: r.removed })); void client.invalidateQueries({ queryKey: ['image-cache'] }); }
      catch (e) { toast.error(t('common.failed', { error: errorText(e) })); }
      finally { setBusy(false); }
    }}>{t('settings.image_cache_clear')}</button>
  </span>;
}
