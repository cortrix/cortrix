import { useState } from 'react';
import { ChevronDownIcon, ChevronRightIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import type { SearchResult } from '../../types/api';

interface SourceCitationProps {
  sources: SearchResult[];
}

export function SourceCitation({ sources }: SourceCitationProps) {
  const [expanded, setExpanded] = useState(true);
  const { t } = useTranslation();

  if (!sources.length) return null;

  return (
    <div className="mt-3 pt-3 border-t border-line">
      <button
        onClick={() => setExpanded(!expanded)}
        className="flex items-center gap-1 text-xs text-muted mb-1.5"
      >
        {expanded ? <ChevronDownIcon className="w-3 h-3" /> : <ChevronRightIcon className="w-3 h-3" />}
        {t('chat.sourceCount', { count: sources.length })}
      </button>
      {expanded && (
        <div className="space-y-1.5">
          {sources.map((s) => (
            <div
              key={s.child_id}
              className="flex items-center gap-2 text-xs text-cortrix-600 dark:text-cortrix-400"
            >
              <span className="px-1.5 py-0.5 rounded text-[10px] bg-magma/10 text-magma-h shrink-0">
                {s.namespace}
              </span>
              <span className="truncate">{s.content}</span>
              <span className="text-muted ml-auto shrink-0">
                {s.score.toFixed(3)}
              </span>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
