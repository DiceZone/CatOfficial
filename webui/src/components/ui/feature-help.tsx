// 标题旁的问号：点开看说明（Dice!Next 同名组件的简化版，不带悬停提示）。

import type { ReactNode } from 'react';
import { useTranslation } from 'react-i18next';
import { HelpCircle } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Dialog, DialogClose, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle, DialogTrigger } from '@/components/ui/dialog';

export function FeatureHelp({ title, description }: { title: string; description: ReactNode }) {
  const { t } = useTranslation();
  return (
    <Dialog>
      <DialogTrigger asChild>
        <button type="button" aria-label={t('common.help_about', { title })}
          className="inline-flex h-6 w-6 shrink-0 items-center justify-center rounded-full text-muted-foreground transition-colors hover:text-foreground focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring">
          <HelpCircle className="h-4 w-4" aria-hidden="true" />
        </button>
      </DialogTrigger>
      <DialogContent className="max-h-[85vh] overflow-y-auto">
        <DialogHeader>
          <DialogTitle className="flex items-center gap-2 pr-6 text-left leading-snug">
            <HelpCircle className="h-5 w-5 shrink-0 text-muted-foreground" aria-hidden="true" />
            <span className="min-w-0 break-words">{title}</span>
          </DialogTitle>
        </DialogHeader>
        <DialogDescription asChild><div className="space-y-3 break-words leading-relaxed">{description}</div></DialogDescription>
        <DialogFooter><DialogClose asChild><Button variant="outline">{t('common.close')}</Button></DialogClose></DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
