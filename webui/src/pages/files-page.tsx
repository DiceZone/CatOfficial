import React from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery } from '@tanstack/react-query';
import { RefreshCw } from 'lucide-react';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { PageHeader } from '@/components/page-header';
import { ExposedWarning } from '@/components/exposed-warning';
import { FileManager } from '@/components/files/file-manager';
import { FileOpsList } from '@/components/files/file-ops-list';
import { apiClient } from '@/lib/bdc-compat';

interface FilesInfo { full_access: boolean; exposed: boolean; retention_days: number }

/// 文件管理页：照搬 BDC 的 InstanceFiles，下面附操作记录（BDC 放在实例的「操作记录」页签里）。
export function FilesPage() {
  const { t } = useTranslation();
  // 从程序托管「在文件管理中打开」过来：#/files?path=/data/instances/x
  const [openPath, setOpenPath] = React.useState<{ path: string; seq: number } | undefined>(() => {
    const requested = new URLSearchParams(window.location.hash.split('?')[1] || '').get('path');
    return requested ? { path: requested, seq: 1 } : undefined;
  });
  React.useEffect(() => {
    const update = () => {
      const requested = new URLSearchParams(window.location.hash.split('?')[1] || '').get('path');
      if (requested) setOpenPath((prev) => ({ path: requested, seq: (prev?.seq ?? 0) + 1 }));
    };
    window.addEventListener('hashchange', update);
    return () => window.removeEventListener('hashchange', update);
  }, []);
  const info = useQuery({ queryKey: ['files-info'], queryFn: () => apiClient.get<FilesInfo>('/api/webui/files/info') });
  const [opsKey, setOpsKey] = React.useState(0);
  const call = React.useCallback((payload: Record<string, unknown>) => apiClient.post<any>('/api/webui/files', payload), []);
  return (
    <>
      <PageHeader title={t('files.title')} description={t('files.description')} />
      {info.data?.exposed && <ExposedWarning />}
      <div className="space-y-6">
        <FileManager fullAccess={info.data?.full_access ?? false} openPath={openPath} />
        <Card>
          <CardHeader>
            <div className="flex items-start justify-between gap-3">
              <div>
                <CardTitle className="text-base">{t('files.ops_title')}</CardTitle>
                <CardDescription>{t('files.ops_desc')}</CardDescription>
              </div>
              <Button size="sm" variant="outline" onClick={() => setOpsKey((k) => k + 1)}><RefreshCw className="mr-1 h-4 w-4" />{t('common.refresh')}</Button>
            </div>
          </CardHeader>
          <CardContent><FileOpsList call={call} reloadKey={opsKey} /></CardContent>
        </Card>
      </div>
    </>
  );
}
