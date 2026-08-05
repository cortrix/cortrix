import type { SearchResult } from '../../types/api';
import { SafeMarkdown } from '../Common/SafeMarkdown';

interface ResultItemProps {
  result: SearchResult;
}

// Cross-NS ResultItem renderer. The result carries child_id/parent_id (ULIDs),
// the matched `content`, the source `namespace` (cross-NS: each hit may come from a
// different NS), and score + rerank_score. block_type/source_path/hit_routes from the
// old single-NS schema no longer exist on the wire.
export function ResultItem({ result }: ResultItemProps) {
  return (
    <div className="rounded-xl p-5 transition-all hover:border-magma/40 bg-surface border border-line card-shadow">
      <div className="flex items-center gap-2 mb-2">
        <span className="px-2 py-0.5 text-xs rounded font-medium bg-magma/15 text-magma-h">
          {result.namespace}
        </span>
        <div className="ml-auto flex items-center gap-1.5">
          {result.rerank_score > 0 && (
            <span className="px-1.5 py-0.5 text-[10px] rounded font-mono bg-ok/10 text-ok">
              RERANK {result.rerank_score.toFixed(3)}
            </span>
          )}
          <span className="text-sm font-semibold text-cortrix-700 dark:text-cortrix-400">
            {result.score.toFixed(3)}
          </span>
        </div>
      </div>
      <div className="text-sm leading-relaxed text-txt prose prose-sm dark:prose-invert max-w-none">
        <SafeMarkdown content={result.content} />
      </div>
      <div className="flex items-center gap-3 mt-3 text-xs text-muted font-mono">
        <span title={result.child_id}>child {result.child_id.slice(0, 10)}…</span>
        {result.parent_id && (
          <>
            <span>&middot;</span>
            <span title={result.parent_id}>parent {result.parent_id.slice(0, 10)}…</span>
          </>
        )}
      </div>
    </div>
  );
}
