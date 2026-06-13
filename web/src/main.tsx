import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import App from './App';
import { ToastContainer } from './components/ui';
import { AppErrorBoundary } from './components/Common/AppErrorBoundary';
import { initWebMetrics } from './telemetry/metrics';
import './i18n';
import './index.css';

// Web UI observability (P02a § 23-bis) — start the OpenTelemetry metrics
// pipeline. Best-effort: a no-op when no collector is reachable (standalone).
initWebMetrics();

// TanStack Query client (P02a design § 3.2). Conservative defaults: no
// refetch-on-focus churn, one retry for transient failures, 30s stale window.
const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      retry: 1,
      refetchOnWindowFocus: false,
      staleTime: 30_000,
    },
  },
});

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <AppErrorBoundary>
      <QueryClientProvider client={queryClient}>
        <App />
        <ToastContainer />
      </QueryClientProvider>
    </AppErrorBoundary>
  </StrictMode>,
);
