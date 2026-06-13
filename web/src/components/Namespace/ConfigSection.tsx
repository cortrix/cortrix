import { BookOpenIcon } from '@heroicons/react/24/outline';
import { Badge } from '../ui';
import { JsonEditor } from './JsonEditor';
import type { ConfigMeta } from './namespaceConfigMeta';

// ConfigSection (P02a design § 8.3) — one card per *_config: title + Feature
// badge + "Learn more" link + JSON editor for the opaque blob. Per-config
// schema is owned by the respective Feature, so the UI offers an example shape
// and validates JSON syntax only. `error` (a parse error message) is surfaced
// inline so an invalid block blocks the save (§ 8.2 Advanced raw JSON rule).

interface ConfigSectionProps {
  meta: ConfigMeta;
  /** Raw JSON text for this config block. */
  value: string;
  onChange: (text: string) => void;
  /** Parse error message, if the current text is not valid JSON. */
  error?: string | null;
  learnMoreLabel: string;
}

export function ConfigSection({
  meta,
  value,
  onChange,
  error,
  learnMoreLabel,
}: ConfigSectionProps) {
  const isEmpty = value.trim() === '' || value.trim() === '{}';

  return (
    <div
      className="rounded-lg border border-line bg-surface card-shadow p-4"
      data-testid={`config-section-${meta.key}`}
    >
      <div className="mb-2 flex items-center justify-between gap-3">
        <div className="flex items-center gap-2">
          <h4 className="text-sm font-semibold text-txt">{meta.label}</h4>
          <Badge variant="magma">{meta.feature}</Badge>
          <code className="font-mono text-[11px] text-muted">{meta.key}</code>
        </div>
        <div className="flex items-center gap-3">
          {isEmpty && (
            <button
              type="button"
              onClick={() => onChange(JSON.stringify(meta.example, null, 2))}
              className="text-xs font-medium text-magma-h hover:underline"
              data-testid={`config-insert-example-${meta.key}`}
            >
              Insert example
            </button>
          )}
          <a
            href={`/docs/features/${meta.feature.toLowerCase()}`}
            target="_blank"
            rel="noopener noreferrer"
            className="inline-flex items-center gap-1 text-xs text-muted hover:text-txt"
          >
            <BookOpenIcon className="h-4 w-4" aria-hidden="true" />
            {learnMoreLabel}
          </a>
        </div>
      </div>
      <JsonEditor
        value={value}
        onChange={onChange}
        ariaLabel={`${meta.label} config JSON`}
        height={140}
      />
      {error && <p className="mt-1 text-xs text-error">{error}</p>}
    </div>
  );
}
