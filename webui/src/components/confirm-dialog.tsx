// 确认框：const { confirm, node } = useConfirm(); if (await confirm({...})) …；把 node 放进页面。

import React from 'react';
import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';

interface Options { title: string; description: string; confirmLabel?: string; destructive?: boolean }

export function useConfirm() {
  const { t } = useTranslation();
  const [pending, setPending] = React.useState<(Options & { resolve: (ok: boolean) => void }) | null>(null);
  const confirm = React.useCallback((options: Options) => new Promise<boolean>((resolve) => setPending({ ...options, resolve })), []);
  const close = (ok: boolean) => { pending?.resolve(ok); setPending(null); };
  const node = (
    <Dialog open={!!pending} onOpenChange={(open) => { if (!open) close(false); }}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>{pending?.title}</DialogTitle>
          <DialogDescription>{pending?.description}</DialogDescription>
        </DialogHeader>
        <DialogFooter>
          <Button variant="outline" onClick={() => close(false)}>{t('common.cancel')}</Button>
          <Button variant={pending?.destructive ? 'destructive' : 'default'} onClick={() => close(true)}>{pending?.confirmLabel ?? t('common.confirm')}</Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
  return { confirm, node };
}
