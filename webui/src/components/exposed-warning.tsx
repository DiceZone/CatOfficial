import { useTranslation } from 'react-i18next';
import { TriangleAlert } from 'lucide-react';
import { Card, CardContent } from '@/components/ui/card';

/// 面板监听的不是本机时，文件管理、网页终端与程序托管页顶部的风险提示（样式同设置页的公网警告）。
export function ExposedWarning() {
  const { t } = useTranslation();
  return (
    <Card className="mb-4 border-amber-500/40 bg-amber-500/5">
      <CardContent className="flex items-start gap-3 text-sm">
        <TriangleAlert className="mt-0.5 h-4 w-4 shrink-0 text-amber-600" />{t('tools.exposed_warning')}
      </CardContent>
    </Card>
  );
}
