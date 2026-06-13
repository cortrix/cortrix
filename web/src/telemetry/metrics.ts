import { metrics, type Counter, type Histogram } from '@opentelemetry/api';

// Web UI observability — OpenTelemetry metrics (P02a design § 23-bis).
//
// The 4 V1.0 minimal-set metrics (§ 23-bis.1) are emitted client-side and
// pushed via OTLP/HTTP to cortrix-server's /v1/metrics reverse proxy
// (§ 23-bis.2 — same-origin, so CSP connect-src stays 'self'), which forwards
// to the collector → Prometheus :9091. Backend correlates client-perceived vs
// server-side latency (§ 23-bis.3).
//
//   cortrix_webui_page_views_total       Counter   (page, edition)
//   cortrix_webui_session_duration_secs  Histogram (edition)
//   cortrix_webui_error_total            Counter   (category, code, page)
//   cortrix_webui_api_latency_seconds    Histogram (endpoint, status, method)
//
// Standalone discipline (D3): exporting is best-effort. With no collector the
// OTLP POSTs simply fail and are dropped — initialisation never throws and the
// app runs unaffected. The instruments are created lazily and memoised so the
// recording helpers are cheap no-ops before init / in tests.

const METRIC_PATH = '/v1/metrics';
const METER_NAME = 'cortrix-webui';

let initialized = false;
let pageViews: Counter | undefined;
let sessionDuration: Histogram | undefined;
let errorTotal: Counter | undefined;
let apiLatency: Histogram | undefined;
let sessionStart = 0;

/** Resolve the build version stamped at compile time (§ 23-bis.2 service.version). */
function serviceVersion(): string {
  // Vite replaces import.meta.env at build; fall back to package version.
  const env = (import.meta as unknown as { env?: Record<string, string> }).env;
  return env?.VITE_APP_VERSION ?? '1.0.0';
}

function deploymentEnv(): string {
  const env = (import.meta as unknown as { env?: Record<string, string> }).env;
  if (env?.PROD) return 'production';
  if (env?.DEV) return 'development';
  return 'unknown';
}

/**
 * Initialise the Web UI MeterProvider + OTLP exporter and create the 4
 * instruments. Idempotent and safe to call once at app start. No-op in
 * non-browser (test/SSR) environments.
 *
 * Perf (P02a § 14): the heavy OTel SDK (MeterProvider / OTLP exporter, ~19KB
 * gzip) is dynamic-imported here so it lands in its own async chunk and never
 * blocks first paint / inflates the initial bundle — only the tiny
 * @opentelemetry/api no-op surface ships in the entry. The recording helpers
 * below stay synchronous and are no-ops until this resolves.
 */
export function initWebMetrics(): void {
  if (initialized) return;
  if (typeof window === 'undefined') return;
  initialized = true;

  void (async () => {
    try {
      const [
        { MeterProvider, PeriodicExportingMetricReader },
        { OTLPMetricExporter },
        { resourceFromAttributes },
        { ATTR_SERVICE_NAME, ATTR_SERVICE_VERSION },
      ] = await Promise.all([
        import('@opentelemetry/sdk-metrics'),
        import('@opentelemetry/exporter-metrics-otlp-http'),
        import('@opentelemetry/resources'),
        import('@opentelemetry/semantic-conventions'),
      ]);

      const exporter = new OTLPMetricExporter({ url: METRIC_PATH });
      const reader = new PeriodicExportingMetricReader({
        exporter,
        exportIntervalMillis: 30_000,
      });
      const provider = new MeterProvider({
        // § 23-bis.2 resource attributes.
        resource: resourceFromAttributes({
          [ATTR_SERVICE_NAME]: 'cortrix-webui',
          [ATTR_SERVICE_VERSION]: serviceVersion(),
          'deployment.environment': deploymentEnv(),
        }),
        readers: [reader],
      });
      metrics.setGlobalMeterProvider(provider);

      const meter = metrics.getMeter(METER_NAME);
      pageViews = meter.createCounter('cortrix_webui_page_views_total', {
        description: 'Web UI page view count',
      });
      sessionDuration = meter.createHistogram('cortrix_webui_session_duration_seconds', {
        description: 'User session duration (first load → unload)',
        unit: 's',
      });
      errorTotal = meter.createCounter('cortrix_webui_error_total', {
        description: 'Business errors surfaced in the UI (ErrorDisplay)',
      });
      apiLatency = meter.createHistogram('cortrix_webui_api_latency_seconds', {
        description: 'Client-side fetch latency to /api/v1/*',
        unit: 's',
      });

      // § 23-bis.1 session_duration — measure first-load → unload.
      sessionStart = performance.now();
      window.addEventListener('pagehide', flushSession, { once: true });
    } catch {
      // Collector unreachable / SDK init issue → telemetry disabled, app unaffected.
    }
  })();
}

function flushSession() {
  if (!sessionDuration || !sessionStart) return;
  const edition = currentEdition();
  sessionDuration.record((performance.now() - sessionStart) / 1000, { edition });
}

// Edition tag is read lazily so we don't import the auth store at module load
// (avoids a cycle); callers that know the edition can pass it explicitly.
let editionTag = 'ce';
/** Set the edition label applied to metrics (called once the session resolves). */
export function setMetricsEdition(edition: string): void {
  editionTag = edition;
}
function currentEdition(): string {
  return editionTag;
}

/** Record a page view (§ 23-bis.1). Safe before init. */
export function recordPageView(page: string): void {
  pageViews?.add(1, { page, edition: currentEdition() });
}

/** Record a UI-surfaced business error (§ 23-bis.1 — ErrorDisplay trigger). */
export function recordUiError(category: string, code: string, page: string): void {
  errorTotal?.add(1, { category, code, page });
}

/** Record a client-side API latency sample in seconds (§ 23-bis.1). */
export function recordApiLatency(
  endpoint: string,
  status: '2xx' | '4xx' | '5xx',
  method: string,
  seconds: number,
): void {
  apiLatency?.record(seconds, { endpoint, status, method });
}

/** Map an HTTP status code to the coarse status bucket label. */
export function statusBucket(status: number): '2xx' | '4xx' | '5xx' {
  if (status >= 500) return '5xx';
  if (status >= 400) return '4xx';
  return '2xx';
}
