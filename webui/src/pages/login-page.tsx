import React from 'react';
import { useTranslation } from 'react-i18next';
import { useQueryClient } from '@tanstack/react-query';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { Brand } from '@/components/layout/brand';
import { api } from '@/lib/api';

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
      client.setQueryData(['session'], { authenticated: true });
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="flex min-h-dvh items-center justify-center bg-background p-4">
      <Card className="w-full max-w-sm">
        <CardHeader>
          <div className="mb-2"><Brand /></div>
          <CardTitle>{t('login.title')}</CardTitle>
          <CardDescription>{t('app.tagline')}</CardDescription>
        </CardHeader>
        <CardContent>
          <form onSubmit={submit} className="space-y-4">
            <div className="space-y-2">
              <Label htmlFor="password">{t('login.password')}</Label>
              <Input id="password" type="password" autoComplete="current-password" autoFocus value={password}
                onChange={(e) => setPassword(e.target.value)} />
            </div>
            {error && <p role="alert" className="text-sm text-destructive">{error}</p>}
            <Button type="submit" className="w-full" disabled={busy || !password}>{t('login.submit')}</Button>
            <p className="text-xs leading-relaxed text-muted-foreground">{t('login.hint')}</p>
          </form>
        </CardContent>
      </Card>
    </div>
  );
}
