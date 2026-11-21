import type React from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery } from '@tanstack/react-query';
import { ArrowDownLeft, ArrowUpRight, Clock, Globe, Plug, Radio, Send, Server } from 'lucide-react';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { PageHeader } from '@/components/page-header';
import { api, type Status } from '@/lib/api';
import { currentLocale } from '@/i18n';
import { formatNumber, formatTime } from '@/lib/utils';

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

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex flex-wrap items-baseline justify-between gap-x-4 gap-y-1 border-b py-2.5 last:border-b-0">
      <span className="text-sm text-muted-foreground">{label}</span>
      <span className="min-w-0 break-all text-right text-sm font-medium">{children}</span>
    </div>
  );
}

function StatusBadge({ status }: { status: Status }) {
  const { t } = useTranslation();
  if (!status.configured) return <Badge variant="warning">{t('dashboard.unconfigured')}</Badge>;
  return status.online ? <Badge variant="success">{t('dashboard.online')}</Badge> : <Badge variant="danger">{t('dashboard.offline')}</Badge>;
}

export function DashboardPage() {
  const { t } = useTranslation();
  const duration = useDuration();
  const locale = currentLocale();
  const { data: status } = useQuery({ queryKey: ['status'], queryFn: api.status, refetchInterval: 5000 });
  if (!status) return <PageHeader title={t('dashboard.title')} description={t('common.loading')} />;
  const c = status.connections;
  const noConnections = !c.listeners.length && !c.http_post.length && !c.ws_reverse.length;

  return (
    <>
      <PageHeader title={t('dashboard.title')} description={t('dashboard.description')} />
      {!status.configured && (
        <Card className="border-amber-500/40 bg-amber-500/5">
          <CardContent className="flex flex-wrap items-center justify-between gap-3">
            <p className="text-sm">{t('dashboard.unconfigured_hint')}</p>
            <Button size="sm" onClick={() => { window.location.hash = '/settings'; }}>{t('dashboard.go_settings')}</Button>
          </CardContent>
        </Card>
      )}
      <div className="grid grid-cols-2 gap-3 lg:grid-cols-4">
        <Metric icon={Radio} label={t('dashboard.bot')} value={<StatusBadge status={status} />} />
        <Metric icon={ArrowDownLeft} label={t('dashboard.received')} value={formatNumber(status.received, locale)} />
        <Metric icon={ArrowUpRight} label={t('dashboard.sent')} value={formatNumber(status.sent, locale)} />
        <Metric icon={Clock} label={t('dashboard.uptime')} value={<span className="text-lg">{duration(status.started_at)}</span>} />
      </div>
      <div className="grid gap-6 xl:grid-cols-2">
        <Card>
          <CardHeader><CardTitle>{t('dashboard.bot')}</CardTitle></CardHeader>
          <CardContent>
            <div className="mb-3 flex items-center gap-3">
              {status.avatar ? <img src={status.avatar} alt="" className="h-12 w-12 rounded-full border" />
                : <div className="h-12 w-12 rounded-full bg-primary/15" />}
              <div className="min-w-0">
                <p className="truncate text-base font-semibold">{status.nickname || '—'}</p>
                <StatusBadge status={status} />
              </div>
            </div>
            <Row label={t('dashboard.self_id')}><span className="font-mono">{status.self_id || '—'}</span></Row>
            <Row label={t('dashboard.app_id')}><span className="font-mono">{status.app_id || '—'}</span></Row>
            <Row label={t('dashboard.mode')}>{t(status.mode === 'webhook' ? 'dashboard.mode_webhook' : 'dashboard.mode_gateway')}</Row>
            {status.webhook && <Row label={t('dashboard.webhook')}><span className="font-mono">{status.webhook.host}:{status.webhook.port}{status.webhook.path}</span></Row>}
            <Row label={t('dashboard.users')}>{formatNumber(status.counts.users, locale)}</Row>
            <Row label={t('dashboard.groups')}>{formatNumber(status.counts.groups, locale)}</Row>
            <Row label={t('dashboard.bound')}>{formatNumber(status.counts.bound, locale)}</Row>
            <Row label={t('dashboard.indexed')}>{formatNumber(status.counts.messages, locale)}</Row>
            <Row label={t('dashboard.version')}>{status.version}</Row>
          </CardContent>
        </Card>
        <Card>
          <CardHeader><CardTitle>{t('dashboard.connections')}</CardTitle></CardHeader>
          <CardContent className="space-y-4">
            {noConnections && <p className="text-sm text-muted-foreground">{t('dashboard.no_connections')}</p>}
            {c.listeners.map((l) => (
              <div key={`${l.type}-${l.host}-${l.port}`} className="flex items-start gap-3">
                {l.type === 'http' ? <Server className="mt-0.5 h-4 w-4 text-muted-foreground" /> : <Plug className="mt-0.5 h-4 w-4 text-muted-foreground" />}
                <div className="min-w-0 flex-1">
                  <p className="text-sm font-medium">{t(l.type === 'http' ? 'dashboard.http_api' : 'dashboard.ws_server')}</p>
                  <p className="font-mono text-xs text-muted-foreground">{l.host}:{l.port}</p>
                </div>
                {l.type === 'ws' && <Badge variant={c.ws_clients > 0 ? 'success' : 'neutral'}>{t('dashboard.ws_clients', { count: c.ws_clients })}</Badge>}
              </div>
            ))}
            {c.http_post.map((p) => (
              <div key={p.url} className="flex items-start gap-3">
                <Send className="mt-0.5 h-4 w-4 text-muted-foreground" />
                <div className="min-w-0 flex-1">
                  <p className="text-sm font-medium">{t('dashboard.http_post')}</p>
                  <p className="break-all font-mono text-xs text-muted-foreground">{p.url}</p>
                  <p className="mt-1 text-xs text-muted-foreground">
                    {!p.last ? t('dashboard.no_report')
                      : p.last.ok ? `${t('dashboard.last_ok', { status: p.last.status })} · ${formatTime(p.last.at, locale)}`
                      : `${t('dashboard.last_failed', { error: p.last.error || `HTTP ${p.last.status}` })} · ${formatTime(p.last.at, locale)}`}
                  </p>
                </div>
                {p.last && <Badge variant={p.last.ok ? 'success' : 'danger'}>{p.last.ok ? 'OK' : '!'}</Badge>}
              </div>
            ))}
            {c.ws_reverse.map((r) => (
              <div key={r.url} className="flex items-start gap-3">
                <Globe className="mt-0.5 h-4 w-4 text-muted-foreground" />
                <div className="min-w-0 flex-1">
                  <p className="text-sm font-medium">{t('dashboard.ws_reverse')}</p>
                  <p className="break-all font-mono text-xs text-muted-foreground">{r.url}</p>
                  {!r.connected && r.error && <p className="mt-1 text-xs text-muted-foreground">{r.error}</p>}
                </div>
                <Badge variant={r.connected ? 'success' : 'neutral'}>{t(r.connected ? 'dashboard.connected' : 'dashboard.disconnected')}</Badge>
              </div>
            ))}
          </CardContent>
        </Card>
      </div>
    </>
  );
}
