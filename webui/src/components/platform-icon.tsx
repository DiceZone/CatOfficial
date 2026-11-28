// 平台图标，图标文件取自 Dice!Next（public/platform-icons）。

import React, { type ImgHTMLAttributes } from 'react';
import { useTranslation } from 'react-i18next';
import { qqPortrait } from '@/lib/api';
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

/// 机器人头像：平台资料 → 真实 QQ 号的普通头像 → 平台图标。
export function BotAvatar({ src, uin, type, className }: { src?: string; uin?: string | number; type: 'official' | 'onebot'; className?: string }) {
  const qq = qqPortrait(uin);
  const urls = [src, qq].filter((u, i, all): u is string => !!u && all.indexOf(u) === i);
  const [i, setI] = React.useState(0);
  React.useEffect(() => { setI(0); }, [src, qq]);
  if (i < urls.length) {
    return <img src={urls[i]} alt="" referrerPolicy="no-referrer" onError={() => setI((n) => n + 1)}
      className={cn('shrink-0 rounded-full bg-muted object-cover', className)} />;
  }
  return (
    <div className={cn('flex shrink-0 items-center justify-center rounded-full bg-primary/10', className)}>
      <PlatformIcon type={type} className="h-1/2 w-1/2" />
    </div>
  );
}

/// 图标 + 平台名称，替代原来的纯文字徽章。
export function PlatformLabel({ type, className }: { type: 'official' | 'onebot'; className?: string }) {
  const { t } = useTranslation();
  return <span className={cn('inline-flex items-center gap-1.5 rounded-full border bg-background px-2 py-0.5 text-xs font-medium text-muted-foreground', className)}>
    <PlatformIcon type={type} title="" />
    {t(type === 'official' ? 'bots.type_official' : 'bots.type_onebot')}
  </span>;
}
