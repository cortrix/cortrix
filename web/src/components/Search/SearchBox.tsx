import { FormEvent } from 'react';
import { MagnifyingGlassIcon as SearchIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import { useSearchStore } from '../../store/useSearchStore';
import { LoadingSpinner } from '../Common/LoadingSpinner';

export function SearchBox() {
  const { query, setQuery, filters, setFilters, search, isSearching } = useSearchStore();
  const { t } = useTranslation();

  const handleSubmit = (e: FormEvent) => {
    e.preventDefault();
    search();
  };

  return (
    <form onSubmit={handleSubmit} className="rounded-xl shadow-sm p-4 mb-6 bg-surface border border-line ">
      <div className="flex gap-3">
        <div className="flex-1 relative">
          <SearchIcon className="absolute left-3 top-3 w-5 h-5 text-muted" />
          <input
            type="text"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            placeholder={t('search.placeholder')}
            aria-label={t('search.placeholder')}
            data-testid="search-input"
            className="w-full pl-10 pr-4 py-2.5 rounded-lg text-sm outline-none border border-line focus:ring-2 focus:ring-magma/20 focus:border-magma bg-surface text-txt placeholder:text-muted"
          />
        </div>
        <button
          type="submit"
          disabled={isSearching || !query.trim()}
          data-testid="search-submit-btn"
          className="px-6 py-2.5 rounded-lg text-sm font-medium transition-colors bg-cortrix-600 text-white hover:bg-cortrix-700 disabled:opacity-50 disabled:cursor-not-allowed flex items-center gap-2"
        >
          {isSearching ? <LoadingSpinner size="sm" /> : null}
          {isSearching ? t('search.searching') : t('search.searchButton')}
        </button>
      </div>
      <div className="flex gap-3 mt-3">
        <select
          value={filters.blockType.length === 1 ? filters.blockType[0] : ''}
          onChange={(e) => setFilters({ blockType: e.target.value ? [e.target.value] : [] })}
          className="text-xs px-3 py-1.5 rounded-md border border-line bg-surface text-muted "
        >
          <option value="">{t('search.allTypes')}</option>
          <option value="FILE">FILE</option>
          <option value="DATABASE">DATABASE</option>
          <option value="MEMORY">MEMORY</option>
        </select>
        <select
          value={filters.topK}
          onChange={(e) => setFilters({ topK: Number(e.target.value) })}
          className="text-xs px-3 py-1.5 rounded-md border border-line bg-surface text-muted "
        >
          <option value={10}>Top 10</option>
          <option value={20}>Top 20</option>
          <option value={50}>Top 50</option>
        </select>
      </div>
    </form>
  );
}
