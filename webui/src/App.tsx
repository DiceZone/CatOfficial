import React from 'react';
import { QueryClient, QueryClientProvider, useQuery, useQueryClient } from '@tanstack/react-query';
import { Toaster } from 'sonner';
import { api, setUnauthorizedHandler } from '@/lib/api';
import { Layout } from '@/components/layout/layout';
import { ChangePasswordPage, LoginPage } from '@/pages/login-page';
import { DashboardPage } from '@/pages/dashboard-page';
import { MessagesPage } from '@/pages/messages-page';
import { BindingsPage } from '@/pages/bindings-page';
import { SettingsPage } from '@/pages/settings-page';
import { BotsPage } from '@/pages/bots-page';
import { ConnectionsPage } from '@/pages/connections-page';
import { LogsPage } from '@/pages/logs-page';
import { AboutPage } from '@/pages/about-page';
import { ChatPage } from '@/pages/chat-page';
import { RepliesPage } from '@/pages/replies-page';
import { MaterialsPage } from '@/pages/materials-page';

const queryClient = new QueryClient({ defaultOptions: { queries: { retry: 1, refetchOnWindowFocus: false } } });

const PAGES: Record<string, React.FC> = {
  '/': DashboardPage,
  '/bots': BotsPage,
  '/connections': ConnectionsPage,
  '/chat': ChatPage,
  '/messages': MessagesPage,
  '/replies': RepliesPage,
  '/materials': MaterialsPage,
  '/logs': LogsPage,
  '/bindings': BindingsPage,
  '/settings': SettingsPage,
  '/about': AboutPage,
};

function readPath(): string {
  const path = window.location.hash.replace(/^#/, '') || '/';
  return path in PAGES ? path : '/';
}

function Shell() {
  const client = useQueryClient();
  const session = useQuery({ queryKey: ['session'], queryFn: api.session });
  const [path, setPath] = React.useState(readPath);

  React.useEffect(() => {
    const update = () => setPath(readPath());
    window.addEventListener('hashchange', update);
    return () => window.removeEventListener('hashchange', update);
  }, []);
  React.useEffect(() => {
    setUnauthorizedHandler((next) => client.setQueryData(['session'], next));
  }, [client]);

  if (session.isLoading) return null;
  if (!session.data?.authenticated) return <LoginPage />;
  if (session.data.must_change_password) return <ChangePasswordPage />;
  const Page = PAGES[path];
  return (
    <Layout currentPath={path} onNavigate={(next) => { window.location.hash = next; }}>
      <Page />
    </Layout>
  );
}

export function App() {
  return (
    <QueryClientProvider client={queryClient}>
      <Shell />
      <Toaster richColors position="top-right" />
    </QueryClientProvider>
  );
}
