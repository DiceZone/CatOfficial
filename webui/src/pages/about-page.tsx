import React from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { Check, Copy, Download, ExternalLink, RefreshCw, Save, ShieldCheck, X } from 'lucide-react';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';
import { FeatureHelp } from '@/components/ui/feature-help';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { PageHeader } from '@/components/page-header';
import { Brand } from '@/components/layout/brand';
import { useConfirm } from '@/components/confirm-dialog';
import { errorText } from '@/components/form';
import { api, type UpdateAction, type UpdateSettings, type UpdateSource, type UpdateStatus } from '@/lib/api';

const BUSY_PHASES = ['checking', 'connecting', 'downloading', 'verifying', 'preparing', 'cancelling', 'installing'];
export const isUpdateBusy = (phase: string) => BUSY_PHASES.includes(phase);

/// 与 Dice!Next 相同的版本写法：beta-0.1.0(3)。
export function formatVersion(info?: { version: string; build: number; prerelease?: boolean } | null) {
  if (!info?.version) return '—';
  return `${info.prerelease ?? info.build > 0 ? 'beta-' : 'v'}${info.version}(${info.build})`;
}

function formatBytes(bytes: number) {
  if (!bytes) return '0 B';
  if (bytes >= 1 << 20) return `${(bytes / (1 << 20)).toFixed(1)} MB`;
  return `${Math.ceil(bytes / 1024)} KB`;
}

function timezoneLabel(offsetMinutes: number) {
  const minutes = Math.abs(offsetMinutes);
  return `UTC${offsetMinutes >= 0 ? '+' : '-'}${String(Math.floor(minutes / 60)).padStart(2, '0')}:${String(minutes % 60).padStart(2, '0')}`;
}

// 安装时间按服务器时区显示，不用浏览器的时区。
function formatScheduledAt(seconds: number, offsetMinutes: number) {
  if (!seconds) return '—';
  const date = new Date((seconds + offsetMinutes * 60) * 1000);
  return `${date.toISOString().slice(0, 16).replace('T', ' ')} (${timezoneLabel(offsetMinutes)})`;
}

const CREDITS = [
  ['dice_next', 'Dice!Next', 'https://github.com/DiceZone/Dice-Next'],
  ['mcsm', 'MCSManager', 'https://github.com/MCSManager/MCSManager'],
  ['onebot', 'OneBot V11', 'https://github.com/botuniverse/onebot-11'],
  ['napcat', 'NapCat', 'https://github.com/NapNeko/NapCatQQ'],
  ['qface', 'QFace', 'https://github.com/koishijs/QFace'],
] as const;

export function AboutPage() {
  const { t } = useTranslation();
  const { data: status } = useQuery({ queryKey: ['status'], queryFn: api.status });
  return (
    <>
      <PageHeader title={t('about.title')} />
      <div className="grid items-start gap-6 lg:grid-cols-[minmax(0,3fr)_minmax(0,2fr)]">
        <UpdateCard />
        <div className="min-w-0 space-y-6">
          <Card>
            <CardContent className="space-y-4">
              <Brand />
              <p className="text-sm">{t('about.description')}</p>
              <p className="text-sm text-muted-foreground">{t('about.version')}：{status?.version ?? '—'}</p>
              <p className="text-sm text-muted-foreground">{t('about.license')}</p>
              <div className="flex flex-wrap gap-4 text-sm">
                <a className="inline-flex items-center gap-1 text-primary hover:underline" href="https://github.com/DiceZone/CatOfficial" target="_blank" rel="noreferrer">
                  {t('about.repo')}<ExternalLink className="h-3.5 w-3.5" />
                </a>
                <a className="inline-flex items-center gap-1 text-primary hover:underline" href="https://github.com/botuniverse/onebot-11" target="_blank" rel="noreferrer">
                  {t('about.onebot')}<ExternalLink className="h-3.5 w-3.5" />
                </a>
              </div>
            </CardContent>
          </Card>
          <Card>
            <CardHeader><CardTitle>{t('about.credits.title')}</CardTitle></CardHeader>
            <CardContent>
              <dl className="space-y-3 text-sm">
                {CREDITS.map(([key, name, url]) => (
                  <div key={key}>
                    <dt className="font-medium">
                      <a className="inline-flex items-center gap-1 text-primary hover:underline" href={url} target="_blank" rel="noreferrer">
                        {name}<ExternalLink className="h-3.5 w-3.5" />
                      </a>
                    </dt>
                    <dd className="text-muted-foreground">{t(`about.credits.${key}`)}</dd>
                  </div>
                ))}
              </dl>
            </CardContent>
          </Card>
        </div>
      </div>
    </>
  );
}

/// 在线更新卡片（照搬 Dice!Next「关于」页的更新卡片）。
function UpdateCard() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const { confirm, node: confirmNode } = useConfirm();
  const query = useQuery({ queryKey: ['update'], queryFn: api.updateStatus, refetchInterval: 3000 });
  const status = query.data;
  const [draft, setDraft] = React.useState<UpdateSettings | null>(null);
  const [dirty, setDirty] = React.useState(false);
  const [working, setWorking] = React.useState('');
  const [operationError, setOperationError] = React.useState('');
  const [dismissed, setDismissed] = React.useState('');
  const [copied, setCopied] = React.useState(false);
  const [saved, setSaved] = React.useState(false);

  React.useEffect(() => { if (status && !dirty) setDraft(status.settings); }, [status, dirty]);

  const accept = (next: UpdateStatus) => client.setQueryData(['update'], next);
  const visibleError = (operationError || status?.error || '') !== dismissed ? (operationError || status?.error || '') : '';
  React.useEffect(() => { setCopied(false); }, [visibleError]);

  const run = async (action: 'check' | 'download' | 'cancel' | 'install') => {
    setWorking(action);
    setOperationError('');
    setDismissed('');
    setSaved(false);
    try { accept(await api.updateAction(action)); }
    catch (e) { setOperationError(errorText(e)); void query.refetch(); }
    finally { setWorking(''); }
  };
  const install = async () => {
    const ok = await confirm({ title: t('about.update_install_confirm_title'), description: t('about.update_install_confirm_desc'), confirmLabel: t('about.update_install'), destructive: true });
    if (ok) await run('install');
  };
  const save = async () => {
    if (!draft) return;
    setWorking('save');
    setOperationError('');
    setSaved(false);
    try {
      const next = await api.saveUpdateSettings(draft);
      accept(next);
      setDraft(next.settings);
      setDirty(false);
      setSaved(true);
    } catch (e) { setOperationError(errorText(e)); }
    finally { setWorking(''); }
  };
  const setting = <K extends keyof UpdateSettings>(key: K, value: UpdateSettings[K]) => {
    setDraft((current) => (current ? { ...current, [key]: value } : current));
    setDirty(true);
    setSaved(false);
  };
  const copyError = async () => {
    try { await navigator.clipboard.writeText(visibleError); setCopied(true); } catch { setCopied(false); }
  };

  const phase = status?.phase ?? 'idle';
  const busy = isUpdateBusy(phase);
  const scheduleDisabled = !draft?.autoCheck || draft.autoAction !== 'install' || !status?.scheduledInstallSupported || !!working;
  const phaseVariant = phase === 'error' ? 'danger' : phase === 'up_to_date' ? 'success' : busy ? 'info' : status?.updateAvailable ? 'warning' : 'neutral';

  return (
    <Card>
      {confirmNode}
      <CardHeader className="space-y-1">
        <div className="flex items-center justify-between gap-3">
          <CardTitle className="flex items-center gap-2 text-base">
            <RefreshCw className="h-4 w-4" />
            {t('about.update_title')}
            <FeatureHelp title={t('about.update_title')} description={<p>{t('about.update_desc')}</p>} />
          </CardTitle>
          <Badge variant={phaseVariant}>{t(`about.phase_${phase}`, { defaultValue: phase })}</Badge>
        </div>
      </CardHeader>
      <CardContent className="space-y-5">
        <div className="grid gap-2 text-sm">
          <Row label={t('about.current_version')}><span className="font-mono">{formatVersion(status?.current)}</span></Row>
          <Row label={t('about.latest_version')}>
            {status?.latest
              ? <a className="inline-flex items-center gap-1 font-mono text-primary hover:underline" href={status.latest.releaseUrl} target="_blank" rel="noreferrer">{status.latest.tag}<ExternalLink className="h-3 w-3" /></a>
              : <span className="font-mono">{t('about.update_not_checked')}</span>}
          </Row>
          <Row label={t('about.update_platform')}><span className="font-mono">{status ? `${status.platform.os}/${status.platform.arch}` : '—'}</span></Row>
          {status?.source && <Row label={t('about.update_source_used')}><span className="break-all text-right font-mono text-xs">{status.source}</span></Row>}
          {!!status?.checkedAt && <Row label={t('about.update_checked_at')}><span>{new Date(status.checkedAt * 1000).toLocaleString()}</span></Row>}
          {status?.latest?.asset && (
            <>
              <Row label={t('about.update_asset')}><span className="text-right font-mono text-xs">{status.latest.asset.name} ({formatBytes(status.latest.asset.size)})</span></Row>
              <Row label={t('about.update_integrity')}><span className="font-mono text-xs">{status.latest.asset.sha256.slice(0, 12)}…</span></Row>
            </>
          )}
          {['connecting', 'downloading', 'verifying', 'preparing', 'cancelling'].includes(phase) && status && (
            <div className="space-y-1 pt-1">
              <div className="flex justify-between text-xs text-muted-foreground">
                <span>{t(`about.phase_${phase}`)}</span>
                <span>{formatBytes(status.downloadedBytes)} / {formatBytes(status.totalBytes)}</span>
              </div>
              <div className="h-2 overflow-hidden rounded-full bg-muted">
                <div className="h-full bg-primary transition-all" style={{ width: `${Math.min(100, status.totalBytes > 0 ? (status.downloadedBytes / status.totalBytes) * 100 : 0)}%` }} />
              </div>
            </div>
          )}
        </div>

        {status?.updateAvailable && status.latest?.releaseNotes && (
          <div className="space-y-1 rounded-md border bg-muted/30 p-3 text-sm">
            <p className="font-medium">{t('about.release_notes')}</p>
            <p className="max-h-48 overflow-auto whitespace-pre-wrap text-muted-foreground">{status.latest.releaseNotes}</p>
          </div>
        )}

        {visibleError && (
          <div className="flex items-start gap-2 rounded-md border border-destructive/30 bg-destructive/10 p-3 text-sm text-destructive">
            <p className="min-w-0 flex-1 select-text whitespace-pre-wrap break-words">{t('about.update_error')}: {visibleError}</p>
            <Button type="button" variant="ghost" size="sm" className="h-7 shrink-0 gap-1 px-2 text-destructive hover:text-destructive" onClick={() => void copyError()} title={t('about.update_error_copy')}>
              {copied ? <Check className="h-3.5 w-3.5" /> : <Copy className="h-3.5 w-3.5" />}
              {t(copied ? 'about.update_error_copied' : 'about.update_error_copy')}
            </Button>
            <Button type="button" variant="ghost" size="icon" className="h-7 w-7 shrink-0 text-destructive hover:text-destructive"
              onClick={() => { setDismissed(visibleError); setOperationError(''); }} aria-label={t('about.update_error_dismiss')} title={t('about.update_error_dismiss')}>
              <X className="h-4 w-4" />
            </Button>
          </div>
        )}

        {status?.selfUpdateBlockedReason === 'container' ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 p-3 text-sm text-amber-800 dark:text-amber-200">
            {t('about.update_container_hint', { type: status.runtime?.containerType || 'container' })}
          </div>
        ) : status && !status.installSupported ? (
          <div className="rounded-md border bg-muted/40 p-3 text-sm text-muted-foreground">{t('about.update_manual_hint')}</div>
        ) : null}

        <div className="flex flex-wrap gap-2">
          <Button variant="outline" onClick={() => void run('check')} disabled={busy || !!working}>
            <RefreshCw className={`mr-2 h-4 w-4 ${phase === 'checking' ? 'animate-spin' : ''}`} />{t('about.update_check')}
          </Button>
          <Button variant="outline" onClick={() => void run('download')}
            disabled={status?.downloadSupported === false || !status?.updateAvailable || !status.latest?.asset || (status.pending && status.pendingTag === status.latest.tag) || busy || !!working}>
            <Download className="mr-2 h-4 w-4" />{t('about.update_download')}
          </Button>
          <Button onClick={() => void install()} disabled={!status?.installSupported || !status.pending || busy || !!working}>
            <ShieldCheck className="mr-2 h-4 w-4" />{t('about.update_install')}
          </Button>
          {status?.cancelSupported && (status.canCancel || phase === 'cancelling') && (
            <Button variant="outline" onClick={() => void run('cancel')} disabled={!status.canCancel || !!working}>
              <X className="mr-2 h-4 w-4" />{t(phase === 'cancelling' ? 'about.phase_cancelling' : 'about.update_cancel')}
            </Button>
          )}
        </div>
        {!!status?.scheduledInstallAt && (
          <div className="rounded-lg border border-primary/20 bg-primary/5 p-3 text-sm" role="status">
            {t('about.update_scheduled_pending', {
              tag: status.pendingTag || status.latest?.tag || '',
              time: formatScheduledAt(status.scheduledInstallAt, status.timezoneMinutes ?? 0),
            })}
          </div>
        )}

        <div className="space-y-4 border-t pt-5">
          <h3 className="text-sm font-medium">{t('about.update_settings')}</h3>
          <div className="flex items-center justify-between gap-4">
            <div className="flex items-center gap-1.5">
              <Label htmlFor="update-auto-check">{t('about.update_auto_check')}</Label>
              <FeatureHelp title={t('about.update_auto_check')} description={<p>{t('about.update_auto_check_desc')}</p>} />
            </div>
            <Switch id="update-auto-check" checked={draft?.autoCheck ?? false} disabled={!draft} onCheckedChange={(checked) => setting('autoCheck', checked)} />
          </div>

          <div className="grid gap-4 sm:grid-cols-2">
            <div className="flex flex-col gap-2">
              {/* 与 DN 相同：两栏的标签行都撑到 min-h-6，带说明按钮的一栏不会把下拉框往下推 */}
              <div className="flex min-h-6 flex-1 items-center">
                <Label htmlFor="update-interval">{t('about.update_interval')}</Label>
              </div>
              <Select value={String(draft?.intervalHours ?? 6)} disabled={!draft || !draft.autoCheck} onValueChange={(value) => setting('intervalHours', Number(value))}>
                <SelectTrigger id="update-interval"><SelectValue /></SelectTrigger>
                <SelectContent>
                  {[1, 3, 6, 12, 24, 48, 72, 168].map((hours) => <SelectItem key={hours} value={String(hours)}>{t('about.update_hours', { count: hours })}</SelectItem>)}
                </SelectContent>
              </Select>
            </div>

            <div className="flex flex-col gap-2">
              <div className="flex min-h-6 flex-1 items-center gap-1.5">
                <Label htmlFor="update-auto-action">{t('about.update_auto_action')}</Label>
                <FeatureHelp title={t('about.update_auto_action')} description={
                  <dl className="space-y-2">
                    {(['notify', 'download', 'install'] as const).map((action) => (
                      <div key={action}><dt className="font-medium">{t(`about.action_${action}`)}</dt><dd>{t(`about.update_action_${action}_desc`)}</dd></div>
                    ))}
                  </dl>
                } />
              </div>
              <Select value={draft?.autoAction ?? 'notify'} disabled={!draft || !draft.autoCheck} onValueChange={(value) => setting('autoAction', value as UpdateAction)}>
                <SelectTrigger id="update-auto-action"><SelectValue /></SelectTrigger>
                <SelectContent>
                  <SelectItem value="notify">{t('about.action_notify')}</SelectItem>
                  <SelectItem value="download" disabled={status?.downloadSupported === false}>{t('about.action_download')}</SelectItem>
                  <SelectItem value="install" disabled={!status?.installSupported}>{t('about.action_install')}</SelectItem>
                </SelectContent>
              </Select>
            </div>

            <div role="group" aria-disabled={scheduleDisabled} className={`space-y-3 rounded-lg border p-3 sm:col-span-2 ${scheduleDisabled ? 'bg-muted/40 opacity-50' : ''}`}>
              <div className="flex items-center justify-between gap-4">
                <div className="flex items-center gap-1.5">
                  <Label htmlFor="update-scheduled-install">{t('about.update_scheduled_install')}</Label>
                  <FeatureHelp title={t('about.update_scheduled_install')} description={<>
                    <p>{t('about.update_scheduled_install_desc')}</p>
                    {!status?.scheduledInstallSupported && <p>{t('about.update_schedule_unsupported')}</p>}
                  </>} />
                </div>
                <Switch id="update-scheduled-install" checked={draft?.scheduledInstall ?? false} disabled={scheduleDisabled}
                  onCheckedChange={(checked) => setting('scheduledInstall', checked)} />
              </div>
              {draft?.scheduledInstall && (
                <div className="max-w-sm space-y-2">
                  <Label htmlFor="update-install-time">{t('about.update_install_time')}</Label>
                  <Input id="update-install-time" type="time" value={draft.installTime ?? '04:00'} disabled={scheduleDisabled}
                    onChange={(event) => setting('installTime', event.target.value)} />
                  <p className="text-xs text-muted-foreground">{t('about.update_install_timezone', { timezone: timezoneLabel(status?.timezoneMinutes ?? 0) })}</p>
                </div>
              )}
            </div>

            <div className="space-y-2 sm:col-span-2">
              <div className="flex items-center gap-1.5">
                <Label>{t('about.update_source')}</Label>
                <FeatureHelp title={t('about.update_source')} description={<><p>{t('about.update_settings_desc')}</p><p>{t('about.update_source_desc')}</p></>} />
              </div>
              <Select value={draft?.source ?? 'auto'} disabled={!draft} onValueChange={(value) => setting('source', value as UpdateSource)}>
                <SelectTrigger><SelectValue /></SelectTrigger>
                <SelectContent>
                  <SelectItem value="auto">{t('about.source_auto')}</SelectItem>
                  <SelectItem value="direct">{t('about.source_direct')}</SelectItem>
                  <SelectItem value="mirror">{t('about.source_mirror')}</SelectItem>
                  <SelectItem value="custom">{t('about.source_custom')}</SelectItem>
                </SelectContent>
              </Select>
            </div>

            {draft?.source === 'custom' && (
              <div className="space-y-2 sm:col-span-2">
                <Label htmlFor="update-custom-mirror">{t('about.update_custom_mirror')}</Label>
                <Input id="update-custom-mirror" value={draft.customMirror} placeholder={t('about.custom_mirror_placeholder')}
                  onChange={(event) => setting('customMirror', event.target.value)} />
                <p className="text-xs text-muted-foreground">{t('about.custom_mirror_desc')}</p>
              </div>
            )}
          </div>

          <div className="flex items-center gap-3">
            <Button onClick={() => void save()} disabled={!dirty || !!working || !draft}>
              <Save className="mr-2 h-4 w-4" />{t('about.update_save')}
            </Button>
            {saved && <span className="text-sm text-green-600 dark:text-green-400">{t('about.update_saved')}</span>}
          </div>
        </div>
      </CardContent>
    </Card>
  );
}

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex items-center justify-between gap-3">
      <span className="text-muted-foreground">{label}</span>
      {children}
    </div>
  );
}
