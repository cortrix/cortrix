import { useTranslation } from 'react-i18next';
import { SearchBox } from './SearchBox';
import { SearchResults } from './SearchResults';
import { ProgrammaticBanner } from '../Common/ProgrammaticBanner';
import { ErrorDisplay } from '../Common/ErrorDisplay';
import { useSearchStore } from '../../store/useSearchStore';

export function SearchPage() {
  const { t } = useTranslation();
  const error = useSearchStore((s) => s.error);
  const search = useSearchStore((s) => s.search);

  return (
    <div className="p-6 max-w-4xl mx-auto">
      <div className="mb-6">
        <h1 className="text-2xl font-extrabold tracking-tight text-txt">{t('search.title')}</h1>
        <p className="text-sm mt-1 text-muted">
          {t('search.subtitle')}
        </p>
      </div>

      <ProgrammaticBanner
        comment={t('search.sdkComment')}
        snippet={
          <>
            <span className="text-magma-h">client</span>.<span className="text-amber">query</span>(
            <span className="text-ok">&quot;…&quot;</span>, <span className="text-txt">top_k</span>=
            <span className="text-ok">10</span>)
          </>
        }
        className="mb-6"
      />

      <SearchBox />

      {/* Inline structured error (§ 16.4 inline placement) */}
      {error && (
        <div className="mb-6">
          <ErrorDisplay error={error} onRetry={() => void search()} />
        </div>
      )}

      <SearchResults />
    </div>
  );
}
