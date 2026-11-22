/**
 * 程序托管（MCSM 式）：在 CatOfficial 所在的机器上托管别的程序（另一个骰子、NapCat、脚本……），
 * 可以启动、停止、重启、强制结束，看它的控制台、直接输入指令；可以随 CatOfficial 启动、退出后自动重启。
 *
 * 控制台和网页终端是同一套（照搬 BDC 的 NodeTerminal）：程序跑在伪终端里，输出走长轮询，按键单独发送。
 * 程序重启后输出接着往下写，之前的记录还在；输出同时写进 data/instance-logs，崩溃、重启 CatOfficial 后也不丢。
 *
 * 实例目录默认在 data/instances/<文件夹> 下（方便管理与搬迁）；用 --files-full-access 启动时才能放到别处。
 * 「导入压缩包」照搬 BDC 实例详情的「导入 data 包」：压缩包解到实例目录，原来的目录移进回收站。
 */
import { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { Download, Eraser, FolderOpen, Loader2, Pencil, Play, Plus, RotateCw, ScrollText, Settings2, Skull, Square, TerminalSquare, Trash2, Upload } from 'lucide-react';
import { Terminal } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';
import '@xterm/xterm/css/xterm.css';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Dialog, DialogContent, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import { Textarea } from '@/components/ui/textarea';
import { PageHeader } from '@/components/page-header';
import { ExposedWarning } from '@/components/exposed-warning';
import { Field, Toggle, digits } from '@/components/form';
import { useStyledDialog } from '@/components/styled-dialog';
import { apiClient, formatDateTime, useToast } from '@/lib/bdc-compat';
import { cn } from '@/lib/utils';
import { uploadWithProgress } from '@/lib/upload';
import { decodeChunk, encodeKeys } from '@/pages/terminal-page';

interface Program {
  id: string;
  name: string;
  command: string;
  cwd: string;
  env: Record<string, string>;
  auto_start: boolean;
  auto_restart: boolean;
  restart_delay_seconds: number;
  stop_command: string;
  stop_timeout_seconds: number;
  state: 'stopped' | 'running' | 'stopping' | 'restarting' | 'crashed';
  pid: number;
  started_at: string;
  exit_code?: number;
  error: string;
  resolved_cwd: string;
  /** 文件管理里打开它用的路径；空表示不在文件管理的范围里 */
  files_path: string;
  folder_exists: boolean;
}

interface LogSettings { log_retention_days: number; log_max_mb: number }
interface ProgramList { programs: Program[]; exposed: boolean; max_programs: number; full_access: boolean; files_enabled: boolean; settings: LogSettings }

type Draft = Omit<Program, 'state' | 'pid' | 'started_at' | 'exit_code' | 'error' | 'resolved_cwd' | 'env' | 'files_path' | 'folder_exists'> & { envText: string };

const EMPTY: Draft = {
  id: '', name: '', command: '', cwd: '', envText: '', auto_start: false, auto_restart: true,
  restart_delay_seconds: 5, stop_command: '', stop_timeout_seconds: 15,
};

const STATE_VARIANT: Record<Program['state'], 'success' | 'neutral' | 'info' | 'warning' | 'danger'> = {
  running: 'success', stopped: 'neutral', stopping: 'info', restarting: 'warning', crashed: 'danger',
};

export function ProgramsPage() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const toast = useToast();
  const dialog = useStyledDialog();
  const list = useQuery({
    queryKey: ['programs'],
    queryFn: () => apiClient.get<ProgramList>('/api/webui/programs'),
    refetchInterval: 3000,
  });
  const programs = list.data?.programs ?? [];
  const [selectedId, setSelectedId] = useState('');
  const selected = programs.find((p) => p.id === selectedId) ?? null;
  const [editing, setEditing] = useState<Draft | null>(null);
  const [busy, setBusy] = useState('');
  const [importing, setImporting] = useState<Program | null>(null);
  const [logsOf, setLogsOf] = useState<Program | null>(null);
  const [deleting, setDeleting] = useState<Program | null>(null);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const fullAccess = list.data?.full_access ?? false;

  useEffect(() => {
    if (!selectedId && programs.length) setSelectedId(programs[0].id);
    if (selectedId && list.data && !programs.some((p) => p.id === selectedId)) setSelectedId(programs[0]?.id ?? '');
  }, [programs, selectedId, list.data]);

  const act = async (program: Program, action: 'start' | 'stop' | 'kill' | 'restart' | 'clear' | 'delete', extra: Record<string, unknown> = {}) => {
    if (action === 'kill') {
      const ok = await dialog.confirm({ title: t('programs.kill_title', { name: program.name }), description: t('programs.kill_desc'), confirmText: t('programs.kill'), destructive: true });
      if (!ok) return;
    }
    setBusy(`${action}:${program.id}`);
    try {
      const resp = await apiClient.post<{ programs: Program[] }>('/api/webui/programs/action', { id: program.id, action, ...extra });
      client.setQueryData(['programs'], (old: ProgramList | undefined) => (old ? { ...old, programs: resp.programs } : old));
      if (action === 'delete') setDeleting(null);
    } catch (err: any) {
      toast.error(err?.body?.detail || t('programs.action_failed'));
    } finally {
      setBusy('');
    }
  };

  const save = async () => {
    if (!editing) return;
    const env: Record<string, string> = {};
    for (const line of editing.envText.split('\n')) {
      const index = line.indexOf('=');
      if (index > 0) env[line.slice(0, index).trim()] = line.slice(index + 1);
    }
    const { envText: _envText, ...rest } = editing;
    setBusy('save');
    try {
      const saved = await apiClient.post<Program>('/api/webui/programs', { ...rest, env });
      toast.success(t('common.saved'));
      setEditing(null);
      setSelectedId(saved.id);
      void client.invalidateQueries({ queryKey: ['programs'] });
    } catch (err: any) {
      toast.error(err?.body?.detail || t('programs.save_failed'));
    } finally {
      setBusy('');
    }
  };

  const edit = (program?: Program) => setEditing(program ? {
    id: program.id, name: program.name, command: program.command, cwd: program.cwd,
    envText: Object.entries(program.env ?? {}).map(([k, v]) => `${k}=${v}`).join('\n'),
    auto_start: program.auto_start, auto_restart: program.auto_restart, restart_delay_seconds: program.restart_delay_seconds,
    stop_command: program.stop_command, stop_timeout_seconds: program.stop_timeout_seconds,
  } : { ...EMPTY });

  return (
    <>
      <PageHeader title={t('programs.title')} description={t('programs.description')}
        actions={<>
          <Button size="sm" variant="outline" onClick={() => setSettingsOpen(true)}><Settings2 className="mr-2 h-4 w-4" />{t('programs.log_settings')}</Button>
          <Button size="sm" onClick={() => edit()}><Plus className="mr-2 h-4 w-4" />{t('programs.add')}</Button>
        </>} />
      {list.data?.exposed && <ExposedWarning />}

      <div className="grid gap-4 lg:grid-cols-[minmax(16rem,20rem)_1fr]">
        <div className="space-y-2">
          {programs.length === 0 && (
            <Card><CardContent className="py-8 text-center text-sm text-muted-foreground">{t('programs.empty')}</CardContent></Card>
          )}
          {programs.map((program) => (
            <button key={program.id} type="button" onClick={() => setSelectedId(program.id)}
              className={cn('w-full rounded-lg border bg-card p-3 text-left transition-colors hover:bg-muted/50', selectedId === program.id && 'border-primary bg-primary/5')}>
              <div className="flex items-center justify-between gap-2">
                <span className="truncate font-medium">{program.name}</span>
                <Badge variant={STATE_VARIANT[program.state]}>{t(`programs.state_${program.state}`)}</Badge>
              </div>
              <p className="mt-1 truncate font-mono text-xs text-muted-foreground" title={program.command}>{program.command}</p>
              <div className="mt-1 flex flex-wrap gap-x-3 text-xs text-muted-foreground">
                {program.state === 'running' && <span>PID {program.pid}</span>}
                {program.auto_start && <span>{t('programs.auto_start_short')}</span>}
                {program.auto_restart && <span>{t('programs.auto_restart_short')}</span>}
              </div>
            </button>
          ))}
        </div>

        <Card className="min-w-0 border-slate-300 dark:border-slate-900">
          <CardHeader>
            <div className="flex flex-wrap items-start justify-between gap-3">
              <div className="min-w-0">
                <CardTitle className="flex items-center gap-2"><TerminalSquare className="h-5 w-5" />{selected?.name ?? t('programs.console')}</CardTitle>
                <CardDescription className="break-all">
                  {selected
                    ? [selected.resolved_cwd, selected.state === 'running' && selected.started_at ? t('programs.started_at', { time: formatDateTime(selected.started_at) }) : '',
                       selected.exit_code !== undefined && selected.state !== 'running' ? t('programs.exit_code', { code: selected.exit_code }) : '', selected.error]
                      .filter(Boolean).join(' · ')
                    : t('programs.pick')}
                </CardDescription>
              </div>
              {selected && (
                <div className="flex flex-wrap gap-2">
                  {selected.state === 'running' || selected.state === 'stopping' ? (
                    <Button size="sm" variant="outline" disabled={!!busy} onClick={() => void act(selected, 'stop')}><Square className="mr-1 h-4 w-4" />{t('programs.stop')}</Button>
                  ) : (
                    <Button size="sm" disabled={!!busy} onClick={() => void act(selected, 'start')}><Play className="mr-1 h-4 w-4" />{t('programs.start')}</Button>
                  )}
                  <Button size="sm" variant="outline" disabled={!!busy} onClick={() => void act(selected, 'restart')}><RotateCw className="mr-1 h-4 w-4" />{t('programs.restart')}</Button>
                  {(selected.state === 'running' || selected.state === 'stopping') && (
                    <Button size="sm" variant="outline" className="text-destructive" disabled={!!busy} onClick={() => void act(selected, 'kill')}><Skull className="mr-1 h-4 w-4" />{t('programs.kill')}</Button>
                  )}
                  <Button size="sm" variant="outline" disabled={!!busy || ['running', 'stopping', 'restarting'].includes(selected.state)}
                    onClick={() => setImporting(selected)}><Upload className="mr-1 h-4 w-4" />{t('programs.import')}</Button>
                  {list.data?.files_enabled && selected.files_path && (
                    <Button size="sm" variant="outline" title={t('programs.open_in_files')}
                      onClick={() => { window.location.hash = `/files?${new URLSearchParams({ path: selected.files_path })}`; }}><FolderOpen className="h-4 w-4" /></Button>
                  )}
                  <Button size="sm" variant="outline" onClick={() => setLogsOf(selected)} title={t('programs.logs')}><ScrollText className="h-4 w-4" /></Button>
                  <Button size="sm" variant="outline" disabled={!!busy} onClick={() => void act(selected, 'clear')} title={t('programs.clear')}><Eraser className="h-4 w-4" /></Button>
                  <Button size="sm" variant="outline" disabled={!!busy} onClick={() => edit(selected)} title={t('common.edit')}><Pencil className="h-4 w-4" /></Button>
                  <Button size="sm" variant="outline" className="text-destructive" disabled={!!busy || selected.state === 'running' || selected.state === 'stopping'}
                    onClick={() => setDeleting(selected)} title={t('common.delete')}><Trash2 className="h-4 w-4" /></Button>
                </div>
              )}
            </div>
          </CardHeader>
          <CardContent>
            <ProgramConsole programId={selected?.id ?? ''} running={selected?.state === 'running'} />
          </CardContent>
        </Card>
      </div>

      <Dialog open={!!editing} onOpenChange={(open) => { if (!open) setEditing(null); }}>
        <DialogContent className="max-w-2xl">
          <DialogHeader><DialogTitle>{editing?.id ? t('programs.edit_title') : t('programs.add')}</DialogTitle></DialogHeader>
          {editing && (
            <div className="grid gap-4 sm:grid-cols-2">
              <Field label={t('programs.name')} id="program-name">
                <Input id="program-name" value={editing.name} onChange={(e) => setEditing({ ...editing, name: e.target.value })} />
              </Field>
              <Field label={t('programs.cwd')} id="program-cwd" hint={t(fullAccess ? 'programs.cwd_hint_full' : 'programs.cwd_hint')}>
                <div className="flex min-w-0 items-center rounded-md border border-input bg-card focus-within:ring-2 focus-within:ring-ring">
                  {/* 填的是文件夹名称时，它在 data/instances 下；开放整台机器时也可以填绝对路径 */}
                  {!/^([A-Za-z]:[\\/]|\/)/.test(editing.cwd) && <span className="shrink-0 pl-3 font-mono text-xs text-muted-foreground">data/instances/</span>}
                  <Input id="program-cwd" className="min-w-0 border-0 font-mono focus-visible:ring-0" value={editing.cwd} placeholder={t('programs.cwd_placeholder')}
                    onChange={(e) => setEditing({ ...editing, cwd: e.target.value })} />
                </div>
              </Field>
              <div className="sm:col-span-2">
                <Field label={t('programs.command')} id="program-command" hint={t('programs.command_hint')}>
                  <Input id="program-command" className="font-mono" value={editing.command} placeholder={t('programs.command_placeholder')}
                    onChange={(e) => setEditing({ ...editing, command: e.target.value })} />
                </Field>
              </div>
              <Field label={t('programs.stop_command')} id="program-stop" hint={t('programs.stop_command_hint')}>
                <Input id="program-stop" className="font-mono" value={editing.stop_command} placeholder="stop" onChange={(e) => setEditing({ ...editing, stop_command: e.target.value })} />
              </Field>
              <Field label={t('programs.stop_timeout')} id="program-timeout" hint={t('programs.stop_timeout_hint')}>
                <Input id="program-timeout" inputMode="numeric" value={editing.stop_timeout_seconds} onChange={(e) => setEditing({ ...editing, stop_timeout_seconds: digits(e.target.value) || 15 })} />
              </Field>
              <div className="sm:col-span-2">
                <Field label={t('programs.env')} id="program-env" hint={t('programs.env_hint')}>
                  <Textarea id="program-env" rows={3} className="font-mono text-xs" value={editing.envText} placeholder="JAVA_HOME=C:\Java" onChange={(e) => setEditing({ ...editing, envText: e.target.value })} />
                </Field>
              </div>
              <div className="space-y-4 sm:col-span-2">
                <Toggle label={t('programs.auto_start')} hint={t('programs.auto_start_hint')} checked={editing.auto_start} onChange={(v) => setEditing({ ...editing, auto_start: v })} />
                <Toggle label={t('programs.auto_restart')} hint={t('programs.auto_restart_hint')} checked={editing.auto_restart} onChange={(v) => setEditing({ ...editing, auto_restart: v })} />
                {editing.auto_restart && (
                  <Field label={t('programs.restart_delay')} id="program-delay">
                    <Input id="program-delay" className="max-w-40" inputMode="numeric" value={editing.restart_delay_seconds} onChange={(e) => setEditing({ ...editing, restart_delay_seconds: digits(e.target.value) || 5 })} />
                  </Field>
                )}
              </div>
            </div>
          )}
          <DialogFooter>
            <Button variant="outline" onClick={() => setEditing(null)}>{t('common.cancel')}</Button>
            <Button disabled={busy === 'save'} onClick={() => void save()}>{t('common.save')}</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
      {importing && <ImportDialog program={importing} onClose={() => setImporting(null)}
        onDone={(programs) => { client.setQueryData(['programs'], (old: ProgramList | undefined) => (old ? { ...old, programs } : old)); setImporting(null); }} />}
      {logsOf && <LogsDialog program={logsOf} onClose={() => setLogsOf(null)} />}
      {settingsOpen && list.data && <LogSettingsDialog initial={list.data.settings} onClose={() => setSettingsOpen(false)}
        onSaved={(settings) => { client.setQueryData(['programs'], (old: ProgramList | undefined) => (old ? { ...old, settings } : old)); setSettingsOpen(false); }} />}
      <DeleteDialog program={deleting} busy={busy.startsWith('delete:')} onClose={() => setDeleting(null)}
        onConfirm={(removeFiles) => deleting && void act(deleting, 'delete', { remove_files: removeFiles })} />
    </>
  );
}


/// 程序的控制台：和网页终端同一套写法（xterm + 长轮询 + 按键直发）。
function ProgramConsole({ programId, running }: { programId: string; running: boolean }) {
  const { t } = useTranslation();
  const holder = useRef<HTMLDivElement | null>(null);
  const term = useRef<Terminal | null>(null);
  const fit = useRef<FitAddon | null>(null);
  const cursor = useRef(0);
  const reading = useRef('');
  const idRef = useRef(programId);
  idRef.current = programId;

  useEffect(() => {
    if (!holder.current || term.current) return;
    const instance = new Terminal({
      fontSize: 13,
      fontFamily: "'SF Mono','Fira Code',Menlo,Consolas,'Microsoft YaHei',monospace",
      cursorBlink: true,
      scrollback: 5000,
      theme: { background: '#0f0f12', foreground: '#e4e4e7' },
    });
    const fitAddon = new FitAddon();
    instance.loadAddon(fitAddon);
    instance.open(holder.current);
    term.current = instance;
    fit.current = fitAddon;
    let timer = 0;
    const relayout = () => {
      window.clearTimeout(timer);
      timer = window.setTimeout(() => {
        const box = holder.current;
        if (!box) return;
        const top = box.getBoundingClientRect().top;
        const wanted = Math.max(320, Math.round(window.innerHeight - top - 44));
        if (Math.abs(box.clientHeight - wanted) > 2) box.style.height = `${wanted}px`;
        try { fitAddon.fit(); } catch { return; }
        const dims = fitAddon.proposeDimensions();
        if (dims && idRef.current) {
          void apiClient.post('/api/webui/programs/resize', { id: idRef.current, cols: dims.cols, rows: dims.rows }).catch(() => {});
        }
      }, 120);
    };
    window.addEventListener('resize', relayout);
    relayout();
    return () => {
      window.clearTimeout(timer);
      window.removeEventListener('resize', relayout);
      instance.dispose();
      term.current = null;
    };
  }, []);

  // 按键直接送给程序（只有在运行时才收）
  useEffect(() => {
    const instance = term.current;
    if (!instance || !programId) return;
    const sub = instance.onData((data) => {
      void apiClient.post('/api/webui/programs/input', { id: programId, data: encodeKeys(data) }).catch(() => {});
    });
    return () => sub.dispose();
  }, [programId]);

  // 切换程序：清屏从头读（后端保留最近 256 KB 输出）
  useEffect(() => {
    if (!programId) return;
    term.current?.reset();
    cursor.current = 0;
    reading.current = programId;
    let stopped = false;
    const dims = fit.current?.proposeDimensions();
    if (dims) void apiClient.post('/api/webui/programs/resize', { id: programId, cols: dims.cols, rows: dims.rows }).catch(() => {});
    void (async () => {
      while (!stopped && reading.current === programId) {
        try {
          const resp = await apiClient.post<{ data: string; cursor: number }>('/api/webui/programs/read', { id: programId, cursor: cursor.current });
          if (stopped || reading.current !== programId) break;
          if (resp.data) term.current?.write(decodeChunk(resp.data));
          cursor.current = resp.cursor ?? cursor.current;
        } catch {
          await new Promise((done) => setTimeout(done, 1000));
        }
      }
    })();
    return () => { stopped = true; };
  }, [programId]);

  return (
    <div className="rounded-lg border border-slate-300 bg-slate-100 p-3 dark:border-slate-900 dark:bg-slate-800">
      <div ref={holder} className="min-h-[320px] w-full overflow-hidden rounded-md bg-[#0f0f12] p-2" />
      {programId && !running && <p className="mt-2 text-xs text-muted-foreground">{t('programs.console_hint')}</p>}
    </div>
  );
}


/// 导入压缩包（照搬 BDC 实例详情的「导入 data 包」对话框）。
function ImportDialog({ program, onClose, onDone }: { program: Program; onClose: () => void; onDone: (programs: Program[]) => void }) {
  const { t } = useTranslation();
  const toast = useToast();
  const input = useRef<HTMLInputElement | null>(null);
  const [file, setFile] = useState<File | null>(null);
  const [encoding, setEncoding] = useState('utf8');
  const [progress, setProgress] = useState<number | null>(null);
  const [importing, setImporting] = useState('');

  const start = async () => {
    if (!file) return;
    setImporting(t('programs.import_uploading'));
    setProgress(0);
    try {
      const resp = await uploadWithProgress({
        url: `./api/webui/programs/import?${new URLSearchParams({ id: program.id, name: file.name, encoding })}`,
        body: file,
        onProgress: (state) => {
          setProgress(state.percent);
          if (state.percent >= 100) setImporting(t('programs.import_extracting'));
        },
      });
      const result = resp.json || {};
      if (resp.status >= 400 || result.error) throw new Error(result.error || `HTTP ${resp.status}`);
      toast.success(t('programs.import_done'));
      onDone(result.programs || []);
    } catch (err: any) {
      toast.error(t('programs.import_failed', { error: err?.message || String(err) }));
      setImporting('');
      setProgress(null);
    }
  };

  return (
    <Dialog open onOpenChange={(open) => { if (!open && !importing) onClose(); }}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>{t('programs.import_title', { name: program.name })}</DialogTitle>
        </DialogHeader>
        <p className="text-sm text-muted-foreground">{t('programs.import_desc')}</p>
        <div className="space-y-2">
          <input ref={input} type="file" className="hidden" accept=".zip,.tar.gz,.tgz" onChange={(event) => setFile(event.target.files?.[0] ?? null)} />
          <div className="flex items-center gap-2">
            <Button type="button" variant="outline" size="sm" disabled={Boolean(importing)} onClick={() => input.current?.click()}>
              <Upload className="mr-1 h-4 w-4" />{t('programs.import_pick')}
            </Button>
            <span className="min-w-0 truncate text-sm text-muted-foreground">
              {file ? `${file.name}（${(file.size / 1024 / 1024).toFixed(1)} MB）` : t('programs.import_none')}
            </span>
          </div>
        </div>
        {file?.name.toLowerCase().endsWith('.zip') && (
          <div className="space-y-2">
            <Label>{t('programs.import_encoding')}</Label>
            <Select value={encoding} onValueChange={setEncoding} disabled={Boolean(importing)}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="utf8">{t('programs.encoding_utf8')}</SelectItem>
                <SelectItem value="gbk">{t('programs.encoding_gbk')}</SelectItem>
              </SelectContent>
            </Select>
          </div>
        )}
        {progress !== null && (
          <div className="space-y-1">
            <div className="flex justify-between text-xs text-muted-foreground"><span>{importing}</span><span>{progress}%</span></div>
            <div className="h-2 w-full overflow-hidden rounded-full bg-muted">
              <div className="h-2 rounded-full bg-primary transition-all" style={{ width: `${progress}%` }} />
            </div>
          </div>
        )}
        <DialogFooter>
          <Button variant="outline" disabled={Boolean(importing)} onClick={onClose}>{t('common.cancel')}</Button>
          <Button disabled={!file || Boolean(importing)} onClick={() => void start()}>
            {importing && <Loader2 className="mr-2 h-4 w-4 animate-spin" />}{t('programs.import_start')}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}

/// 历史日志：每天一份，可以下载。崩溃、重启 CatOfficial 之后也在。
function LogsDialog({ program, onClose }: { program: Program; onClose: () => void }) {
  const { t } = useTranslation();
  const logs = useQuery({
    queryKey: ['program-logs', program.id],
    queryFn: () => apiClient.get<{ logs: { name: string; size: number; modified: number }[]; settings: LogSettings }>(`/api/webui/programs/logs?id=${encodeURIComponent(program.id)}`),
  });
  const size = (bytes: number) => (bytes >= 1 << 20 ? `${(bytes / (1 << 20)).toFixed(1)} MB` : `${Math.max(1, Math.ceil(bytes / 1024))} KB`);
  return (
    <Dialog open onOpenChange={(open) => { if (!open) onClose(); }}>
      <DialogContent className="max-w-2xl">
        <DialogHeader><DialogTitle>{t('programs.logs_title', { name: program.name })}</DialogTitle></DialogHeader>
        {logs.data && <p className="text-sm text-muted-foreground">{t('programs.logs_desc', { days: logs.data.settings.log_retention_days, mb: logs.data.settings.log_max_mb })}</p>}
        <div className="max-h-[60vh] overflow-auto rounded-md border">
          <Table>
            <TableHeader>
              <TableRow><TableHead>{t('programs.log_file')}</TableHead><TableHead>{t('programs.log_size')}</TableHead><TableHead>{t('programs.log_modified')}</TableHead><TableHead className="w-12" /></TableRow>
            </TableHeader>
            <TableBody>
              {(logs.data?.logs ?? []).length === 0 && (
                <TableRow><TableCell colSpan={4} className="py-8 text-center text-muted-foreground">{logs.isLoading ? t('common.loading') : t('programs.logs_empty')}</TableCell></TableRow>
              )}
              {(logs.data?.logs ?? []).map((log) => (
                <TableRow key={log.name}>
                  <TableCell className="font-mono text-xs">{log.name}</TableCell>
                  <TableCell className="text-xs">{size(log.size)}</TableCell>
                  <TableCell className="text-xs">{formatDateTime(log.modified * 1000)}</TableCell>
                  <TableCell>
                    <a className="inline-flex text-primary hover:underline" title={t('programs.log_download')}
                      href={`./api/webui/programs/log?${new URLSearchParams({ id: program.id, name: log.name })}`} download={`${program.name}-${log.name}`}>
                      <Download className="h-4 w-4" />
                    </a>
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </div>
      </DialogContent>
    </Dialog>
  );
}

/// 日志设置（所有实例共用）。
function LogSettingsDialog({ initial, onClose, onSaved }: { initial: LogSettings; onClose: () => void; onSaved: (settings: LogSettings) => void }) {
  const { t } = useTranslation();
  const toast = useToast();
  const [days, setDays] = useState(initial.log_retention_days);
  const [mb, setMb] = useState(initial.log_max_mb);
  const [busy, setBusy] = useState(false);
  const save = async () => {
    setBusy(true);
    try {
      onSaved(await apiClient.post<LogSettings>('/api/webui/programs/settings', { log_retention_days: days, log_max_mb: mb }));
      toast.success(t('common.saved'));
    } catch (err: any) {
      toast.error(err?.body?.detail || t('programs.save_failed'));
    } finally {
      setBusy(false);
    }
  };
  return (
    <Dialog open onOpenChange={(open) => { if (!open) onClose(); }}>
      <DialogContent className="max-w-md">
        <DialogHeader><DialogTitle>{t('programs.log_settings')}</DialogTitle></DialogHeader>
        <div className="space-y-4">
          <Field label={t('programs.log_retention')} id="log-days" hint={t('programs.log_retention_hint')}>
            <Input id="log-days" inputMode="numeric" value={days} onChange={(e) => setDays(digits(e.target.value))} />
          </Field>
          <Field label={t('programs.log_max')} id="log-mb" hint={t('programs.log_max_hint')}>
            <Input id="log-mb" inputMode="numeric" value={mb} onChange={(e) => setMb(digits(e.target.value))} />
          </Field>
        </div>
        <DialogFooter>
          <Button variant="outline" onClick={onClose}>{t('common.cancel')}</Button>
          <Button disabled={busy} onClick={() => void save()}>{t('common.save')}</Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}

/// 删除实例：默认只删托管设置；可选连实例目录与日志一起移进回收站（保留 7 天）。
function DeleteDialog({ program, busy, onClose, onConfirm }: { program: Program | null; busy: boolean; onClose: () => void; onConfirm: (removeFiles: boolean) => void }) {
  const { t } = useTranslation();
  const [removeFiles, setRemoveFiles] = useState(false);
  useEffect(() => { setRemoveFiles(false); }, [program?.id]);
  return (
    <Dialog open={!!program} onOpenChange={(open) => { if (!open) onClose(); }}>
      <DialogContent className="max-w-md">
        <DialogHeader><DialogTitle>{t('programs.delete_title', { name: program?.name ?? '' })}</DialogTitle></DialogHeader>
        <p className="text-sm text-muted-foreground">{t('programs.delete_desc')}</p>
        <label className="flex items-start gap-3 text-sm">
          <Switch checked={removeFiles} onCheckedChange={setRemoveFiles} />
          <span>{t('programs.delete_files')}<span className="mt-1 block break-all font-mono text-xs text-muted-foreground">{program?.resolved_cwd}</span></span>
        </label>
        <DialogFooter>
          <Button variant="outline" onClick={onClose}>{t('common.cancel')}</Button>
          <Button variant="destructive" disabled={busy} onClick={() => onConfirm(removeFiles)}>{t('common.delete')}</Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
