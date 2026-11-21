// 平台图标，图标文件取自 Dice!Next（public/platform-icons）。

import type { ImgHTMLAttributes } from 'react';
import { useTranslation } from 'react-i18next';
import { cn } from '@/lib/utils';

const ICONS: Record<string, string> = {
  official: './platform-icons/qq-official.svg',
  onebot: './platform-icons/onebot.svg',
};

type Props = Omit<ImgHTMLAttributes<HTMLImageElement>, 'src' | 'alt'> & { type: 'official' | 'onebot'; alt?: string };

export function PlatformIcon({ type, className, alt, title, ...props }: Props) {
  const { t } = useTranslation();
  const label = t(type === 'official' ? 'bots.type_official' : 'bots.type_onebot');
  return <img src={ICONS[type]} alt={alt ?? ''} title={title ?? label} draggable={false}
    className={cn('inline-block h-4 w-4 shrink-0 object-contain', className)} {...props} />;
}

/// 图标 + 平台名称，替代原来的纯文字徽章。
export function PlatformLabel({ type, className }: { type: 'official' | 'onebot'; className?: string }) {
  const { t } = useTranslation();
  return <span className={cn('inline-flex items-center gap-1.5 rounded-full border bg-background px-2 py-0.5 text-xs font-medium text-muted-foreground', className)}>
    <PlatformIcon type={type} title="" />
    {t(type === 'official' ? 'bots.type_official' : 'bots.type_onebot')}
  </span>;
}
