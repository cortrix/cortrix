import { useTranslation } from 'react-i18next';
import { useQuery } from '@tanstack/react-query';
import {
  CheckCircleIcon,
  ExclamationTriangleIcon,
  ArrowPathIcon,
  HeartIcon,
} from '@heroicons/react/24/outline';
import { getLive, getReady, type ComponentReadiness } from '../api/health';
import { Badge, Button } from '../components/ui';
import { LoadingSpinner } from '../components/Common/LoadingSpinner';
import { ProgrammaticBanner } from '../components/Common/ProgrammaticBanner';
import { ErrorDisplay } from '../components/Common/ErrorDisplay';
import { parseAgentError } from '../api/errors';

// Health dashboard (P02a design § 4.2, F20-7). Surfaces the two K8s probes:
//   /live  — process liveness (status + uptime + version)
//   /ready — readiness with the 5-component breakdown (F20-7 § 8.4)
// Agent-friendly: data-testid + data-component on each component row so an
// Agent can scrape readiness; a ProgrammaticBanner points at the SDK / REST
// equivalent (a human watches the dashboard, an Agent polls /ready).

const HEALTH_POLL_MS = 10_000;

// Component-specific extra fields we know how to label nicely; anything else is
// shown verbatim as key: value.
function detailEntries(c: ComponentReadiness): [string, string][] {
  return Object.entries(c)
    .filter(([k]) => k !== 'status')
    .map(([k, v]) => [k, typeof v === 'object' ? JSON.stringify(v) : String(v)]);
}

function uptime(seconds: number): string {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = Math.floor(seconds % 60);
  return `${h}h ${m}m ${s}s`;
}

export function HealthPage() {
  const { t } = useTranslation();

  const live = useQuery({
    queryKey: ['health-live'],
    queryFn: getLive,
    refetchInterval: HEALTH_POLL_MS,
  });
  const ready = useQuery({
    queryKey: ['health-ready'],
    queryFn: getReady,
    refetchInterval: HEALTH_POLL_MS,
  });

  const components = ready.data ? Object.entries(ready.data.components) : [];
  const allReady = ready.data?.status === 'ready';

  return (
    <section className="mx-auto max-w-4xl px-6 py-7 space-y-6" data-testid="health-page">
      <div className="flex items-end justify-between gap-4">
        <div>
          <p className="mb-1.5 text-xs font-semibold uppercase tracking-[3px] text-magma-h">{t('health.eyebrow')}</p>
          <h1 className="text-3xl font-extrabold tracking-tight text-txt">{t('health.title')}</h1>
          <p className="mt-1.5 text-sm text-muted">{t('health.subtitle')}</p>
        </div>
        <Button
          variant="secondary"
          leftIcon={<ArrowPathIcon className="h-4 w-4" />}
          onClick={() => {
            void live.refetch();
            void ready.refetch();
          }}
          data-testid="health-refresh-btn"
        >
          {t('common.refresh')}
        </Button>
      </div>

      <ProgrammaticBanner
        comment={t('health.sdkComment')}
        snippet={
          <>
            <span className="text-magma-h">curl</span>{' '}
            <span className="text-ok">/api/v1/system/health/ready</span>
          </>
        }
        channels="REST · K8s probe"
      />

      {/* Liveness */}
      <div className="rounded-xl border border-line bg-surface p-5 card-shadow" data-testid="health-live-card">
        <div className="flex items-center justify-between">
          <h2 className="flex items-center gap-2 text-[15px] font-semibold text-txt">
            <HeartIcon className="h-5 w-5 text-magma-h" aria-hidden="true" />
            {t('health.liveness')}
          </h2>
          {live.isLoading ? (
            <LoadingSpinner size="sm" />
          ) : live.isError ? (
            <Badge variant="error">{t('health.down')}</Badge>
          ) : (
            <Badge variant="ok">{t('health.alive')}</Badge>
          )}
        </div>
        {live.data && (
          <dl className="mt-3 grid grid-cols-2 gap-3 text-sm sm:grid-cols-3">
            <div>
              <dt className="text-xs text-muted">{t('health.status')}</dt>
              <dd className="font-mono text-txt" data-testid="live-status">{live.data.status}</dd>
            </div>
            <div>
              <dt className="text-xs text-muted">{t('health.uptime')}</dt>
              <dd className="font-mono text-txt">{uptime(live.data.uptime_seconds)}</dd>
            </div>
            <div>
              <dt className="text-xs text-muted">{t('health.version')}</dt>
              <dd className="font-mono text-txt">v{live.data.version}</dd>
            </div>
          </dl>
        )}
      </div>

      {/* Readiness */}
      <div className="rounded-xl border border-line bg-surface p-5 card-shadow" data-testid="health-ready-card">
        <div className="flex items-center justify-between">
          <h2 className="text-[15px] font-semibold text-txt">{t('health.readiness')}</h2>
          {ready.isLoading ? (
            <LoadingSpinner size="sm" />
          ) : ready.isError ? (
            <Badge variant="error">{t('health.down')}</Badge>
          ) : (
            <Badge variant={allReady ? 'ok' : 'warning'} data-testid="ready-status-badge">
              {allReady ? t('health.ready') : t('health.notReady')}
            </Badge>
          )}
        </div>

        {ready.isError ? (
          <div className="mt-4">
            <ErrorDisplay error={parseAgentError(ready.error)} onRetry={() => void ready.refetch()} />
          </div>
        ) : (
          <ul className="mt-4 space-y-2" data-testid="health-components">
            {components.map(([name, c]) => {
              const ok = c.status === 'ok';
              return (
                <li
                  key={name}
                  data-testid="health-component"
                  data-component={name}
                  data-component-status={c.status}
                  className="flex items-start gap-3 rounded-lg border border-line bg-bg px-4 py-3"
                >
                  {ok ? (
                    <CheckCircleIcon className="mt-0.5 h-5 w-5 shrink-0 text-ok" aria-hidden="true" />
                  ) : (
                    <ExclamationTriangleIcon className="mt-0.5 h-5 w-5 shrink-0 text-amber" aria-hidden="true" />
                  )}
                  <div className="min-w-0 flex-1">
                    <div className="flex items-center gap-2">
                      <span className="font-mono text-sm font-semibold text-txt">{name}</span>
                      <Badge variant={ok ? 'ok' : 'warning'}>{c.status}</Badge>
                    </div>
                    {detailEntries(c).length > 0 && (
                      <div className="mt-1 flex flex-wrap gap-x-4 gap-y-0.5 font-mono text-xs text-muted">
                        {detailEntries(c).map(([k, v]) => (
                          <span key={k}>
                            {k}: <span className="text-txt">{v}</span>
                          </span>
                        ))}
                      </div>
                    )}
                  </div>
                </li>
              );
            })}
          </ul>
        )}

        {ready.data?.warnings && ready.data.warnings.length > 0 && (
          <div className="mt-4 rounded-lg border border-amber/40 bg-amber/10 px-4 py-3 text-sm text-amber">
            <ul className="space-y-1">
              {ready.data.warnings.map((w, i) => (
                <li key={i}>{w}</li>
              ))}
            </ul>
          </div>
        )}
      </div>
    </section>
  );
}
