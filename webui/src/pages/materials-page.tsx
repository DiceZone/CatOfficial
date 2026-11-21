// 官方图床：官方「素材管理」接口的图片，照 Dice!Next「QQ 官方 → 平台图片」的版式。
// 上传后得到 qqbot-media:// 引用，可写进 Markdown 消息：![说明 #宽px #高px](qqbot-media://…)。

import React from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery } from '@tanstack/react-query';
import { ImageOff, Images, MoreHorizontal, RefreshCw, Upload } from 'lucide-react';
import { toast } from 'sonner';
import { Card } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Dialog, DialogContent, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '@/components/ui/dropdown-menu';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { PageHeader } from '@/components/page-header';
import { FeatureHelp } from '@/components/ui/feature-help';
import { EmptyState, errorText } from '@/components/form';
import { useConfirm } from '@/components/confirm-dialog';
import { PlatformIcon } from '@/components/platform-icon';
import { api, botLabel, fileToBase64, type Material } from '@/lib/api';
import { currentLocale } from '@/i18n';

const https = (value?: string) => { try { return !!value && new URL(value).protocol === 'https:'; } catch { return false; } };
const acceptImage = (file?: File) => !!file && file.size >= 1 && file.size <= 10 * 1024 * 1024 && /^image\/(png|jpeg|gif|webp)$/.test(file.type);

function Thumbnail({ item, open }: { item: Material; open: (item: Material) => void }) {
  const { t } = useTranslation();
  const [failed, setFailed] = React.useState(false);
  const src = https(item.preview_url) ? item.preview_url : '';
  return (
    <button type="button" className="grid aspect-[4/3] w-full place-items-center overflow-hidden bg-muted/60" disabled={!src || failed}
      aria-label={t('materials.preview')} onClick={() => open(item)}>
      {src && !failed
        ? <img src={src} alt={item.file_name || item.image_id} loading="lazy" decoding="async" referrerPolicy="no-referrer" onError={() => setFailed(true)} className="h-full w-full object-contain" />
        : <span className="flex flex-col items-center gap-1 text-xs text-muted-foreground"><ImageOff className="size-5" />{t('materials.preview_failed')}</span>}
    </button>
  );
}

export function MaterialsPage() {
  const { t } = useTranslation();
  const locale = currentLocale();
  const { confirm, node } = useConfirm();
  const bots = useQuery({ queryKey: ['bots'], queryFn: api.bots });
  const officials = (bots.data ?? []).filter((b) => b.type === 'official' && b.enabled);
  const [bot, setBot] = React.useState('');
  const [busy, setBusy] = React.useState(false);
  const [error, setError] = React.useState('');
  const [items, setItems] = React.useState<Material[]>([]);
  const [total, setTotal] = React.useState<number>();
  const [cursors, setCursors] = React.useState(['']);
  const [next, setNext] = React.useState('');
  const [file, setFile] = React.useState<File>();
  const [preview, setPreview] = React.useState<Material>();
  const [replaceId, setReplaceId] = React.useState('');
  const replaceRef = React.useRef<HTMLInputElement>(null);
  const uploadRef = React.useRef<HTMLInputElement>(null);
  const current = officials.find((b) => b.id === bot);

  React.useEffect(() => { if (!bot && officials.length) setBot(officials[0].id); }, [officials, bot]);

  const run = async (job: () => Promise<void>) => {
    setBusy(true); setError('');
    try { await job(); } catch (e) { setError(errorText(e)); } finally { setBusy(false); }
  };
  const load = async (pages = ['']) => {
    const data = await api.materials(bot, pages[pages.length - 1]);
    setItems(Array.isArray(data.images) ? data.images : []);
    setTotal(Number.isFinite(Number(data.total)) ? Number(data.total) : undefined);
    setNext(typeof data.next_cursor === 'string' ? data.next_cursor : '');
    setCursors(pages);
  };
  React.useEffect(() => {
    if (!bot) return;
    setItems([]); setTotal(undefined);
    if (current?.runtime?.online) void run(() => load());
  }, [bot, current?.runtime?.online]);

  const upload = async (chosen: File) => {
    if (!acceptImage(chosen)) throw new Error(t('materials.limit'));
    const result = await api.uploadMaterial(bot, chosen.name, await fileToBase64(chosen));
    await load(cursors);
    toast.success(t('materials.uploaded'), { description: result.url });
  };
  const copy = async (text: string) => { await navigator.clipboard.writeText(text); toast.success(t('materials.copied')); };

  return (
    <>
      {node}
      <PageHeader title={t('materials.title')} description={t('materials.description')}
        actions={<Button variant="outline" size="sm" disabled={busy || !current?.runtime?.online} onClick={() => void run(() => load(cursors))}>
          <RefreshCw className={busy ? 'mr-2 h-4 w-4 animate-spin' : 'mr-2 h-4 w-4'} />{t('common.refresh')}</Button>} />

      {officials.length === 0
        ? <EmptyState icon={Images} title={t('materials.no_bot')} hint={t('materials.no_bot_hint')} />
        : <>
          <Card className="flex flex-wrap items-center gap-3 p-4">
            <span className="text-sm font-medium">{t('materials.bot')}</span>
            <Select value={bot} onValueChange={(v) => { setBot(v); setCursors(['']); }}>
              <SelectTrigger className="w-full sm:w-72" aria-label={t('materials.bot')}><SelectValue /></SelectTrigger>
              <SelectContent>{officials.map((b) => (
                <SelectItem key={b.id} value={b.id}><span className="inline-flex items-center gap-2"><PlatformIcon type="official" />{botLabel(b)}</span></SelectItem>
              ))}</SelectContent>
            </Select>
            {Number.isFinite(total) && <span className="text-xs tabular-nums text-muted-foreground">{t('materials.count', { n: total })}</span>}
            <FeatureHelp title={t('materials.title')} description={<><p>{t('materials.help')}</p><p>{t('materials.help_markdown')}</p></>} />
          </Card>

          {current && !current.runtime?.online && <p className="rounded-lg border border-amber-400/30 bg-amber-400/5 p-3 text-sm text-amber-600 dark:text-amber-300">{t('materials.offline')}</p>}
          {error && <p role="alert" className="break-all text-sm text-destructive">{error}</p>}

          <Card className="space-y-4 p-4">
            <div className="grid grid-cols-[minmax(0,1fr)_auto] items-center gap-2 sm:max-w-md">
              <Input ref={uploadRef} type="file" className="w-full min-w-0 truncate" accept="image/png,image/jpeg,image/gif,image/webp" disabled={busy || !current?.runtime?.online}
                aria-label={t('materials.upload')} onChange={(e) => setFile(e.target.files?.[0])} />
              <Button disabled={busy || !file || !current?.runtime?.online} onClick={() => void run(async () => {
                if (file) { await upload(file); setFile(undefined); if (uploadRef.current) uploadRef.current.value = ''; }
              })}><Upload className="mr-2 h-4 w-4" />{t('materials.upload')}</Button>
            </div>

            {items.length === 0 && !busy && current?.runtime?.online && <p className="text-sm text-muted-foreground">{t('materials.empty')}</p>}
            <div className="grid grid-cols-2 gap-2 sm:grid-cols-[repeat(auto-fill,minmax(13rem,1fr))] sm:gap-4">
              {items.map((item) => (
                <article key={item.image_id} className="min-w-0 overflow-hidden rounded-lg border bg-card">
                  <Thumbnail item={item} open={setPreview} />
                  <div className="space-y-1.5 p-2.5 sm:p-3">
                    <h3 title={item.file_name || item.image_id} className="truncate text-sm font-medium">{item.file_name || item.image_id}</h3>
                    {!!item.expires_at && <p className="text-xs text-muted-foreground">{t('materials.expires')} {new Date(item.expires_at * 1000).toLocaleDateString(locale)}</p>}
                    <div className="flex flex-wrap items-center justify-between gap-1 pt-1">
                      <Button size="sm" variant="outline" className="h-8 px-2 text-xs" disabled={busy}
                        onClick={() => void copy(`![${(item.file_name || 'image').replace(/[[\]]/g, '')}](${item.url})`)}>{t('materials.copy_markdown')}</Button>
                      <DropdownMenu>
                        <DropdownMenuTrigger asChild><Button size="icon" variant="ghost" className="h-8 w-8" disabled={busy} aria-label={t('common.more')}><MoreHorizontal className="size-4" /></Button></DropdownMenuTrigger>
                        <DropdownMenuContent align="end">
                          <DropdownMenuItem onClick={() => void copy(item.url)}>{t('materials.copy_url')}</DropdownMenuItem>
                          <DropdownMenuItem onClick={() => { setReplaceId(item.image_id); replaceRef.current?.click(); }}>{t('materials.replace')}</DropdownMenuItem>
                          <DropdownMenuItem className="text-destructive" onClick={() => void run(async () => {
                            if (!await confirm({ title: t('common.delete'), description: t('materials.delete_confirm'), confirmLabel: t('common.delete'), destructive: true })) return;
                            await api.deleteMaterial(bot, item.image_id);
                            await load(cursors.length > 1 && items.length === 1 ? cursors.slice(0, -1) : cursors);
                          })}>{t('common.delete')}</DropdownMenuItem>
                        </DropdownMenuContent>
                      </DropdownMenu>
                    </div>
                  </div>
                </article>
              ))}
            </div>
            {(cursors.length > 1 || next) && (
              <div className="flex gap-2">
                <Button variant="outline" disabled={busy || cursors.length < 2} onClick={() => void run(() => load(cursors.slice(0, -1)))}>{t('materials.previous')}</Button>
                <Button variant="outline" disabled={busy || !next} onClick={() => void run(() => load([...cursors, next]))}>{t('materials.next')}</Button>
              </div>
            )}
          </Card>
        </>}

      <input ref={replaceRef} type="file" accept="image/png,image/jpeg,image/gif,image/webp" className="hidden" onChange={(e) => {
        const chosen = e.target.files?.[0]; e.target.value = '';
        if (!chosen || !replaceId) return;
        void run(async () => {
          if (!acceptImage(chosen)) throw new Error(t('materials.limit'));
          if (!await confirm({ title: t('materials.replace'), description: t('materials.replace_confirm'), destructive: true })) return;
          await api.deleteMaterial(bot, replaceId);
          await upload(chosen);
        });
      }} />
      <Dialog open={!!preview} onOpenChange={(open) => { if (!open) setPreview(undefined); }}>
        <DialogContent className="max-w-3xl">
          <DialogHeader><DialogTitle className="truncate pr-6">{preview?.file_name || t('materials.preview')}</DialogTitle></DialogHeader>
          {preview && https(preview.preview_url) && <img src={preview.preview_url} alt={preview.file_name} referrerPolicy="no-referrer" className="max-h-[65vh] w-full object-contain" />}
          {preview && <p className="break-all font-mono text-xs text-muted-foreground">{preview.url}</p>}
        </DialogContent>
      </Dialog>
    </>
  );
}
