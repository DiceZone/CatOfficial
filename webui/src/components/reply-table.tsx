// 自定义回复列表：照 Dice!Next 的 reply-table —— 宽屏是可排序的表格，窄屏是卡片；
// 每条都带启用开关、编辑与删除（自带规则为「恢复默认」）。分页默认每页 5 条。

import React from 'react';
import { useTranslation } from 'react-i18next';
import { ArrowDown, ArrowUp, ArrowUpDown, Pencil, RotateCcw, Trash2 } from 'lucide-react';
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import { Button } from '@/components/ui/button';
import { Badge } from '@/components/ui/badge';
import { Switch } from '@/components/ui/switch';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { PaginationBar } from '@/components/ui/pagination-bar';
import type { MatchType, ReplyRule } from '@/lib/api';
import { cn } from '@/lib/utils';

type SortKey = 'type' | 'match' | 'reply' | 'priority' | 'enabled';
interface Props {
  rules: ReplyRule[];
  filterText: string;
  matchTypeFilter: 'all' | MatchType;
  statusFilter: 'all' | 'enabled' | 'disabled';
  onEdit: (rule: ReplyRule) => void;
  onDelete: (rule: ReplyRule) => void;
  onReset: (rule: ReplyRule) => void;
  onToggle: (rule: ReplyRule, enabled: boolean) => void;
}

const SIZES = [5, 10, 20, 50];
const truncate = (text: string, n: number) => ([...text].length > n ? [...text].slice(0, n).join('') + '…' : text);
const collator = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });

function SortHeader({ label, active, desc, onClick, hint }: { label: string; active: boolean; desc: boolean; onClick: () => void; hint: string }) {
  const Icon = !active ? ArrowUpDown : desc ? ArrowDown : ArrowUp;
  return <button type="button" onClick={onClick} title={hint}
    className="inline-flex items-center gap-1 whitespace-nowrap rounded-sm hover:text-foreground focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring">
    {label}<Icon className={cn('h-3.5 w-3.5', !active && 'text-muted-foreground/60')} />
  </button>;
}

export function ReplyTable({ rules, filterText, matchTypeFilter, statusFilter, onEdit, onDelete, onReset, onToggle }: Props) {
  const { t } = useTranslation();
  const [sort, setSort] = React.useState<{ key: SortKey; desc: boolean } | null>(null);
  const [page, setPage] = React.useState(1);
  const [size, setSize] = React.useState(5);

  const limits = (r: ReplyRule) => {
    const out: string[] = [];
    if (r.prob < 100) out.push(`${r.prob}%`);
    if (r.cooldown_sec > 0) out.push(`CD ${r.cooldown_sec}s`);
    if (r.scope !== 'all') out.push(t(r.scope === 'group' ? 'replies.scope_group' : 'replies.scope_private'));
    if (r.format === 'markdown') out.push('Markdown');
    if (r.pass_through) out.push(t('replies.pass_through_badge'));
    return out;
  };

  const filtered = React.useMemo(() => {
    const needle = filterText.trim().toLowerCase();
    return rules.filter((r) => {
      if (matchTypeFilter !== 'all' && !r.conditions.some((c) => c.type === matchTypeFilter)) return false;
      if (statusFilter === 'enabled' && !r.enabled) return false;
      if (statusFilter === 'disabled' && r.enabled) return false;
      if (!needle) return true;
      return [r.name, ...r.conditions.map((c) => c.content), ...r.results].join('\n').toLowerCase().includes(needle);
    });
  }, [rules, filterText, matchTypeFilter, statusFilter]);

  const sorted = React.useMemo(() => {
    const list = [...filtered];
    if (!sort) return list.sort((a, b) => b.priority - a.priority);
    const value = (r: ReplyRule): string | number => sort.key === 'type' ? r.conditions[0]?.type ?? '' : sort.key === 'match' ? r.conditions[0]?.content ?? ''
      : sort.key === 'reply' ? r.results[0] ?? '' : sort.key === 'priority' ? r.priority : r.enabled ? 1 : 0;
    list.sort((a, b) => {
      const x = value(a), y = value(b);
      const c = typeof x === 'number' && typeof y === 'number' ? x - y : collator.compare(String(x), String(y));
      return sort.desc ? -c : c;
    });
    return list;
  }, [filtered, sort]);

  React.useEffect(() => { setPage(1); }, [filterText, matchTypeFilter, statusFilter]);
  const pages = Math.max(1, Math.ceil(sorted.length / size));
  React.useEffect(() => { if (page > pages) setPage(pages); }, [page, pages]);
  const shown = sorted.slice((page - 1) * size, page * size);
  const toggleSort = (key: SortKey) => setSort((s) => (s?.key === key ? { key, desc: !s.desc } : { key, desc: false }));
  const header = (key: SortKey, label: string) =>
    <SortHeader label={label} active={sort?.key === key} desc={!!sort?.desc} onClick={() => toggleSort(key)} hint={t('replies.sort_hint')} />;
  const title = (r: ReplyRule) => r.name || r.conditions[0]?.content || r.id;
  const actions = (r: ReplyRule, wide: boolean) => <>
    <Button variant="ghost" size={wide ? 'icon' : 'sm'} className={wide ? 'h-8 w-8' : undefined} aria-label={`${t('common.edit')} · ${title(r)}`} onClick={() => onEdit(r)}>
      <Pencil className={wide ? 'h-3.5 w-3.5' : 'mr-1.5 h-3.5 w-3.5'} />{!wide && t('common.edit')}</Button>
    {r.builtin
      ? <Button variant="ghost" size={wide ? 'icon' : 'sm'} className={wide ? 'h-8 w-8' : undefined} title={t('replies.reset')} aria-label={`${t('replies.reset')} · ${title(r)}`} onClick={() => onReset(r)}>
          <RotateCcw className={wide ? 'h-3.5 w-3.5' : 'mr-1.5 h-3.5 w-3.5'} />{!wide && t('replies.reset')}</Button>
      : <Button variant="ghost" size={wide ? 'icon' : 'sm'} className={cn('text-destructive', wide && 'h-8 w-8')} aria-label={`${t('common.delete')} · ${title(r)}`} onClick={() => onDelete(r)}>
          <Trash2 className={wide ? 'h-3.5 w-3.5' : 'mr-1.5 h-3.5 w-3.5'} />{!wide && t('common.delete')}</Button>}
  </>;
  const empty = <span className="text-muted-foreground">{filterText ? t('replies.no_match') : t('replies.empty')}</span>;

  return (
    <div className="space-y-3">
      {/* 窄屏：卡片 */}
      <div className="space-y-3 sm:hidden">
        <div className="flex items-center gap-2">
          <Select value={sort?.key ?? 'default'} onValueChange={(key) => setSort(key === 'default' ? null : { key: key as SortKey, desc: sort?.desc ?? false })}>
            <SelectTrigger className="min-w-0 flex-1" aria-label={t('replies.sort_by')}><SelectValue /></SelectTrigger>
            <SelectContent>
              <SelectItem value="default">{t('replies.sort_default')}</SelectItem>
              {([['type', 'match_type'], ['match', 'match_content'], ['reply', 'reply_content'], ['priority', 'priority'], ['enabled', 'col_status']] as const)
                .map(([id, label]) => <SelectItem key={id} value={id}>{t('replies.' + label)}</SelectItem>)}
            </SelectContent>
          </Select>
          <Button variant="outline" disabled={!sort} onClick={() => setSort((s) => (s ? { ...s, desc: !s.desc } : s))}>
            {sort?.desc ? <ArrowDown className="mr-1.5 h-4 w-4" /> : <ArrowUp className="mr-1.5 h-4 w-4" />}{t(sort?.desc ? 'replies.descending' : 'replies.ascending')}
          </Button>
        </div>
        {shown.length === 0 ? <div className="rounded-lg border px-4 py-10 text-center text-sm">{empty}</div> : shown.map((r) => {
          const lim = limits(r);
          return <article key={r.id} className={cn('overflow-hidden rounded-xl border bg-card shadow-sm', !r.enabled && 'opacity-70')}>
            <div className="flex items-start justify-between gap-3 border-b px-4 py-3">
              <div className="min-w-0 flex-1">
                <div className="flex flex-wrap items-center gap-1.5">
                  <Badge variant="secondary" className="text-[11px]">{t('replies.mt_' + r.conditions[0]?.type)}</Badge>
                  {r.conditions.length > 1 && <Badge variant="outline" className="text-[11px]">+{r.conditions.length - 1}</Badge>}
                  {r.builtin && <Badge variant="info" className="text-[11px]">{t('replies.builtin')}</Badge>}
                </div>
                <h3 className="mt-2 whitespace-pre-wrap break-words font-mono text-sm font-semibold leading-5">{r.conditions[0]?.content}</h3>
                {r.name && <p className="mt-0.5 text-xs text-muted-foreground">{r.name}</p>}
              </div>
              <Switch checked={r.enabled} aria-label={`${t('replies.col_status')} · ${title(r)}`} onCheckedChange={(v) => onToggle(r, v)} />
            </div>
            <div className="space-y-3 px-4 py-3">
              {r.conditions.length > 1 && <p className="line-clamp-2 break-words text-xs text-muted-foreground">
                {r.conditions.slice(1).map((c) => `${t('replies.mt_' + c.type)}: ${c.content}`).join(' · ')}</p>}
              <div>
                <p className="text-[11px] font-medium text-muted-foreground">{t('replies.reply_content')}</p>
                <p className="mt-1 line-clamp-3 whitespace-pre-wrap break-words text-sm leading-5">{r.results[0]}</p>
                {r.results.length > 1 && <span className="mt-1 inline-block text-[11px] text-muted-foreground">+{r.results.length - 1} {t('replies.result_label')}</span>}
              </div>
              {lim.length > 0 && <div className="flex flex-wrap gap-1.5">{lim.map((l) => <Badge key={l} variant="outline" className="text-[11px] font-normal">{l}</Badge>)}</div>}
            </div>
            <div className="flex items-center gap-2 border-t bg-muted/20 px-4 py-2">
              <span className="mr-auto text-xs text-muted-foreground">{t('replies.priority')} {r.priority}</span>
              {actions(r, false)}
            </div>
          </article>;
        })}
      </div>

      {/* 宽屏：表格 */}
      <div className="hidden overflow-x-auto rounded-md border sm:block">
        <Table>
          <TableHeader>
            <TableRow>
              <TableHead style={{ width: 120 }}>{header('type', t('replies.match_type'))}</TableHead>
              <TableHead>{header('match', t('replies.match_content'))}</TableHead>
              <TableHead>{header('reply', t('replies.reply_content'))}</TableHead>
              <TableHead style={{ width: 90 }}>{header('priority', t('replies.priority'))}</TableHead>
              <TableHead style={{ width: 84 }}>{header('enabled', t('replies.col_status'))}</TableHead>
              <TableHead style={{ width: 90 }}>{t('replies.col_actions')}</TableHead>
            </TableRow>
          </TableHeader>
          <TableBody>
            {shown.length === 0
              ? <TableRow><TableCell colSpan={6} className="h-24 text-center">{empty}</TableCell></TableRow>
              : shown.map((r) => {
                const lim = limits(r);
                return (
                  <TableRow key={r.id} className={cn(!r.enabled && 'opacity-50')}>
                    <TableCell>
                      <div className="flex flex-wrap items-center gap-1">
                        <Badge variant="secondary" className="text-xs">{t('replies.mt_' + r.conditions[0]?.type)}</Badge>
                        {r.conditions.length > 1 && <Badge variant="outline" className="text-[10px]" title={t(`replies.logic_${r.logic}`)}>+{r.conditions.length - 1}</Badge>}
                      </div>
                    </TableCell>
                    <TableCell>
                      <div className="flex flex-wrap items-center gap-1.5">
                        <code className="rounded bg-muted px-1.5 py-0.5 font-mono text-xs">{truncate(r.conditions[0]?.content ?? '', 40)}</code>
                        {r.builtin && <Badge variant="info" className="text-[10px]">{t('replies.builtin')}</Badge>}
                      </div>
                      {(r.name || lim.length > 0) && <span className="mt-0.5 block text-[10px] text-muted-foreground">{[r.name, ...lim].filter(Boolean).join(' · ')}</span>}
                    </TableCell>
                    <TableCell>
                      <span className="text-sm text-muted-foreground">{truncate((r.results[0] ?? '').replace(/\s+/g, ' '), 50)}
                        {r.results.length > 1 && <Badge variant="outline" className="ml-1 text-[10px]" title={t('replies.result_random')}>×{r.results.length}</Badge>}</span>
                    </TableCell>
                    <TableCell><span className="font-mono text-xs">{r.priority}</span></TableCell>
                    <TableCell><Switch checked={r.enabled} aria-label={`${t('replies.col_status')} · ${title(r)}`} onCheckedChange={(v) => onToggle(r, v)} /></TableCell>
                    <TableCell><div className="flex items-center gap-1">{actions(r, true)}</div></TableCell>
                  </TableRow>
                );
              })}
          </TableBody>
        </Table>
      </div>
      <PaginationBar total={sorted.length} page={page} pageSize={size} onPageChange={setPage} sizes={SIZES}
        onPageSizeChange={(n) => { setSize(n); setPage(1); }} />
    </div>
  );
}
