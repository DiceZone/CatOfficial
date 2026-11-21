import { useTranslation } from 'react-i18next';
import { useQuery } from '@tanstack/react-query';
import { ExternalLink } from 'lucide-react';
import { Card, CardContent } from '@/components/ui/card';
import { PageHeader } from '@/components/page-header';
import { Brand } from '@/components/layout/brand';
import { api } from '@/lib/api';

export function AboutPage() {
  const { t } = useTranslation();
  const { data: status } = useQuery({ queryKey: ['status'], queryFn: api.status });
  return (
    <>
      <PageHeader title={t('about.title')} />
      <Card>
        <CardContent className="space-y-4">
          <Brand />
          <p className="text-sm">{t('about.description')}</p>
          <p className="text-sm text-muted-foreground">{t('about.version')}：{status?.version ?? '—'}</p>
          <p className="text-sm text-muted-foreground">{t('about.license')}</p>
          <div className="flex flex-wrap gap-4 text-sm">
            <a className="inline-flex items-center gap-1 text-primary hover:underline" href="https://github.com/DiceZone/CatOfficial" target="_blank" rel="noreferrer">
              {t('about.repo')}<ExternalLink className="h-3.5 w-3.5" />
            </a>
            <a className="inline-flex items-center gap-1 text-primary hover:underline" href="https://github.com/botuniverse/onebot-11" target="_blank" rel="noreferrer">
              {t('about.onebot')}<ExternalLink className="h-3.5 w-3.5" />
            </a>
          </div>
        </CardContent>
      </Card>
    </>
  );
}
