import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import App from './App';
import { ToastContainer } from './components/ui';
import { AppErrorBoundary } from './components/Common/AppErrorBoundary';
import { initWebMetrics } from './telemetry/metrics';
import './i18n';
import './index.css';

// Web UI observability (web UI) — start the OpenTelemetry metrics
// pipeline. Best-effort: a no-op when no collector is reachable (standalone).
initWebMetrics();

// TanStack Query client (web UI design § 3.2). Conservative defaults: no
// refetch-on-focus churn, one retry for transient failures, 30s stale window.
const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      // One retry for transient (5xx / network) failures, but never retry a 4xx:
      // a client error (e.g. AdminGuard 403 over a published port, a 400 validation)
      // won't succeed on a retry, so surface it immediately instead of leaving the
      // page on a loading spinner through the retry/backoff window.
      retry: (failureCount, error) => {
        const status = (error as { status?: number })?.status;
        if (typeof status === 'number' && status >= 400 && status < 500) return false;
        return failureCount < 1;
      },
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
