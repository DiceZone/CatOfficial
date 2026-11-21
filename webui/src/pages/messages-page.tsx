import { useTranslation } from 'react-i18next';
import { useInfiniteQuery } from '@tanstack/react-query';
import { RefreshCw } from 'lucide-react';
import { Card } from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import { PageHeader } from '@/components/page-header';
import { api, type MessageRow } from '@/lib/api';
import { currentLocale } from '@/i18n';
import { formatTime } from '@/lib/utils';

const PAGE = 50;

export function MessagesPage() {
  const { t } = useTranslation();
  const locale = currentLocale();
  const query = useInfiniteQuery({
    queryKey: ['messages'],
    queryFn: ({ pageParam }) => api.messages(pageParam, PAGE),
    initialPageParam: 0,
    getNextPageParam: (last: MessageRow[]) => (last.length < PAGE ? undefined : last[last.length - 1].id),
  });
  const rows = query.data?.pages.flat() ?? [];
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
                  <TableCell className="max-w-xl whitespace-pre-wrap break-all text-sm" title={m.content}>{m.text || m.content}</TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </div>
        {!query.isLoading && rows.length === 0 && <p className="p-6 text-center text-sm text-muted-foreground">{t('messages.empty')}</p>}
        {query.hasNextPage && (
          <div className="border-t p-3 text-center">
            <Button variant="ghost" size="sm" disabled={query.isFetchingNextPage} onClick={() => query.fetchNextPage()}>{t('common.load_more')}</Button>
          </div>
        )}
      </Card>
    </>
  );
}
