import { ExclamationTriangleIcon, PencilIcon, TrashIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import type { MemoryItem } from '../../types/api';
import { Badge, Button } from '../ui';
import { formatEpoch } from '../../utils/formatters';
import { ExtractionMethodBadge } from './ExtractionMethodBadge';

// One memory record card (P02a design § 7.1). Shows A-class fields by default;
// B-class provenance (§ 7.3 explain mode) is revealed when `explain` is on.
// `revoked_at` is surfaced as a transparency badge (§ 7.4 — admin revoke
// history). Invalidate = soft delete (§ 7.2), never a hard delete.

interface MemoryCardProps {
  memory: MemoryItem;
  explain: boolean;
  onEdit: (m: MemoryItem) => void;
  onInvalidate: (m: MemoryItem) => void;
}

const TYPE_VARIANT = {
  fact: 'magma',
  preference: 'amber',
  event: 'ok',
} as const;

export function MemoryCard({ memory, explain, onEdit, onInvalidate }: MemoryCardProps) {
  const { t } = useTranslation();
  const invalidated = memory.status === 'invalidated';

  return (
    <article
      data-testid="memory-card"
      data-memory-id={memory.memory_id}
      className={[
        'rounded-xl border bg-surface card-shadow p-4 transition-colors',
        invalidated ? 'border-line opacity-70' : 'border-line hover:border-magma/40',
      ].join(' ')}
    >
      <div className="flex items-start justify-between gap-3">
        <div className="min-w-0 flex-1">
          <div className="flex flex-wrap items-center gap-2">
            <Badge variant={TYPE_VARIANT[memory.memory_type]}>{memory.memory_type}</Badge>
            {invalidated ? (
              <Badge variant="error">invalidated</Badge>
            ) : (
              <Badge variant="ok">{memory.status}</Badge>
            )}
            {memory.revoked_at != null && (
              <span
                className="inline-flex items-center gap-1 rounded-full bg-amber/15 px-2 py-0.5 text-xs font-medium text-amber"
                data-testid="memory-revoked-badge"
              >
                <ExclamationTriangleIcon className="h-4 w-4" aria-hidden="true" />
                {t('memory.revokedAt', { date: formatEpoch(memory.revoked_at) })}
              </span>
            )}
          </div>
          <p className="mt-2 text-sm text-txt">{memory.content}</p>
          <div className="mt-2 flex flex-wrap items-center gap-x-4 gap-y-1 text-xs text-muted">
            <span className="font-mono">{memory.memory_id}</span>
            <span>
              {t('memory.extractedAt')}: {formatEpoch(memory.extracted_at)}
            </span>
            {explain && memory.extraction_method && (
              <ExtractionMethodBadge method={memory.extraction_method} />
            )}
          </div>
        </div>

        {!invalidated && (
          <div className="flex shrink-0 items-center gap-1">
            <Button
              size="sm"
              variant="secondary"
              leftIcon={<PencilIcon className="h-4 w-4" />}
              onClick={() => onEdit(memory)}
              aria-label={`Edit ${memory.memory_id}`}
              data-testid="memory-edit-btn"
            >
              {t('common.edit')}
            </Button>
            <Button
              size="sm"
              variant="secondary"
              leftIcon={<TrashIcon className="h-4 w-4" />}
              onClick={() => onInvalidate(memory)}
              aria-label={`Invalidate ${memory.memory_id}`}
              data-testid="memory-invalidate-btn"
            >
              {t('memory.invalidateConfirm')}
            </Button>
          </div>
        )}
      </div>

      {/* B-class provenance — only when explain mode is on (§ 7.3). */}
      {explain &&
        (memory.source_session_id ||
          memory.source_interaction_id ||
          memory.invalidated_by_block_id) && (
          <dl className="mt-3 grid grid-cols-1 gap-x-6 gap-y-1 border-t border-line pt-3 text-xs sm:grid-cols-2">
            {memory.source_session_id && (
              <div className="flex gap-2">
                <dt className="text-muted">{t('memory.sourceSession')}:</dt>
                <dd className="truncate font-mono text-txt">{memory.source_session_id}</dd>
              </div>
            )}
            {memory.source_interaction_id && (
              <div className="flex gap-2">
                <dt className="text-muted">{t('memory.sourceInteraction')}:</dt>
                <dd className="truncate font-mono text-txt">
                  {memory.source_interaction_id}
                </dd>
              </div>
            )}
            {memory.invalidated_by_block_id && (
              <div className="flex gap-2">
                <dt className="text-muted">{t('memory.invalidatedBy')}:</dt>
                <dd className="truncate font-mono text-txt">
                  {memory.invalidated_by_block_id}
                </dd>
              </div>
            )}
          </dl>
        )}
    </article>
  );
}
