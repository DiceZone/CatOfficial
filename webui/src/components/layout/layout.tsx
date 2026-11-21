import React from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import * as DialogPrimitive from '@radix-ui/react-dialog';
import { Activity, Bot, Images, Info, Languages, Link2, LogOut, Menu, MessageSquareReply, MessageSquareText, MessagesSquare, Moon, PlugZap, RotateCw, ScrollText, SlidersHorizontal, Sun, X } from 'lucide-react';
import { toast } from 'sonner';
import { Button } from '@/components/ui/button';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { cn } from '@/lib/utils';
import { api } from '@/lib/api';
import { LANGUAGES, setLanguage } from '@/i18n';
import { Brand } from './brand';

type Icon = React.ComponentType<{ className?: string }>;
type Section = 'run' | 'manage' | 'system';
interface NavItem { path: string; labelKey: string; icon: Icon; section: Section }

const NAV: NavItem[] = [
  { path: '/', labelKey: 'nav.dashboard', icon: Activity, section: 'run' },
  { path: '/chat', labelKey: 'nav.chat', icon: MessagesSquare, section: 'run' },
  { path: '/messages', labelKey: 'nav.messages', icon: MessageSquareText, section: 'run' },
  { path: '/logs', labelKey: 'nav.logs', icon: ScrollText, section: 'run' },
  { path: '/bots', labelKey: 'nav.bots', icon: Bot, section: 'manage' },
  { path: '/connections', labelKey: 'nav.connections', icon: PlugZap, section: 'manage' },
  { path: '/replies', labelKey: 'nav.replies', icon: MessageSquareReply, section: 'manage' },
  { path: '/materials', labelKey: 'nav.materials', icon: Images, section: 'manage' },
  { path: '/bindings', labelKey: 'nav.bindings', icon: Link2, section: 'manage' },
  { path: '/settings', labelKey: 'nav.settings', icon: SlidersHorizontal, section: 'system' },
  { path: '/about', labelKey: 'nav.about', icon: Info, section: 'system' },
];
const SECTIONS: Section[] = ['run', 'manage', 'system'];

interface LayoutProps { children: React.ReactNode; currentPath: string; onNavigate: (path: string) => void }

export function Layout({ children, currentPath, onNavigate }: LayoutProps) {
  const { t } = useTranslation();
  const [menuOpen, setMenuOpen] = React.useState(false);
  const go = (path: string) => { onNavigate(path); setMenuOpen(false); };

  const navList = (mobile: boolean) => SECTIONS.map((section) => (
    <div key={section}>
      <p className="v2-section-label">{t(`nav.section_${section}`)}</p>
      <div className="space-y-0.5">
        {NAV.filter((item) => item.section === section).map((item) => (
          <button key={item.path} onClick={() => go(item.path)} aria-current={item.path === currentPath ? 'page' : undefined}
            className={cn('v2-nav-item flex w-full items-center gap-3 px-3 py-2 text-left text-muted-foreground transition-colors hover:bg-accent hover:text-accent-foreground focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-inset focus-visible:ring-ring',
              mobile ? 'text-sm' : 'text-[13px]')}>
            <item.icon className="h-[18px] w-[18px] shrink-0" />
            <span className="truncate">{t(item.labelKey)}</span>
          </button>
        ))}
      </div>
    </div>
  ));

  return (
    <div className="v2-shell flex h-dvh overflow-hidden bg-background">
      <aside className="relative z-40 hidden h-full w-[232px] shrink-0 flex-col border-r bg-card lg:flex">
        <div className="flex h-16 shrink-0 items-center px-4"><Brand /></div>
        <nav aria-label={t('header.menu')} className="min-h-0 flex-1 overflow-y-auto px-3 pb-3">{navList(false)}</nav>
      </aside>
      <DialogPrimitive.Root open={menuOpen} onOpenChange={setMenuOpen}>
        <DialogPrimitive.Portal>
          <DialogPrimitive.Overlay className="fixed inset-0 z-40 bg-black/45" />
          <DialogPrimitive.Content aria-describedby={undefined} className="v2-mobile-menu fixed inset-0 z-50 flex h-dvh w-full flex-col bg-card outline-none">
            <div className="flex shrink-0 items-center justify-between border-b px-5 py-3">
              <DialogPrimitive.Title><Brand /></DialogPrimitive.Title>
              <DialogPrimitive.Close asChild><Button variant="ghost" size="icon" aria-label={t('common.close')}><X className="h-5 w-5" /></Button></DialogPrimitive.Close>
            </div>
            <nav className="min-h-0 flex-1 overflow-y-auto px-4 pb-6">{navList(true)}</nav>
          </DialogPrimitive.Content>
        </DialogPrimitive.Portal>
      </DialogPrimitive.Root>
      <div className="flex flex-1 flex-col overflow-hidden">
        <Header currentPath={currentPath} onMenu={() => setMenuOpen(true)} />
        <RestartBanner />
        <main className="v2-main flex-1 overflow-y-auto overflow-x-hidden p-4 sm:p-6 xl:p-9">
          <div className="v2-workspace space-y-6">{children}</div>
        </main>
      </div>
    </div>
  );
}

/// 配置改动后提示重启。重启期间轮询会话，恢复后刷新页面数据。
function RestartBanner() {
  const { t } = useTranslation();
  const client = useQueryClient();
  const { data } = useQuery({ queryKey: ['status'], queryFn: api.status, refetchInterval: 5000 });
  const [running, setRunning] = React.useState(false);
  if (!data?.pending_restart && !running) return null;
  const restart = async () => {
    setRunning(true);
    try {
      await api.restart();
      for (let i = 0; i < 40; i++) {
        await new Promise((r) => setTimeout(r, 1000));
        try { await api.status(); break; } catch { /* 还没起来 */ }
      }
      await client.invalidateQueries();
      toast.success(t('restart.done'));
    } catch (e) {
      toast.error(t('common.failed', { error: e instanceof Error ? e.message : String(e) }));
    } finally {
      setRunning(false);
    }
  };
  return (
    <div className="flex shrink-0 flex-wrap items-center justify-between gap-3 border-b border-amber-500/30 bg-amber-500/10 px-4 py-2 text-sm sm:px-6">
      <span>{t('restart.pending')}</span>
      <Button size="sm" variant="outline" disabled={running} onClick={restart}>
        <RotateCw className={cn('mr-2 h-4 w-4', running && 'animate-spin')} />{t(running ? 'restart.running' : 'restart.now')}
      </Button>
    </div>
  );
}

function Header({ currentPath, onMenu }: { currentPath: string; onMenu: () => void }) {
  const { t, i18n } = useTranslation();
  const client = useQueryClient();
  const [dark, setDark] = React.useState(() => document.documentElement.classList.contains('dark'));
  const current = NAV.find((item) => item.path === currentPath);
  const toggleTheme = () => {
    const next = !dark;
    setDark(next);
    document.documentElement.classList.toggle('dark', next);
    try { localStorage.setItem('catofficial.theme', next ? 'dark' : 'light'); } catch { /* 忽略 */ }
  };
  const logout = async () => {
    try { await api.logout(); } finally { client.setQueryData(['session'], { authenticated: false }); }
  };
  return (
    <header className="flex h-16 shrink-0 items-center gap-2 border-b bg-card px-4 sm:px-6">
      <Button variant="ghost" size="icon" className="lg:hidden" aria-label={t('header.menu')} onClick={onMenu}><Menu className="h-5 w-5" /></Button>
      <div className="lg:hidden"><Brand compact /></div>
      <span className="min-w-0 flex-1 truncate text-sm font-medium text-muted-foreground">{current ? t(current.labelKey) : ''}</span>
      <Select value={i18n.language} onValueChange={setLanguage}>
        <SelectTrigger className="h-9 w-auto gap-2 border-0 bg-transparent px-2 shadow-none" aria-label={t('header.language')}>
          <Languages className="h-4 w-4" /><span className="hidden sm:inline"><SelectValue /></span>
        </SelectTrigger>
        <SelectContent>{LANGUAGES.map((l) => <SelectItem key={l.code} value={l.code}>{l.label}</SelectItem>)}</SelectContent>
      </Select>
      <Button variant="ghost" size="icon" aria-label={t('header.theme')} title={t('header.theme')} onClick={toggleTheme}>
        {dark ? <Sun className="h-[18px] w-[18px]" /> : <Moon className="h-[18px] w-[18px]" />}
      </Button>
      <Button variant="ghost" size="icon" aria-label={t('header.logout')} title={t('header.logout')} onClick={logout}><LogOut className="h-[18px] w-[18px]" /></Button>
    </header>
  );
}
