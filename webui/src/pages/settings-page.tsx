import React from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { MailCheck, Save, TriangleAlert } from 'lucide-react';
import { toast } from 'sonner';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { PageHeader } from '@/components/page-header';
import { Choice, Field, Toggle, digits, errorText } from '@/components/form';
import { api, type Settings } from '@/lib/api';

export function SettingsPage() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const loaded = useQuery({ queryKey: ['settings'], queryFn: api.settings });
  const [s, setS] = React.useState<Settings | null>(null);
  const [newPassword, setNewPassword] = React.useState('');
  const [testTo, setTestTo] = React.useState('');
  const [busy, setBusy] = React.useState(false);

  React.useEffect(() => { if (loaded.data && !s) setS(structuredClone(loaded.data)); }, [loaded.data, s]);
  if (!s) return <PageHeader title={t('settings.title')} description={t('common.loading')} />;

  const patch = <K extends 'webhook' | 'smtp' | 'bind' | 'webui'>(section: K, value: Partial<Settings[K]>) =>
    setS((prev) => (prev ? { ...prev, [section]: { ...prev[section], ...value } } : prev));
  const set = (value: Partial<Settings>) => setS((prev) => (prev ? { ...prev, ...value } : prev));

  const save = async () => {
    setBusy(true);
    try {
      await api.saveSettings({ ...s, new_password: newPassword });
      setNewPassword('');
      toast.success(t('common.saved'));
      void client.invalidateQueries({ queryKey: ['status'] });
    } catch (e) { toast.error(t('common.failed', { error: errorText(e) })); }
    finally { setBusy(false); }
  };
  const testMail = async () => {
    try { await api.testSmtp(s.smtp, testTo || s.smtp.owner_email || ''); toast.success(t('settings.smtp_test_sent')); }
    catch (e) { toast.error(t('common.failed', { error: errorText(e) })); }
  };

  const publicHost = s.webui.host === '0.0.0.0';
  return (
    <>
      <PageHeader title={t('settings.title')} description={t('settings.description')}
        actions={<Button size="sm" disabled={busy} onClick={save}><Save className="mr-2 h-4 w-4" />{t('common.save')}</Button>} />

      {publicHost && (
        <Card className="border-amber-500/40 bg-amber-500/5">
          <CardContent className="flex items-start gap-3 text-sm"><TriangleAlert className="mt-0.5 h-4 w-4 shrink-0 text-amber-600" />{t('settings.public_warning')}</CardContent>
        </Card>
      )}

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
            <Field label={t('settings.owner_email')}><Input value={s.smtp.owner_email ?? ''} onChange={(e) => patch('smtp', { owner_email: e.target.value.trim() })} /></Field>
          </div>
          <Toggle label={t('settings.notify_offline')} checked={s.smtp.notify_offline !== false} onChange={(v) => patch('smtp', { notify_offline: v })} />
          <div className="flex flex-wrap items-end gap-2 border-t pt-5">
            <div className="min-w-[14rem] flex-1">
              <Field label={t('settings.owner_email')}><Input value={testTo} placeholder={s.smtp.owner_email ?? ''} onChange={(e) => setTestTo(e.target.value.trim())} /></Field>
            </div>
            <Button variant="outline" onClick={testMail}><MailCheck className="mr-2 h-4 w-4" />{t('settings.smtp_test')}</Button>
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
          <Toggle label={t('settings.bind_email')} checked={s.bind.email_enabled !== false} onChange={(v) => patch('bind', { email_enabled: v })} />
          <div className="grid gap-5 md:grid-cols-3">
            <Field label={t('settings.bind_domain')}><Input value={s.bind.email_domain ?? 'qq.com'} onChange={(e) => patch('bind', { email_domain: e.target.value.trim() })} /></Field>
            <Field label={t('settings.bind_ttl')}><Input inputMode="numeric" value={s.bind.code_ttl_minutes ?? 10} onChange={(e) => patch('bind', { code_ttl_minutes: digits(e.target.value) })} /></Field>
          </div>
        </CardContent>
      </Card>

      <Card>
        <CardHeader><CardTitle>{t('settings.webhook')}</CardTitle><CardDescription>{t('settings.webhook_hint')}</CardDescription></CardHeader>
        <CardContent className="grid gap-5 md:grid-cols-3">
          <Field label={t('settings.host')}><Input value={s.webhook.host ?? '0.0.0.0'} onChange={(e) => patch('webhook', { host: e.target.value.trim() })} /></Field>
          <Field label={t('settings.port')}><Input inputMode="numeric" value={s.webhook.port ?? 0} onChange={(e) => patch('webhook', { port: digits(e.target.value) })} /></Field>
          <Field label={t('settings.path')}><Input value={s.webhook.path ?? '/webhook/qqbot'} onChange={(e) => patch('webhook', { path: e.target.value.trim() })} /></Field>
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
        <CardHeader><CardTitle>{t('settings.other')}</CardTitle></CardHeader>
        <CardContent className="grid gap-5 md:grid-cols-3">
          <Field label={t('settings.data_dir')}><Input value={s.data_dir ?? 'data'} onChange={(e) => set({ data_dir: e.target.value })} /></Field>
          <Field label={t('settings.log_level')}>
            <Choice value={s.log_level ?? 'info'} onChange={(v) => set({ log_level: v })} options={['debug', 'info', 'warn', 'error'].map((v) => ({ value: v, label: v }))} />
          </Field>
          <Field label={t('settings.retention')}><Input inputMode="numeric" value={s.message_retention_days ?? 7} onChange={(e) => set({ message_retention_days: digits(e.target.value) })} /></Field>
        </CardContent>
      </Card>
    </>
  );
}
