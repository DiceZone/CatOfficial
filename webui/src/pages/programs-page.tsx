/**
 * 程序托管（MCSM 式）：在 CatOfficial 所在的机器上托管别的程序（另一个骰子、NapCat、脚本……），
 * 可以启动、停止、重启、强制结束，看它的控制台、直接输入指令；可以随 CatOfficial 启动、退出后自动重启。
 *
 * 控制台和网页终端是同一套（照搬 BDC 的 NodeTerminal）：程序跑在伪终端里，输出走长轮询，按键单独发送。
 * 程序重启后输出接着往下写，之前的记录还在。
 */
import { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { Eraser, Pencil, Play, Plus, RotateCw, Skull, Square, TerminalSquare, Trash2 } from 'lucide-react';
import { Terminal } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';
import '@xterm/xterm/css/xterm.css';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Dialog, DialogContent, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import { Input } from '@/components/ui/input';
import { Textarea } from '@/components/ui/textarea';
import { PageHeader } from '@/components/page-header';
import { ExposedWarning } from '@/components/exposed-warning';
import { Field, Toggle, digits } from '@/components/form';
import { useStyledDialog } from '@/components/styled-dialog';
import { apiClient, formatDateTime, useToast } from '@/lib/bdc-compat';
import { cn } from '@/lib/utils';
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
}

type Draft = Omit<Program, 'state' | 'pid' | 'started_at' | 'exit_code' | 'error' | 'resolved_cwd' | 'env'> & { envText: string };

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
    queryFn: () => apiClient.get<{ programs: Program[]; exposed: boolean }>('/api/webui/programs'),
    refetchInterval: 3000,
  });
  const programs = list.data?.programs ?? [];
  const [selectedId, setSelectedId] = useState('');
  const selected = programs.find((p) => p.id === selectedId) ?? null;
  const [editing, setEditing] = useState<Draft | null>(null);
  const [busy, setBusy] = useState('');

  useEffect(() => {
    if (!selectedId && programs.length) setSelectedId(programs[0].id);
    if (selectedId && list.data && !programs.some((p) => p.id === selectedId)) setSelectedId(programs[0]?.id ?? '');
  }, [programs, selectedId, list.data]);

  const act = async (program: Program, action: 'start' | 'stop' | 'kill' | 'restart' | 'clear' | 'delete') => {
    if (action === 'delete') {
      const ok = await dialog.confirm({ title: t('programs.delete_title', { name: program.name }), description: t('programs.delete_desc'), confirmText: t('common.delete'), destructive: true });
      if (!ok) return;
    }
    if (action === 'kill') {
      const ok = await dialog.confirm({ title: t('programs.kill_title', { name: program.name }), description: t('programs.kill_desc'), confirmText: t('programs.kill'), destructive: true });
      if (!ok) return;
    }
    setBusy(`${action}:${program.id}`);
    try {
      const resp = await apiClient.post<{ programs: Program[] }>('/api/webui/programs/action', { id: program.id, action });
      client.setQueryData(['programs'], (old: { programs: Program[]; exposed: boolean } | undefined) => ({ exposed: old?.exposed ?? false, programs: resp.programs }));
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
        actions={<Button size="sm" onClick={() => edit()}><Plus className="mr-2 h-4 w-4" />{t('programs.add')}</Button>} />
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
                  <Button size="sm" variant="outline" disabled={!!busy} onClick={() => void act(selected, 'clear')} title={t('programs.clear')}><Eraser className="h-4 w-4" /></Button>
                  <Button size="sm" variant="outline" disabled={!!busy} onClick={() => edit(selected)} title={t('common.edit')}><Pencil className="h-4 w-4" /></Button>
                  <Button size="sm" variant="outline" className="text-destructive" disabled={!!busy || selected.state === 'running' || selected.state === 'stopping'}
                    onClick={() => void act(selected, 'delete')} title={t('common.delete')}><Trash2 className="h-4 w-4" /></Button>
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
              <Field label={t('programs.cwd')} id="program-cwd" hint={t('programs.cwd_hint')}>
                <Input id="program-cwd" value={editing.cwd} placeholder={t('programs.cwd_placeholder')} onChange={(e) => setEditing({ ...editing, cwd: e.target.value })} />
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

