import { useEffect } from 'react';
import { Outlet } from 'react-router-dom';
import { Header } from './Header';
import { Sidebar } from './Sidebar';
import { useAppStore } from '../../store/useAppStore';
import { useFeatureFlagsStore } from '../../hooks/useFeatureFlags';
import { useAuthStore } from '../../store/useAuthStore';
import { usePolling } from '../../hooks/usePolling';
import { SYSTEM_POLL_MS } from '../../utils/constants';
import { ErrorMessage } from '../Common/ErrorMessage';
import { usePageViews } from '../../telemetry/usePageViews';
import { setMetricsEdition } from '../../telemetry/metrics';

// App shell (web UI design — react-router). Header + Sidebar frame an
// <Outlet/> that renders the active route. Page selection moved from the old
// `activePage` zustand field to the URL (BrowserRouter) in R3/S5; every R1/R2
// page (Upload / Search / Chat / Memory / Namespaces / Settings) is now a
// nested route under this layout (see src/routes.tsx), plus the new admin pages.
export function Layout() {
  const { loadNamespaces, loadSystemStatus, globalError, setGlobalError, theme } = useAppStore();
  const loadFeatures = useFeatureFlagsStore((s) => s.loadFeatures);
  const edition = useAuthStore((s) => s.edition);

  // Page-view metric per route change.
  usePageViews();

  // Tag session/page metrics with the resolved edition (labels).
  useEffect(() => {
    setMetricsEdition(edition);
  }, [edition]);

  // Initialize theme class on mount
  useEffect(() => {
    if (theme === 'dark') {
      document.documentElement.classList.add('dark');
    } else {
      document.documentElement.classList.remove('dark');
    }
  }, [theme]);

  // Load initial data
  useEffect(() => {
    loadNamespaces();
    loadFeatures();
  }, [loadNamespaces, loadFeatures]);

  // Poll system status
  usePolling(() => loadSystemStatus(), SYSTEM_POLL_MS);

  return (
    <div className="h-full flex flex-col bg-bg text-txt">
      <Header />
      <div className="flex flex-1 overflow-hidden">
        <Sidebar />
        <main className="flex-1 overflow-y-auto bg-bg dots">
          {globalError && (
            <div className="p-4">
              <ErrorMessage message={globalError} onDismiss={() => setGlobalError(null)} />
            </div>
          )}
          <Outlet />
        </main>
      </div>
    </div>
  );
}
