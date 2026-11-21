import React from 'react';
import { useTranslation } from 'react-i18next';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { Check, Plus, Search, Unlink } from 'lucide-react';
import { toast } from 'sonner';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { Badge } from '@/components/ui/badge';
import { Dialog, DialogBody, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import { PageHeader } from '@/components/page-header';
import { api, type Identity } from '@/lib/api';

interface Draft { type: 'user' | 'group'; target: string; qq: string }

export function BindingsPage() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const bindings = useQuery({ queryKey: ['bindings'], queryFn: api.bindings });
  const [query, setQuery] = React.useState('');
  const [search, setSearch] = React.useState('');
  const identities = useQuery({ queryKey: ['identities', search], queryFn: () => api.identities(search), enabled: search.length > 0 });
  const [draft, setDraft] = React.useState<Draft | null>(null);
  const [removing, setRemoving] = React.useState<Identity | null>(null);

  const fail = (e: unknown) => toast.error(t('common.failed', { error: e instanceof Error ? e.message : String(e) }));
  const refresh = () => {
    void client.invalidateQueries({ queryKey: ['bindings'] });
    void client.invalidateQueries({ queryKey: ['identities'] });
  };

  const bind = useMutation({
    mutationFn: (d: Draft) => {
      const qq = Number(d.qq);
      const target = d.target.trim();
      const numeric = /^\d+$/.test(target);
      return api.bind({
        type: d.type, qq,
        ...(numeric ? { [d.type === 'group' ? 'group_id' : 'user_id']: Number(target) } : { openid: target }),
      });
    },
    onSuccess: () => { toast.success(t('bindings.added')); setDraft(null); refresh(); },
    onError: fail,
  });
  const confirmGroup = useMutation({
    mutationFn: (g: { openid: string; real_group: number }) => api.bind({ type: 'group', openid: g.openid, qq: g.real_group }),
    onSuccess: () => { toast.success(t('bindings.added')); refresh(); },
    onError: fail,
  });
  const unbind = useMutation({
    mutationFn: (i: Identity) => api.unbind(i.type, i.openid),
    onSuccess: (r) => { toast.success(t('bindings.removed', { number: r.number })); setRemoving(null); refresh(); },
    onError: fail,
  });

  const typeLabel = (type: string) => t(type === 'group' ? 'bindings.group' : 'bindings.user');

  return (
    <>
      <PageHeader title={t('bindings.title')} description={t('bindings.description')}
        actions={<Button size="sm" onClick={() => setDraft({ type: 'user', target: '', qq: '' })}><Plus className="mr-2 h-4 w-4" />{t('bindings.add')}</Button>} />
      <p className="rounded-md border bg-card px-4 py-3 text-sm leading-relaxed text-muted-foreground">{t('bindings.how')}</p>

      {(bindings.data?.pending_groups.length ?? 0) > 0 && (
        <Card>
          <CardHeader>
            <CardTitle>{t('bindings.pending')}</CardTitle>
            <p className="text-sm text-muted-foreground">{t('bindings.pending_hint', { required: bindings.data?.required_votes })}</p>
          </CardHeader>
          <CardContent className="divide-y">
            {bindings.data!.pending_groups.map((g) => (
              <div key={`${g.openid}-${g.real_group}`} className="flex flex-wrap items-center gap-3 py-2.5">
                <span className="text-sm font-medium">{g.name || g.number}</span>
                <span className="font-mono text-xs text-muted-foreground">→ {g.real_group}</span>
                <Badge variant="warning">{t('bindings.votes', { votes: g.votes, required: bindings.data!.required_votes })}</Badge>
                <span className="min-w-0 flex-1 break-all font-mono text-xs text-muted-foreground">{g.openid}</span>
                <Button size="sm" variant="outline" onClick={() => confirmGroup.mutate(g)}><Check className="mr-1 h-4 w-4" />{t('bindings.confirm')}</Button>
              </div>
            ))}
          </CardContent>
        </Card>
      )}

      <Card className="overflow-hidden">
        <CardHeader><CardTitle>{t('bindings.list')}</CardTitle></CardHeader>
        <div className="overflow-x-auto">
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>{t('bindings.type')}</TableHead>
                <TableHead>{t('bindings.qq')}</TableHead>
                <TableHead>{t('bindings.name')}</TableHead>
                <TableHead>{t('bindings.openid')}</TableHead>
                <TableHead className="w-24" />
              </TableRow>
            </TableHeader>
            <TableBody>
              {(bindings.data?.bound ?? []).map((i) => (
                <TableRow key={`${i.type}-${i.openid}`}>
                  <TableCell><Badge variant="neutral">{typeLabel(i.type)}</Badge></TableCell>
                  <TableCell className="font-mono">{i.number}</TableCell>
                  <TableCell>{i.name || '—'}</TableCell>
                  <TableCell className="break-all font-mono text-xs text-muted-foreground">{i.openid}</TableCell>
                  <TableCell className="text-right">
                    <Button variant="ghost" size="sm" onClick={() => setRemoving(i)}><Unlink className="mr-1 h-4 w-4" />{t('bindings.unbind')}</Button>
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </div>
        {bindings.data?.bound.length === 0 && <p className="px-5 pb-5 text-sm text-muted-foreground">{t('bindings.empty')}</p>}
      </Card>

      <Card>
        <CardHeader><CardTitle>{t('bindings.search_title')}</CardTitle></CardHeader>
        <CardContent className="space-y-4">
          <form className="flex gap-2" onSubmit={(e) => { e.preventDefault(); setSearch(query.trim()); }}>
            <Input value={query} onChange={(e) => setQuery(e.target.value)} placeholder={t('bindings.search_placeholder')} />
            <Button type="submit" variant="outline"><Search className="h-4 w-4" /></Button>
          </form>
          {search && identities.data?.length === 0 && <p className="text-sm text-muted-foreground">{t('bindings.no_match')}</p>}
          <div className="divide-y">
            {(identities.data ?? []).map((i) => (
              <div key={`${i.type}-${i.openid}`} className="flex flex-wrap items-center gap-3 py-2.5">
                <Badge variant="neutral">{typeLabel(i.type)}</Badge>
                <span className="font-mono text-sm">{i.number}</span>
                <span className="text-sm">{i.name}</span>
                <span className="min-w-0 flex-1 break-all font-mono text-xs text-muted-foreground">{i.openid}</span>
                {i.bound ? <Badge variant="success">{t('bindings.list')}</Badge>
                  : <Button size="sm" variant="outline" onClick={() => setDraft({ type: i.type, target: String(i.number), qq: '' })}>{t('bindings.bind_this')}</Button>}
              </div>
            ))}
          </div>
        </CardContent>
      </Card>

      <Dialog open={draft !== null} onOpenChange={(open) => { if (!open) setDraft(null); }}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>{t('bindings.add')}</DialogTitle>
            <DialogDescription>{t('bindings.form_hint')}</DialogDescription>
          </DialogHeader>
          {draft && (
            <DialogBody className="space-y-4">
              <div className="space-y-2">
                <Label>{t('bindings.type')}</Label>
                <Select value={draft.type} onValueChange={(v) => setDraft({ ...draft, type: v as Draft['type'] })}>
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent>
                    <SelectItem value="user">{t('bindings.user')}</SelectItem>
                    <SelectItem value="group">{t('bindings.group')}</SelectItem>
                  </SelectContent>
                </Select>
              </div>
              <div className="space-y-2">
                <Label htmlFor="bind-target">{t('bindings.current')} / {t('bindings.openid')}</Label>
                <Input id="bind-target" value={draft.target} onChange={(e) => setDraft({ ...draft, target: e.target.value })} />
              </div>
              <div className="space-y-2">
                <Label htmlFor="bind-qq">{t('bindings.qq')}</Label>
                <Input id="bind-qq" inputMode="numeric" placeholder={t('bindings.qq_hint')} value={draft.qq}
                  onChange={(e) => setDraft({ ...draft, qq: e.target.value.replace(/\D/g, '') })} />
              </div>
            </DialogBody>
          )}
          <DialogFooter>
            <Button variant="outline" onClick={() => setDraft(null)}>{t('common.cancel')}</Button>
            <Button disabled={!draft?.target.trim() || !draft?.qq || bind.isPending} onClick={() => draft && bind.mutate(draft)}>{t('common.save')}</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <Dialog open={removing !== null} onOpenChange={(open) => { if (!open) setRemoving(null); }}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>{t('bindings.unbind')}</DialogTitle>
            <DialogDescription>{removing && t('bindings.unbind_confirm', { number: removing.number })}</DialogDescription>
          </DialogHeader>
          <DialogFooter>
            <Button variant="outline" onClick={() => setRemoving(null)}>{t('common.cancel')}</Button>
            <Button variant="destructive" disabled={unbind.isPending} onClick={() => removing && unbind.mutate(removing)}>{t('bindings.unbind')}</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  );
}
