import React from 'react';
import { useTranslation } from 'react-i18next';
import { useQueryClient } from '@tanstack/react-query';
import { toast } from 'sonner';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { Brand } from '@/components/layout/brand';
import { api } from '@/lib/api';

function AuthCard({ title, description, children }: { title: string; description?: string; children: React.ReactNode }) {
  return (
    <div className="flex min-h-dvh items-center justify-center bg-background p-4">
      <Card className="w-full max-w-sm">
        <CardHeader>
          <div className="mb-2"><Brand /></div>
          <CardTitle>{title}</CardTitle>
          {description && <CardDescription>{description}</CardDescription>}
        </CardHeader>
        <CardContent>{children}</CardContent>
      </Card>
    </div>
  );
}

export function LoginPage() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const [password, setPassword] = React.useState('');
  const [error, setError] = React.useState('');
  const [busy, setBusy] = React.useState(false);

  const submit = async (event: React.FormEvent) => {
    event.preventDefault();
    setBusy(true);
    setError('');
    try {
      await api.login(password);
      // 重新读取会话：自动生成的密码登录后要先改密码。
      await client.fetchQuery({ queryKey: ['session'], queryFn: api.session }).then((s) => client.setQueryData(['session'], s));
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <AuthCard title={t('login.title')}>
      <form onSubmit={submit} className="space-y-4">
        <div className="space-y-2">
          <Label htmlFor="password">{t('login.password')}</Label>
          <Input id="password" type="password" autoComplete="current-password" autoFocus value={password}
            onChange={(e) => setPassword(e.target.value)} />
        </div>
        {error && <p role="alert" className="text-sm text-destructive">{error}</p>}
        <Button type="submit" className="w-full" disabled={busy || !password}>{t('login.submit')}</Button>
      </form>
    </AuthCard>
  );
}

/// 首次登录：自动生成的密码必须先改成自己的密码，才能进入面板。
export function ChangePasswordPage() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const [password, setPassword] = React.useState('');
  const [confirm, setConfirm] = React.useState('');
  const [error, setError] = React.useState('');
  const [busy, setBusy] = React.useState(false);
  const mismatch = confirm.length > 0 && password !== confirm;
  const tooShort = password.length > 0 && password.length < 6;

  const submit = async (event: React.FormEvent) => {
    event.preventDefault();
    if (tooShort || mismatch) return;
    setBusy(true);
    setError('');
    try {
      await api.changePassword(password);
      toast.success(t('login.change_done'));
      client.setQueryData(['session'], { authenticated: true, must_change_password: false });
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  };

  const logout = async () => {
    try { await api.logout(); } finally { client.setQueryData(['session'], { authenticated: false }); }
  };

  return (
    <AuthCard title={t('login.change_title')} description={t('login.change_hint')}>
      <form onSubmit={submit} className="space-y-4">
        <div className="space-y-2">
          <Label htmlFor="new-password">{t('login.new_password')}</Label>
          <Input id="new-password" type="password" autoComplete="new-password" autoFocus value={password}
            aria-invalid={tooShort} onChange={(e) => setPassword(e.target.value)} />
          {tooShort && <p className="text-xs text-destructive">{t('login.too_short')}</p>}
        </div>
        <div className="space-y-2">
          <Label htmlFor="confirm-password">{t('login.confirm_password')}</Label>
          <Input id="confirm-password" type="password" autoComplete="new-password" value={confirm}
            aria-invalid={mismatch} onChange={(e) => setConfirm(e.target.value)} />
          {mismatch && <p className="text-xs text-destructive">{t('login.mismatch')}</p>}
        </div>
        {error && <p role="alert" className="text-sm text-destructive">{error}</p>}
        <Button type="submit" className="w-full" disabled={busy || !password || password !== confirm || tooShort}>{t('login.change_submit')}</Button>
        <Button type="button" variant="ghost" className="w-full" onClick={logout}>{t('login.logout')}</Button>
      </form>
    </AuthCard>
  );
}
