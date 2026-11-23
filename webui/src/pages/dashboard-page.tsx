import type React from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery } from '@tanstack/react-query';
import { ArrowDownLeft, ArrowUpRight, Clock, Globe, Plug, Radio, Send, Server, Users } from 'lucide-react';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { PageHeader } from '@/components/page-header';
import { StatusDot } from '@/components/form';
import { PlatformIcon, PlatformLabel } from '@/components/platform-icon';
import { ServerInfo } from '@/components/dashboard/server-info';
import { api, type Status } from '@/lib/api';
import { currentLocale } from '@/i18n';
import { formatNumber } from '@/lib/utils';

function useDuration() {
  const { t } = useTranslation();
  return (since: number) => {
    const seconds = Math.max(0, Math.floor(Date.now() / 1000) - since);
    const d = Math.floor(seconds / 86400), h = Math.floor((seconds % 86400) / 3600), m = Math.floor((seconds % 3600) / 60);
    if (d > 0) return t('dashboard.duration_days', { d, h });
    if (h > 0) return t('dashboard.duration_hours', { h, m });
    return t('dashboard.duration_minutes', { m });
  };
}

function Metric({ icon: Icon, label, value }: { icon: React.ComponentType<{ className?: string }>; label: string; value: React.ReactNode }) {
  return (
    <Card className="v2-metric">
      <div className="flex items-center gap-2 text-xs font-medium text-muted-foreground"><Icon className="h-4 w-4" />{label}</div>
      <div className="v2-metric-value mt-2">{value}</div>
    </Card>
  );
}

function botTone(b: Status['bots'][number]): 'ok' | 'warn' | 'off' {
  if (!b.enabled) return 'off';
  return b.runtime?.online ? 'ok' : 'warn';
}

function connectionStatus(c: Status['connections'][number], t: (k: string, o?: Record<string, unknown>) => string): { tone: 'ok' | 'warn' | 'off'; text: string } {
  if (!c.enabled) return { tone: 'off', text: t('status.disabled') };
  const rt = c.runtime ?? {};
  if (c.type === 'ws_server') return { tone: (rt.clients ?? 0) > 0 ? 'ok' : 'warn', text: t('connections.clients', { count: rt.clients ?? 0 }) };
  if (c.type === 'http_server') return { tone: 'ok', text: t('connections.listening') };
  if (c.type === 'ws_client') {
    const states = Object.values(rt.reverse ?? {});
    const up = states.filter((s) => s.connected).length;
    return { tone: states.length && up === states.length ? 'ok' : 'warn', text: `${t('connections.connected')} ${up}/${states.length}` };
  }
  const last = rt.last_post;
  if (!last) return { tone: 'warn', text: t('connections.no_report') };
  return last.ok ? { tone: 'ok', text: t('connections.last_ok', { status: last.status }) } : { tone: 'warn', text: t('connections.last_failed', { error: last.error || `HTTP ${last.status}` }) };
}

const CONN_ICON = { http_server: Server, ws_server: Plug, http_client: Send, ws_client: Globe } as const;

export function DashboardPage() {
  const { t } = useTranslation();
  const duration = useDuration();
  const locale = currentLocale();
  const { data: status } = useQuery({ queryKey: ['status'], queryFn: api.status, refetchInterval: 5000 });
  if (!status) return <PageHeader title={t('dashboard.title')} description={t('common.loading')} />;
  const officials = status.bots.filter((b) => b.type === 'official' && b.enabled);
  const online = officials.filter((b) => b.runtime?.online).length;

  return (
    <>
      <PageHeader title={t('dashboard.title')} description={t('dashboard.description')} />
      <div className="grid grid-cols-2 gap-3 lg:grid-cols-4">
        <Metric icon={Radio} label={t('dashboard.online_bots')} value={`${online} / ${officials.length}`} />
        <Metric icon={ArrowDownLeft} label={t('dashboard.received')} value={formatNumber(status.received, locale)} />
        <Metric icon={ArrowUpRight} label={t('dashboard.sent')} value={formatNumber(status.sent, locale)} />
        <Metric icon={Clock} label={t('dashboard.uptime')} value={<span className="text-lg">{duration(status.started_at)}</span>} />
      </div>
      <div className="grid min-w-0 grid-cols-1 gap-6">
        <div className="flex min-h-[22rem] min-w-0 flex-col"><ServerInfo /></div>
        <Card>
          <CardHeader><CardTitle>{t('dashboard.bots')}</CardTitle></CardHeader>
          <CardContent className="space-y-3">
            {status.bots.length === 0 && (
              <div className="flex flex-wrap items-center justify-between gap-3">
                <p className="text-sm text-muted-foreground">{t('dashboard.no_bots')}</p>
                <Button size="sm" onClick={() => { window.location.hash = '/bots'; }}>{t('dashboard.go_bots')}</Button>
              </div>
            )}
            {status.bots.map((b) => (
              <div key={b.id} className="flex items-center gap-3">
                <div className="relative shrink-0">
                  {b.runtime?.avatar ? <img src={b.runtime.avatar} alt="" className="h-9 w-9 rounded-full" />
                    : <div className="flex h-9 w-9 items-center justify-center rounded-full bg-primary/10"><PlatformIcon type={b.type} /></div>}
                  <span className="absolute -bottom-0.5 -right-0.5"><StatusDot tone={botTone(b)} /></span>
                </div>
                <div className="min-w-0 flex-1">
                  <p className="truncate text-sm font-medium">{b.name || b.runtime?.display_name || b.runtime?.nickname || b.id}</p>
                  <p className="truncate font-mono text-xs text-muted-foreground">{b.runtime?.self_id || '—'}</p>
                </div>
                <PlatformLabel type={b.type} />
              </div>
            ))}
          </CardContent>
        </Card>
        <Card>
          <CardHeader><CardTitle>{t('dashboard.connections')}</CardTitle></CardHeader>
          <CardContent className="space-y-3">
            {status.connections.length === 0 && <p className="text-sm text-muted-foreground">{t('dashboard.no_connections')}</p>}
            {status.connections.map((c) => {
              const Icon = CONN_ICON[c.type];
              const s = connectionStatus(c, t);
              return (
                <div key={c.id} className="flex items-center gap-3">
                  <div className="relative shrink-0">
                    <div className="flex h-9 w-9 items-center justify-center rounded-full bg-primary/10"><Icon className="h-4 w-4 text-primary" /></div>
                    <span className="absolute -bottom-0.5 -right-0.5"><StatusDot tone={s.tone} /></span>
                  </div>
                  <div className="min-w-0 flex-1">
                    <p className="truncate text-sm font-medium">{c.name}</p>
                    <p className="truncate font-mono text-xs text-muted-foreground">{c.type.endsWith('server') ? `${c.host}:${c.port}` : c.url}</p>
                  </div>
                  <span className="max-w-[45%] truncate text-right text-xs text-muted-foreground" title={s.text}>{s.text}</span>
                </div>
              );
            })}
          </CardContent>
        </Card>
      </div>
      <div className="grid grid-cols-2 gap-3 lg:grid-cols-4">
        <Metric icon={Users} label={t('dashboard.users')} value={formatNumber(status.counts.users, locale)} />
        <Metric icon={Users} label={t('dashboard.groups')} value={formatNumber(status.counts.groups, locale)} />
        <Metric icon={Users} label={t('dashboard.bound')} value={formatNumber(status.counts.bound, locale)} />
        <Metric icon={Users} label={t('dashboard.indexed')} value={formatNumber(status.counts.messages, locale)} />
      </div>
    </>
  );
}
