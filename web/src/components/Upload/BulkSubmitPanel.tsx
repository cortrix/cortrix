import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  RectangleStackIcon,
  CheckCircleIcon,
  ChevronDownIcon,
  ChevronRightIcon,
} from '@heroicons/react/24/outline';
import { useUploadStore } from '../../store/useUploadStore';
import { Button, Badge, Textarea } from '../ui';
import { ErrorDisplay } from '../Common/ErrorDisplay';
import { ProgrammaticBanner } from '../Common/ProgrammaticBanner';
import type { AgentError, BatchSubmitFailure } from '../../types/api';

// Bulk submit panel (web UI, batch submit). Submits a JSON array of
// documents (1–100) and renders the partial-success schema: a succeeded group
// (chips) + a failed group (each row an ErrorDisplay built from the per-doc
// GEN-Agent 5 fields). A BATCH-level error (empty / >100 / duplicate / invalid
// JSON) renders as a single ErrorDisplay above the form. Collapsible by default
// so it doesn't crowd the interactive single-file dropzone.

const SAMPLE = JSON.stringify(
  [
    { doc_id: 'doc_001', content: 'First document body…', metadata: { source: 'web' } },
    { doc_id: 'doc_002', content: 'Second document body…', metadata: { source: 'manual' } },
    { doc_id: 'doc_003', content: 'Third document body…' },
    { doc_id: 'doc_004', content: 'Fourth document body…' },
  ],
  null,
  2,
);

/** Build a GEN-Agent error from a per-doc failure so we reuse ErrorDisplay. */
function failureToError(f: BatchSubmitFailure): AgentError {
  return {
    code: f.error_code,
    message: `${f.doc_id}: ${f.error_code}`,
    retryable: f.retryable,
    category: f.category,
    retry_after_ms: f.retry_after_ms,
    structured_data: f.structured_data,
  };
}

export function BulkSubmitPanel() {
  const { t } = useTranslation();
  const [open, setOpen] = useState(false);
  const [json, setJson] = useState(SAMPLE);
  const { submitBatch, clearBatch, batchResult, batchError, batchSubmitting } = useUploadStore();

  const meta = batchResult?.meta;

  return (
    <section className="mt-8 rounded-xl border border-line bg-surface card-shadow" data-testid="bulk-submit-panel">
      <button
        type="button"
        onClick={() => setOpen((o) => !o)}
        aria-expanded={open}
        data-testid="bulk-submit-toggle"
        className="flex w-full items-center gap-2 px-5 py-3.5 text-left"
      >
        {open ? (
          <ChevronDownIcon className="h-4 w-4 text-muted" aria-hidden="true" />
        ) : (
          <ChevronRightIcon className="h-4 w-4 text-muted" aria-hidden="true" />
        )}
        <RectangleStackIcon className="h-5 w-5 text-magma-h" aria-hidden="true" />
        <span className="text-[15px] font-semibold text-txt">{t('bulk.title')}</span>
        <span className="ml-2 text-xs text-muted">{t('bulk.subtitle')}</span>
      </button>

      {open && (
        <div className="border-t border-line px-5 py-4 space-y-4">
          <ProgrammaticBanner
            comment={t('bulk.sdkComment')}
            snippet={
              <>
                <span className="text-magma-h">client</span>.<span className="text-amber">documents</span>.
                <span className="text-amber">batch_submit</span>(<span className="text-txt">documents</span>=[…])
              </>
            }
          />

          <div>
            <label htmlFor="bulk-json" className="mb-1.5 block text-sm font-medium text-txt">
              {t('bulk.jsonLabel')}
            </label>
            <Textarea
              id="bulk-json"
              value={json}
              onChange={(e) => setJson(e.target.value)}
              rows={8}
              data-testid="bulk-json-input"
              className="font-mono text-xs"
              spellCheck={false}
            />
            <p className="mt-1 text-xs text-muted">{t('bulk.jsonHint')}</p>
          </div>

          <div className="flex gap-3">
            <Button
              loading={batchSubmitting}
              leftIcon={<RectangleStackIcon className="h-4 w-4" />}
              onClick={() => void submitBatch(json)}
              data-testid="bulk-submit-btn"
            >
              {t('bulk.submit')}
            </Button>
            {(batchResult || batchError) && (
              <Button variant="secondary" onClick={clearBatch} data-testid="bulk-clear-btn">
                {t('bulk.clear')}
              </Button>
            )}
          </div>

          {/* BATCH-level error */}
          {batchError && <ErrorDisplay error={batchError} />}

          {/* Partial-success result */}
          {meta && (
            <div className="space-y-4" data-testid="bulk-result">
              <div className="flex flex-wrap items-center gap-3 text-sm">
                <Badge variant="ok">
                  {t('bulk.succeededCount', { count: meta.succeeded.length })}
                </Badge>
                {meta.failed.length > 0 && (
                  <Badge variant="warning">
                    {t('bulk.failedCount', { count: meta.failed.length })}
                  </Badge>
                )}
                <span className="font-mono text-xs text-muted">
                  {t('bulk.coverage', { pct: Math.round(meta.coverage_ratio * 100) })}
                </span>
              </div>

              {/* Succeeded group */}
              {meta.succeeded.length > 0 && (
                <div data-testid="bulk-succeeded">
                  <h3 className="mb-2 text-xs font-semibold uppercase tracking-wider text-ok">
                    {t('bulk.succeeded')}
                  </h3>
                  <div className="flex flex-wrap gap-2">
                    {batchResult!.results.map((r) => (
                      <span
                        key={r.doc_id}
                        data-doc-id={r.doc_id}
                        className="inline-flex items-center gap-1.5 rounded-md border border-ok/30 bg-ok/10 px-2.5 py-1 font-mono text-xs text-ok"
                      >
                        <CheckCircleIcon className="h-3.5 w-3.5" aria-hidden="true" />
                        {r.doc_id}
                      </span>
                    ))}
                  </div>
                </div>
              )}

              {/* Failed group */}
              {meta.failed.length > 0 && (
                <div data-testid="bulk-failed">
                  <h3 className="mb-2 text-xs font-semibold uppercase tracking-wider text-amber">
                    {t('bulk.failed')}
                  </h3>
                  <div className="space-y-2">
                    {meta.failed.map((f) => (
                      <ErrorDisplay key={f.doc_id} error={failureToError(f)} />
                    ))}
                  </div>
                </div>
              )}
            </div>
          )}
        </div>
      )}
    </section>
  );
}
