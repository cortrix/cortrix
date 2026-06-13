import type { ReactNode } from 'react';
import { ErrorBoundary, type FallbackProps } from 'react-error-boundary';
import { ExclamationTriangleIcon, ArrowPathIcon, BookOpenIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';

// Global ErrorBoundary (P02a design § 16.3, § 16.4 "JS exception" placement).
// Catches React render-time exceptions that would otherwise blank the app and
// renders a recoverable fallback: message + reset button + a link to the
// troubleshooting docs. Built on `react-error-boundary`. Magma VI tokens only.
//
// This is the page-level error surface (§ 16.4: JS exception → ErrorBoundary);
// the other three placements are ErrorDisplay inline (Dialog), notify.error
// (Toast), and PrivateRoute redirect (route auth).

function ErrorFallback({ error, resetErrorBoundary }: FallbackProps) {
  const { t } = useTranslation();
  const message = error instanceof Error ? error.message : String(error);

  return (
    <div
      role="alert"
      data-testid="app-error-boundary"
      className="grid min-h-[60vh] place-items-center px-6"
    >
      <div className="w-full max-w-md rounded-2xl border border-line bg-surface p-8 text-center card-shadow">
        <span className="mx-auto grid h-14 w-14 place-items-center rounded-2xl border border-error/30 bg-error/10 text-error">
          <ExclamationTriangleIcon className="h-7 w-7" aria-hidden="true" />
        </span>
        <h1 className="mt-5 text-xl font-extrabold tracking-tight text-txt">
          {t('errorBoundary.title')}
        </h1>
        <p className="mt-2 text-sm text-muted">{t('errorBoundary.description')}</p>

        {message && (
          <pre className="mt-4 overflow-auto rounded-lg bg-codebg p-3 text-left font-mono text-xs text-txt">
            {message}
          </pre>
        )}

        <div className="mt-6 flex items-center justify-center gap-3">
          <button
            type="button"
            onClick={resetErrorBoundary}
            data-testid="error-boundary-reset"
            className="magma-grad inline-flex items-center gap-2 rounded-md px-4 py-2.5 text-sm font-semibold text-white shadow-lg shadow-magma/25 transition-all hover:brightness-110"
          >
            <ArrowPathIcon className="h-4 w-4" aria-hidden="true" />
            {t('errorBoundary.reset')}
          </button>
          <a
            href="/docs/troubleshooting"
            target="_blank"
            rel="noopener noreferrer"
            className="inline-flex items-center gap-2 rounded-md border border-line bg-surface px-4 py-2.5 text-sm font-medium text-txt transition-colors hover:border-magma/40"
          >
            <BookOpenIcon className="h-4 w-4" aria-hidden="true" />
            {t('errorBoundary.docs')}
          </a>
        </div>
      </div>
    </div>
  );
}

export function AppErrorBoundary({ children }: { children: ReactNode }) {
  return <ErrorBoundary FallbackComponent={ErrorFallback}>{children}</ErrorBoundary>;
}
