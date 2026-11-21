// 自定义回复：与 Dice!Next 的自定义回复同一套规则写法（条件、随机回复与权重、概率、冷却、范围、优先级）。

import React from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { FlaskConical, Plus, Search, X } from 'lucide-react';
import { toast } from 'sonner';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';
import { Textarea } from '@/components/ui/textarea';
import { Dialog, DialogBody, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { PageHeader } from '@/components/page-header';
import { useConfirm } from '@/components/confirm-dialog';
import { FeatureHelp } from '@/components/ui/feature-help';
import { Choice, Field, digits, errorText } from '@/components/form';
import { ReplyTable } from '@/components/reply-table';
import { MarkdownToolbar, useMarkdownEditing } from '@/components/markdown/markdown-toolbar';
import { MarkdownView } from '@/components/markdown/markdown-view';
import { api, botLabel, type MatchType, type ReplyRule, type ReplyVariable } from '@/lib/api';
import { cn } from '@/lib/utils';

// 下拉顺序与 Dice!Next 相同：包含 → 完全匹配 → 前缀 → 正则。
const MATCH_TYPES: MatchType[] = ['search', 'keyword', 'prefix', 'regex'];

const emptyRule = (): ReplyRule => ({
  id: '', name: '', enabled: true, builtin: false, priority: 100, conditions: [{ type: 'keyword', content: '' }], logic: 'or',
  results: [''], weights: [1], format: 'plain', scope: 'all', bots: [], prob: 100, cooldown_sec: 0, pass_through: false,
});

const regexError = (source: string) => { try { new RegExp(source); return ''; } catch (e) { return String((e as Error).message); } };

/** 一条回复的编辑框：Markdown 格式时带工具栏与预览。 */
function ResultEditor({ value, onChange, markdown, onFocus, index, inputRef }: {
  value: string; onChange: (v: string) => void; markdown: boolean; onFocus: () => void; index: number; inputRef: React.RefObject<HTMLTextAreaElement>;
}) {
  const { t } = useTranslation();
  const keys = useMarkdownEditing({ textareaRef: inputRef, value, onChange });
  return (
    <div className="space-y-2">
      {markdown && <MarkdownToolbar textareaRef={inputRef} value={value} onChange={onChange} />}
      <div className={cn('grid gap-3', markdown && 'lg:grid-cols-2')}>
        <Textarea ref={inputRef} value={value} rows={5} onFocus={onFocus} onChange={(e) => onChange(e.target.value)}
          onKeyDown={markdown ? keys.onKeyDown : undefined} placeholder={t('replies.result_ph')} aria-label={t('replies.result_n', { n: index + 1 })}
          className="font-mono text-sm" />
        {markdown && <div className="min-h-[7rem] overflow-y-auto rounded-md border bg-muted/20 p-3 text-sm">
          {value.trim() ? <MarkdownView text={value} /> : <span className="text-muted-foreground">{t('markdown.preview_empty')}</span>}
        </div>}
      </div>
    </div>
  );
}

function ReplyForm({ open, onOpenChange, rule, variables, onSaved }: {
  open: boolean; onOpenChange: (v: boolean) => void; rule: ReplyRule | null; variables: ReplyVariable[]; onSaved: () => void;
}) {
  const { t } = useTranslation();
  const bots = useQuery({ queryKey: ['bots'], queryFn: api.bots });
  const officials = (bots.data ?? []).filter((b) => b.type === 'official');
  const [draft, setDraft] = React.useState<ReplyRule>(emptyRule);
  const [error, setError] = React.useState('');
  const [busy, setBusy] = React.useState(false);
  const [active, setActive] = React.useState(0);
  const refs = React.useRef<React.RefObject<HTMLTextAreaElement>[]>([]);
  React.useEffect(() => { if (open) { setDraft(rule ? structuredClone(rule) : emptyRule()); setError(''); setActive(0); } }, [open, rule]);
  const set = (p: Partial<ReplyRule>) => setDraft((d) => ({ ...d, ...p }));
  while (refs.current.length < draft.results.length) refs.current.push(React.createRef<HTMLTextAreaElement>());

  const insertVariable = (name: string) => {
    const i = Math.min(active, draft.results.length - 1);
    const area = refs.current[i]?.current;
    const value = draft.results[i] ?? '';
    const start = area?.selectionStart ?? value.length, end = area?.selectionEnd ?? value.length;
    const token = `{${name}}`;
    set({ results: draft.results.map((r, j) => (j === i ? value.slice(0, start) + token + value.slice(end) : r)) });
    requestAnimationFrame(() => { area?.focus(); area?.setSelectionRange(start + token.length, start + token.length); });
  };

  const total = draft.weights.reduce((a, w, i) => a + (draft.results[i]?.trim() ? w : 0), 0);
  const submit = async () => {
    const conditions = draft.conditions.filter((c) => c.content.trim());
    if (!conditions.length) { setError(t('replies.err_need_cond')); return; }
    const bad = conditions.find((c) => c.type === 'regex' && regexError(c.content));
    if (bad) { setError(t('replies.err_bad_regex', { err: regexError(bad.content) })); return; }
    const pairs = draft.results.map((r, i) => [r, draft.weights[i] ?? 1] as const).filter(([r]) => r.trim());
    if (!pairs.length) { setError(t('replies.err_need_result')); return; }
    if (!pairs.some(([, w]) => w > 0)) { setError(t('replies.err_weights')); return; }
    setBusy(true);
    try {
      await api.saveReply({ ...draft, conditions, results: pairs.map(([r]) => r), weights: pairs.map(([, w]) => w) });
      toast.success(t('common.saved'));
      onSaved(); onOpenChange(false);
    } catch (e) { setError(errorText(e)); }
    finally { setBusy(false); }
  };

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-4xl">
        <DialogHeader>
          <DialogTitle className="flex items-center gap-2">{t(rule ? 'replies.edit_title' : 'replies.add_title')}
            <FeatureHelp title={t('replies.result_label')} description={<><p>{t('replies.form_desc')}</p><p>{t('replies.var_hint')}</p><p>{t('replies.result_weight_hint')}</p></>} />
          </DialogTitle>
          <DialogDescription className="sr-only">{t('replies.form_desc')}</DialogDescription>
        </DialogHeader>
        <DialogBody className="space-y-5">
          <Field label={t('replies.name')} id="reply-name"><Input id="reply-name" value={draft.name} placeholder={t('replies.name_ph')} onChange={(e) => set({ name: e.target.value })} /></Field>

          <div className="space-y-2">
            <div className="flex flex-wrap items-center justify-between gap-2">
              <Label>{t('replies.cond_label')}</Label>
              {draft.conditions.length > 1 && (
                <div className="flex gap-1 text-xs">
                  {(['and', 'or'] as const).map((l) => (
                    <button key={l} type="button" onClick={() => set({ logic: l })}
                      className={cn('rounded border px-2 py-0.5', draft.logic === l ? 'border-primary bg-primary text-primary-foreground' : 'text-muted-foreground')}>{t(`replies.logic_${l}`)}</button>
                  ))}
                </div>
              )}
            </div>
            {draft.conditions.map((c, i) => (
              <div key={i} className="space-y-1">
                <div className="flex gap-2">
                  <Select value={c.type} onValueChange={(v) => set({ conditions: draft.conditions.map((x, j) => (j === i ? { ...x, type: v as MatchType } : x)) })}>
                    <SelectTrigger className="w-28 shrink-0"><SelectValue /></SelectTrigger>
                    <SelectContent>{MATCH_TYPES.map((m) => <SelectItem key={m} value={m}>{t(`replies.mt_${m}`)}</SelectItem>)}</SelectContent>
                  </Select>
                  <Input value={c.content} placeholder={t(`replies.ph_${c.type}`)} className={cn('flex-1', c.type === 'regex' && 'font-mono')}
                    onChange={(e) => set({ conditions: draft.conditions.map((x, j) => (j === i ? { ...x, content: e.target.value } : x)) })} />
                  {draft.conditions.length > 1 && <Button variant="ghost" size="icon" aria-label={t('common.remove')} onClick={() => set({ conditions: draft.conditions.filter((_, j) => j !== i) })}><X className="h-4 w-4" /></Button>}
                </div>
                {c.type === 'regex' && c.content && regexError(c.content) && <p className="pl-[7.5rem] text-xs text-destructive">{t('replies.regex_invalid')}</p>}
              </div>
            ))}
            <Button variant="outline" size="sm" onClick={() => set({ conditions: [...draft.conditions, { type: 'search', content: '' }] })}><Plus className="mr-1 h-3.5 w-3.5" />{t('replies.add_cond')}</Button>
          </div>

          <div className="space-y-3">
            <div className="flex flex-wrap items-center justify-between gap-3">
              <Label>{t('replies.result_label')}{draft.results.length > 1 && <span className="ml-1 text-xs font-normal text-muted-foreground">{t('replies.result_random')}</span>}</Label>
              <label className="flex items-center gap-2 text-xs">{t('replies.format')}
                <Select value={draft.format} onValueChange={(v) => set({ format: v as ReplyRule['format'] })}>
                  <SelectTrigger className="h-8 w-32"><SelectValue /></SelectTrigger>
                  <SelectContent><SelectItem value="plain">{t('replies.format_plain')}</SelectItem><SelectItem value="markdown">Markdown</SelectItem></SelectContent>
                </Select>
              </label>
            </div>
            <div className="flex flex-wrap gap-1.5" aria-label={t('replies.variables')}>
              {variables.map((v) => (
                <button key={v.name} type="button" title={v.desc} onMouseDown={(e) => e.preventDefault()} onClick={() => insertVariable(v.name)}
                  className="rounded border bg-muted/40 px-1.5 py-0.5 font-mono text-[11px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground">{`{${v.name}}`}</button>
              ))}
            </div>
            {draft.results.map((r, i) => (
              <div key={i} className="space-y-2 rounded-md border p-3">
                <div className="flex items-center justify-between gap-2">
                  <span className="text-xs text-muted-foreground">{t('replies.result_n', { n: i + 1 })}</span>
                  {draft.results.length > 1 && <Button variant="ghost" size="icon" className="h-7 w-7" aria-label={t('replies.remove_result')}
                    onClick={() => set({ results: draft.results.filter((_, j) => j !== i), weights: draft.weights.filter((_, j) => j !== i) })}><X className="h-3.5 w-3.5" /></Button>}
                </div>
                <ResultEditor value={r} index={i} inputRef={refs.current[i]} markdown={draft.format === 'markdown'} onFocus={() => setActive(i)}
                  onChange={(v) => set({ results: draft.results.map((x, j) => (j === i ? v : x)) })} />
                {draft.results.length > 1 && (
                  <div className="flex items-center gap-2 text-xs">
                    <Label htmlFor={`weight-${i}`} className="text-xs">{t('replies.result_weight')}</Label>
                    <Input id={`weight-${i}`} inputMode="numeric" className="h-8 w-20" value={draft.weights[i] ?? 1}
                      onChange={(e) => set({ weights: draft.results.map((_, j) => (j === i ? Math.min(10000, digits(e.target.value)) : draft.weights[j] ?? 1)) })} />
                    <span className="text-muted-foreground">{t('replies.result_probability', { value: total > 0 && r.trim() ? (((draft.weights[i] ?? 1) / total) * 100).toFixed(1) : '0' })}</span>
                  </div>
                )}
              </div>
            ))}
            <Button variant="outline" size="sm" onClick={() => set({ results: [...draft.results, ''], weights: [...draft.weights, 1] })}><Plus className="mr-1 h-3.5 w-3.5" />{t('replies.add_result')}</Button>
          </div>

          <div className="space-y-4 rounded-md border p-4">
            <p className="text-sm font-medium">{t('replies.advanced')}</p>
            <div className="grid gap-4 sm:grid-cols-2 lg:grid-cols-4">
              <Field label={t('replies.priority')} hint={t('replies.priority_hint')}><Input inputMode="numeric" value={draft.priority} onChange={(e) => set({ priority: digits(e.target.value) })} /></Field>
              <Field label={t('replies.limit_prob')}><Input inputMode="numeric" value={draft.prob} onChange={(e) => set({ prob: Math.min(100, digits(e.target.value)) })} /></Field>
              <Field label={t('replies.limit_cd')} hint={t('replies.limit_cd_hint')}><Input inputMode="numeric" value={draft.cooldown_sec} onChange={(e) => set({ cooldown_sec: digits(e.target.value) })} /></Field>
              <Field label={t('replies.scope')}>
                <Choice value={draft.scope} onChange={(v) => set({ scope: v as ReplyRule['scope'] })}
                  options={[{ value: 'all', label: t('replies.scope_all') }, { value: 'group', label: t('replies.scope_group') }, { value: 'private', label: t('replies.scope_private') }]} />
              </Field>
            </div>
            {officials.length > 1 && (
              <div className="space-y-2">
                <Label>{t('replies.bots')}</Label>
                <div className="flex flex-wrap gap-2">
                  {officials.map((b) => {
                    const on = draft.bots.includes(b.id);
                    return <button key={b.id} type="button" onClick={() => set({ bots: on ? draft.bots.filter((x) => x !== b.id) : [...draft.bots, b.id] })}
                      className={cn('rounded-full border px-3 py-1 text-xs', on ? 'border-primary bg-primary/10 text-primary' : 'text-muted-foreground')}>{botLabel(b)}</button>;
                  })}
                </div>
                <p className="text-xs text-muted-foreground">{t('replies.bots_hint')}</p>
              </div>
            )}
            <div className="flex items-start justify-between gap-4">
              <div><p className="text-sm font-medium">{t('replies.pass_through')}</p><p className="mt-1 text-xs text-muted-foreground">{t('replies.pass_through_hint')}</p></div>
              <Switch checked={draft.pass_through} onCheckedChange={(v) => set({ pass_through: v })} aria-label={t('replies.pass_through')} />
            </div>
          </div>
          {error && <p role="alert" className="text-sm text-destructive">{error}</p>}
        </DialogBody>
        <DialogFooter>
          <Button variant="outline" onClick={() => onOpenChange(false)} disabled={busy}>{t('common.cancel')}</Button>
          <Button onClick={() => void submit()} disabled={busy}>{busy ? t('common.saving') : t(rule ? 'replies.save_edit' : 'common.add')}</Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}

function TryIt() {
  const { t } = useTranslation();
  const [message, setMessage] = React.useState('#co');
  const [scene, setScene] = React.useState('group');
  const [result, setResult] = React.useState<{ matched?: boolean; rule_name?: string; rule_id?: string; format?: string; text?: string } | null>(null);
  const run = async () => {
    try { setResult(await api.testReply({ message, scene })); } catch (e) { toast.error(t('common.failed', { error: errorText(e) })); }
  };
  return (
    <Card>
      <CardHeader><CardTitle className="flex items-center gap-2"><FlaskConical className="h-4 w-4" />{t('replies.try_title')}</CardTitle><CardDescription>{t('replies.try_hint')}</CardDescription></CardHeader>
      <CardContent className="space-y-3">
        <div className="flex flex-wrap gap-2">
          <Input value={message} onChange={(e) => setMessage(e.target.value)} onKeyDown={(e) => { if (e.key === 'Enter') void run(); }}
            placeholder={t('replies.try_ph')} aria-label={t('replies.try_ph')} className="min-w-[12rem] flex-1" />
          <Select value={scene} onValueChange={setScene}>
            <SelectTrigger className="w-28"><SelectValue /></SelectTrigger>
            <SelectContent><SelectItem value="group">{t('messages.group')}</SelectItem><SelectItem value="private">{t('messages.private')}</SelectItem></SelectContent>
          </Select>
          <Button onClick={() => void run()}>{t('replies.try')}</Button>
        </div>
        {result && (result.matched
          ? <div className="space-y-2 rounded-md border bg-muted/20 p-3 text-sm">
              <p className="text-xs text-muted-foreground">{t('replies.try_hit', { name: result.rule_name || result.rule_id })}</p>
              {result.format === 'markdown' ? <MarkdownView text={result.text ?? ''} /> : <p className="whitespace-pre-wrap">{result.text}</p>}
            </div>
          : <p className="text-sm text-muted-foreground">{t('replies.try_miss')}</p>)}
      </CardContent>
    </Card>
  );
}

export function RepliesPage() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const query = useQuery({ queryKey: ['replies'], queryFn: api.replies });
  const [editing, setEditing] = React.useState<ReplyRule | null>(null);
  const [formOpen, setFormOpen] = React.useState(false);
  const [filterText, setFilterText] = React.useState('');
  const [matchType, setMatchType] = React.useState<'all' | MatchType>('all');
  const [status, setStatus] = React.useState<'all' | 'enabled' | 'disabled'>('all');
  const { confirm, node } = useConfirm();
  const refresh = () => void client.invalidateQueries({ queryKey: ['replies'] });
  const act = async (job: () => Promise<unknown>, done?: string) => {
    try { await job(); if (done) toast.success(done); refresh(); } catch (e) { toast.error(t('common.failed', { error: errorText(e) })); }
  };
  const open = (rule: ReplyRule | null) => { setEditing(rule); setFormOpen(true); };
  const title = (r: ReplyRule) => r.name || r.conditions[0]?.content || r.id;

  return (
    <>
      {node}
      <PageHeader title={t('replies.title')} description={t('replies.description')}
        actions={<Button size="sm" disabled={query.isLoading} onClick={() => open(null)}><Plus className="mr-2 h-4 w-4" />{t('replies.add')}</Button>} />
      <div className="grid items-start gap-6 2xl:grid-cols-[minmax(0,1fr)_320px]">
        <div className="min-w-0 space-y-4">
          <div className="grid grid-cols-2 items-center gap-2 md:flex md:flex-wrap">
            <div className="relative col-span-2 min-w-0 md:flex-1">
              <Search className="absolute left-3 top-1/2 h-4 w-4 -translate-y-1/2 text-muted-foreground" />
              <Input placeholder={t('replies.search_placeholder')} aria-label={t('replies.search_placeholder')} value={filterText} onChange={(e) => setFilterText(e.target.value)} className="pl-9" />
            </div>
            <Select value={matchType} onValueChange={(v) => setMatchType(v as 'all' | MatchType)}>
              <SelectTrigger className="w-full md:w-[150px]" aria-label={t('replies.filter_match_type')}><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="all">{t('replies.filter_type_all')}</SelectItem>
                {(['keyword', 'prefix', 'search', 'regex'] as const).map((m) => <SelectItem key={m} value={m}>{t(`replies.mt_${m}`)}</SelectItem>)}
              </SelectContent>
            </Select>
            <Select value={status} onValueChange={(v) => setStatus(v as 'all' | 'enabled' | 'disabled')}>
              <SelectTrigger className="w-full md:w-[130px]" aria-label={t('replies.filter_status')}><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="all">{t('replies.filter_status_all')}</SelectItem>
                <SelectItem value="enabled">{t('replies.filter_enabled')}</SelectItem>
                <SelectItem value="disabled">{t('replies.filter_disabled')}</SelectItem>
              </SelectContent>
            </Select>
          </div>
          {query.isError
            ? <div role="alert" className="space-y-2 rounded-lg border border-destructive/40 p-4">
                <p className="text-sm text-destructive">{t('common.load_failed')}</p>
                <Button variant="outline" onClick={() => void query.refetch()}>{t('common.retry')}</Button>
              </div>
            : query.isLoading ? <div className="h-64 animate-pulse rounded-lg bg-muted" />
            : <ReplyTable rules={query.data?.rules ?? []} filterText={filterText} matchTypeFilter={matchType} statusFilter={status}
                onEdit={open}
                onToggle={(r, v) => void act(() => api.saveReply({ ...r, enabled: v }))}
                onReset={(r) => void confirm({ title: t('replies.reset'), description: t('replies.reset_confirm') }).then((ok) => { if (ok) void act(() => api.resetReply(r.id), t('replies.reset_done')); })}
                onDelete={(r) => void confirm({ title: t('common.delete'), description: `${title(r)} · ${t('replies.delete_confirm')}`, confirmLabel: t('common.delete'), destructive: true })
                  .then((ok) => { if (ok) void act(() => api.deleteReply(r.id), t('replies.deleted')); })} />}
        </div>
        <div className="min-w-0 2xl:sticky 2xl:top-0"><TryIt /></div>
      </div>
      <ReplyForm open={formOpen} onOpenChange={setFormOpen} rule={editing} variables={query.data?.variables ?? []} onSaved={refresh} />
    </>
  );
}
