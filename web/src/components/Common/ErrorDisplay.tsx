import { useEffect, useState } from 'react';
import {
  LockClosedIcon,
  NoSymbolIcon,
  ClockIcon,
  ExclamationCircleIcon,
  BoltIcon,
} from '@heroicons/react/24/outline';
import type { ComponentType, SVGProps } from 'react';
import type { AgentError, AgentErrorCategory } from '../../types/api';
import { Button } from '../ui';
import { recordUiError } from '../../telemetry/metrics';

// GEN-Agent 5-field error display (P02a design § 16.1). Renders code / message /
// category badge / retry countdown (retry_after_ms) / collapsible
// structured_data. Category → variant + icon mapping per § 16.1. Magma VI
// tokens only (no hard-coded colours) so it works in both themes.

const CATEGORY_META: Record<
  AgentErrorCategory,
  { icon: ComponentType<SVGProps<SVGSVGElement>>; tone: string; label: string }
> = {
  auth: { icon: LockClosedIcon, tone: 'border-error/40 bg-error/5 text-error', label: 'Auth' },
  permanent: {
    icon: NoSymbolIcon,
    tone: 'border-error/40 bg-error/5 text-error',
    label: 'Permanent',
  },
  quota: {
    icon: ExclamationCircleIcon,
    tone: 'border-amber/40 bg-amber/10 text-amber',
    label: 'Quota',
  },
  transient: { icon: BoltIcon, tone: 'border-amber/40 bg-amber/10 text-amber', label: 'Transient' },
  timeout: { icon: ClockIcon, tone: 'border-amber/40 bg-amber/10 text-amber', label: 'Timeout' },
};

interface ErrorDisplayProps {
  error: AgentError;
  /** Shown when retryable; fires after the retry countdown elapses (if any). */
  onRetry?: () => void;
  className?: string;
}

export function ErrorDisplay({ error, onRetry, className = '' }: ErrorDisplayProps) {
  const meta = CATEGORY_META[error.category] ?? CATEGORY_META.permanent;
  const Icon = meta.icon;
  const [remainingMs, setRemainingMs] = useState(error.retry_after_ms ?? 0);

  useEffect(() => {
    setRemainingMs(error.retry_after_ms ?? 0);
    // § 23-bis.1 — count each surfaced business error (category/code labels
    // align with the CX_ERR_* family). Best-effort; no-op before metrics init.
    recordUiError(error.category, error.code, window.location.pathname);
  }, [error.retry_after_ms, error.code, error.message, error.category]);

  useEffect(() => {
    if (remainingMs <= 0) return;
    const id = setInterval(() => {
      setRemainingMs((ms) => Math.max(0, ms - 1000));
    }, 1000);
    return () => clearInterval(id);
  }, [remainingMs]);

  const showRetry = error.retryable && Boolean(onRetry);
  const countingDown = remainingMs > 0;

  return (
    <div
      role="alert"
      data-testid="error-display"
      data-error-code={error.code}
      className={`rounded-lg border px-4 py-3 ${meta.tone} ${className}`}
    >
      <div className="flex items-start gap-3">
        <Icon className="mt-0.5 h-5 w-5 shrink-0" aria-hidden="true" />
        <div className="min-w-0 flex-1">
          <div className="flex items-center gap-2">
            <span className="rounded-full bg-surface px-2 py-0.5 text-[10px] font-bold uppercase tracking-wide">
              {meta.label}
            </span>
            <code className="truncate font-mono text-xs opacity-90">{error.code}</code>
          </div>
          <p className="mt-1.5 text-sm text-txt">{error.message}</p>

          {error.structured_data && Object.keys(error.structured_data).length > 0 && (
            <details className="mt-2">
              <summary className="cursor-pointer text-xs font-medium text-muted hover:text-txt">
                Details
              </summary>
              <pre className="mt-1 overflow-auto rounded-md bg-codebg p-2 font-mono text-[11px] text-txt">
                {JSON.stringify(error.structured_data, null, 2)}
              </pre>
            </details>
          )}

          {showRetry && (
            <div className="mt-3">
              <Button
                size="sm"
                variant="secondary"
                disabled={countingDown}
                onClick={onRetry}
                data-testid="error-retry"
              >
                {countingDown ? `Retry in ${Math.ceil(remainingMs / 1000)}s` : 'Retry'}
              </Button>
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
