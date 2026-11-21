import React from 'react';
import { useTranslation } from 'react-i18next';
import { Pause, Play, Trash2 } from 'lucide-react';
import { Card } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { PageHeader } from '@/components/page-header';
import { Choice } from '@/components/form';
import { api, type LogEntry } from '@/lib/api';
import { cn } from '@/lib/utils';

const LEVEL_STYLE: Record<LogEntry['level'], string> = {
  debug: 'text-muted-foreground',
  info: 'text-sky-600 dark:text-sky-400',
  warn: 'text-amber-600 dark:text-amber-400',
  error: 'text-red-600 dark:text-red-400',
};
const MAX = 3000;

function clock(seconds: number) {
  const d = new Date(seconds * 1000);
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

export function LogsPage() {
  const { t } = useTranslation();
  const [entries, setEntries] = React.useState<LogEntry[]>([]);
  const [level, setLevel] = React.useState('all');
  const [filter, setFilter] = React.useState('');
  const [paused, setPaused] = React.useState(false);
  const lastSeq = React.useRef(0);
  const box = React.useRef<HTMLDivElement>(null);

  React.useEffect(() => {
    let stopped = false;
    const load = async () => {
      try {
        const next = await api.logs(lastSeq.current);
        if (stopped || next.length === 0) return;
        lastSeq.current = next[next.length - 1].seq;
        setEntries((prev) => [...prev, ...next].slice(-MAX));
      } catch { /* 下次再试 */ }
    };
    void load();
    const timer = window.setInterval(load, 2000);
    return () => { stopped = true; window.clearInterval(timer); };
  }, []);

  const shown = entries.filter((e) => {
    if (level !== 'all' && e.level !== level) return false;
    return !filter || e.message.toLowerCase().includes(filter.toLowerCase());
  });

  React.useLayoutEffect(() => {
    if (!paused && box.current) box.current.scrollTop = box.current.scrollHeight;
  }, [shown.length, paused]);

  return (
    <>
      <PageHeader title={t('logs.title')} description={t('logs.description')}
        actions={<>
          <Button variant="outline" size="sm" onClick={() => setPaused((p) => !p)}>
            {paused ? <Play className="mr-2 h-4 w-4" /> : <Pause className="mr-2 h-4 w-4" />}{t(paused ? 'logs.resume' : 'logs.pause')}
          </Button>
          <Button variant="outline" size="sm" onClick={() => setEntries([])}><Trash2 className="mr-2 h-4 w-4" />{t('logs.clear')}</Button>
        </>} />
      <div className="flex flex-wrap gap-3">
        <div className="w-40">
          <Choice value={level} onChange={setLevel} options={[
            { value: 'all', label: t('logs.level_all') }, { value: 'debug', label: 'DEBUG' }, { value: 'info', label: 'INFO' },
            { value: 'warn', label: 'WARN' }, { value: 'error', label: 'ERROR' }]} />
        </div>
        <Input className="max-w-sm" placeholder={t('logs.filter')} value={filter} onChange={(e) => setFilter(e.target.value)} />
      </div>
      <Card className="overflow-hidden">
        <div ref={box} className="h-[calc(100dvh-20rem)] min-h-[20rem] overflow-y-auto bg-card p-3 font-mono text-xs leading-relaxed">
          {shown.length === 0 && <p className="p-3 text-muted-foreground">{t('logs.empty')}</p>}
          {shown.map((e) => (
            <div key={e.seq} className="flex gap-3 whitespace-pre-wrap break-all py-0.5">
              <span className="shrink-0 text-muted-foreground">{clock(e.time)}</span>
              <span className={cn('w-12 shrink-0 font-semibold uppercase', LEVEL_STYLE[e.level])}>{e.level}</span>
              <span className="min-w-0">{e.message}</span>
            </div>
          ))}
        </div>
      </Card>
    </>
  );
}
