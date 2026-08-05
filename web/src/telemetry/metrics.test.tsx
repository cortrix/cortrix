import { describe, it, expect } from 'vitest';
import { render } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import {
  statusBucket,
  recordPageView,
  recordUiError,
  recordApiLatency,
  setMetricsEdition,
  initWebMetrics,
} from './metrics';
import { pageLabel, usePageViews } from './usePageViews';

// Web UI metrics unit tests (web UI design § 23-bis). The recording helpers are
// no-ops before initWebMetrics() (the SDK is not started in jsdom) — the test
// asserts they are total no-ops (no throw) and that the pure label/bucket
// helpers map correctly.

describe('telemetry metrics (§ 23-bis)', () => {
  it('statusBucket maps HTTP codes to coarse buckets', () => {
    expect(statusBucket(200)).toBe('2xx');
    expect(statusBucket(204)).toBe('2xx');
    expect(statusBucket(404)).toBe('4xx');
    expect(statusBucket(401)).toBe('4xx');
    expect(statusBucket(500)).toBe('5xx');
    expect(statusBucket(503)).toBe('5xx');
    expect(statusBucket(0)).toBe('2xx');
  });

  it('recording helpers are safe no-ops before init', () => {
    expect(() => {
      setMetricsEdition('cloud');
      recordPageView('home');
      recordUiError('quota', 'CX_ERR_NS_QUOTA_EXCEEDED', '/memory');
      recordApiLatency('/api/v1/health', '2xx', 'GET', 0.012);
    }).not.toThrow();
  });

  it('pageLabel normalizes pathnames to low-cardinality labels', () => {
    expect(pageLabel('/')).toBe('home');
    expect(pageLabel('')).toBe('home');
    expect(pageLabel('/namespaces')).toBe('ns');
    expect(pageLabel('/memory')).toBe('memory');
    expect(pageLabel('/chat')).toBe('chat');
    expect(pageLabel('/admin/users')).toBe('admin:users');
    expect(pageLabel('/ent/text-to-sql')).toBe('ent:text-to-sql');
  });

  it('initWebMetrics is idempotent and never throws', () => {
    expect(() => {
      initWebMetrics();
      initWebMetrics(); // second call is a no-op guard
    }).not.toThrow();
  });

  it('usePageViews records a page view on mount without throwing', () => {
    function Probe() {
      usePageViews();
      return null;
    }
    expect(() =>
      render(
        <MemoryRouter initialEntries={['/memory']}>
          <Probe />
        </MemoryRouter>,
      ),
    ).not.toThrow();
  });
});
