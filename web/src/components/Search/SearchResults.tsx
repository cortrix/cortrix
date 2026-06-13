import { useTranslation } from 'react-i18next';
import { useSearchStore } from '../../store/useSearchStore';
import { ResultItem } from './ResultItem';

export function SearchResults() {
  const { results, meta } = useSearchStore();
  const { t } = useTranslation();

  if (!meta) return null;

  if (results.length === 0) {
    return (
      <div className="text-center py-12 text-muted">
        <p className="text-lg">{t('search.noResults')}</p>
        <p className="text-sm mt-1">{t('search.noResultsHint')}</p>
      </div>
    );
  }

  return (
    <div>
      <div className="flex items-center mb-4 text-sm text-muted">
        <strong className="text-txt">{results.length}</strong>
        &nbsp;{t('search.resultsSummary', { count: results.length, latency: meta.latency_ms }).replace(`${results.length}`, '').trim()}
        {meta.namespaces_queried.length > 1 && (
          <span className="ml-2 px-2 py-0.5 text-xs rounded bg-cortrix-100 text-cortrix-700 dark:bg-cortrix-800/15 dark:text-cortrix-400">
            {meta.namespaces_succeeded.length}/{meta.namespaces_queried.length} NS · cov {(meta.coverage_ratio * 100).toFixed(0)}%
          </span>
        )}
        {meta.namespaces_failed.length > 0 && (
          <span className="ml-2 px-2 py-0.5 text-xs rounded bg-amber/15 text-amber">
            {t('search.degraded')} ({meta.namespaces_failed.length})
          </span>
        )}
      </div>
      <div className="space-y-4">
        {results.map((r) => (
          <ResultItem key={r.child_id} result={r} />
        ))}
      </div>
    </div>
  );
}
