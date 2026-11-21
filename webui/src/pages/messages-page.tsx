import React from 'react';
import { useTranslation } from 'react-i18next';
import { keepPreviousData, useQuery } from '@tanstack/react-query';
import { ExternalLink, RefreshCw } from 'lucide-react';
import { Card } from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Dialog, DialogContent, DialogTitle } from '@/components/ui/dialog';
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import { PaginationBar } from '@/components/ui/pagination-bar';
import { PageHeader } from '@/components/page-header';
import { api } from '@/lib/api';
import { currentLocale } from '@/i18n';
import { formatTime } from '@/lib/utils';

const PAGE_SIZE = 50;

export function MessagesPage() {
  const { t } = useTranslation();
  const locale = currentLocale();
  const [page, setPage] = React.useState(1);
  const [preview, setPreview] = React.useState<string | null>(null);
  const query = useQuery({ queryKey: ['messages', page], queryFn: () => api.messages(page, PAGE_SIZE), placeholderData: keepPreviousData });
  const rows = query.data?.items ?? [];
  const total = query.data?.total ?? 0;
  const label = (name: string | undefined, id: number | undefined) => (name ? `${name} (${id})` : String(id ?? '—'));

  return (
    <>
      <PageHeader title={t('messages.title')} description={t('messages.description')}
        actions={<Button variant="outline" size="sm" onClick={() => query.refetch()}><RefreshCw className="mr-2 h-4 w-4" />{t('common.refresh')}</Button>} />
      <Card className="overflow-hidden">
        <div className="overflow-x-auto">
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead className="w-44">{t('messages.time')}</TableHead>
                <TableHead className="w-16">{t('messages.direction')}</TableHead>
                <TableHead>{t('messages.conversation')}</TableHead>
                <TableHead>{t('messages.sender')}</TableHead>
                <TableHead className="min-w-[16rem]">{t('messages.content')}</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {rows.map((m) => (
                <TableRow key={m.id}>
                  <TableCell className="whitespace-nowrap font-mono text-xs text-muted-foreground">{formatTime(m.time, locale)}</TableCell>
                  <TableCell><Badge variant={m.self ? 'info' : 'neutral'}>{t(m.self ? 'messages.outgoing' : 'messages.incoming')}</Badge></TableCell>
                  <TableCell className="text-sm">
                    <span className="mr-1 text-muted-foreground">{t(m.scene === 'group' ? 'messages.group' : 'messages.private')}</span>
                    {label(m.peer_name, m.peer)}
                  </TableCell>
                  <TableCell className="text-sm">{m.self ? t('messages.self') : label(m.sender_name, m.sender)}</TableCell>
                  <TableCell className="max-w-xl text-sm" title={m.content}>
                    {m.text && <p className="whitespace-pre-wrap break-all">{m.text}</p>}
                    {m.images.length > 0 && (
                      <div className="mt-1.5 flex flex-wrap gap-1.5">
                        {m.images.map((url) => (
                          <button key={url} type="button" onClick={() => setPreview(url)} aria-label={t('messages.preview')}
                            className="overflow-hidden rounded-md border bg-muted transition-opacity hover:opacity-80 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring">
                            <img src={url} alt="" loading="lazy" referrerPolicy="no-referrer" className="h-16 w-16 object-cover" />
                          </button>
                        ))}
                      </div>
                    )}
                    {!m.text && m.images.length === 0 && <span className="break-all text-muted-foreground">{m.content}</span>}
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </div>
        {!query.isLoading && rows.length === 0 && <p className="p-6 text-center text-sm text-muted-foreground">{t('messages.empty')}</p>}
        <div className="border-t px-4 pb-3">
          <PaginationBar total={total} page={page} pageSize={PAGE_SIZE} onPageChange={setPage} fixedSize disabled={query.isFetching} />
        </div>
      </Card>

      <Dialog open={preview !== null} onOpenChange={(v) => { if (!v) setPreview(null); }}>
        <DialogContent className="max-w-4xl p-3">
          <DialogTitle className="sr-only">{t('messages.preview')}</DialogTitle>
          {preview && (
            <div className="space-y-2">
              <img src={preview} alt="" referrerPolicy="no-referrer" className="mx-auto max-h-[75vh] w-auto rounded-md object-contain" />
              <a href={preview} target="_blank" rel="noreferrer" className="inline-flex items-center gap-1 text-xs text-primary hover:underline">
                {t('messages.open_original')}<ExternalLink className="h-3 w-3" />
              </a>
            </div>
          )}
        </DialogContent>
      </Dialog>
    </>
  );
}
