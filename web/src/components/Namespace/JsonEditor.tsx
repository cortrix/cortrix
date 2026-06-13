import { lazy, Suspense } from 'react';
import { useAppStore } from '../../store/useAppStore';

// Thin Monaco wrapper for editing a JSON config blob (P02a design § 8.2 — raw
// JSON / Advanced mode). Follows the app theme (Magma light ↔ dark) via the
// store. Kept minimal: no language services beyond JSON, fixed compact height.
//
// Perf (P02a § 14.2/§ 14.3 — S10 code-split): @monaco-editor/react is the
// single heaviest dependency and is only reached on the Namespace Advanced /
// raw-JSON drawer. It is React.lazy-imported here so Monaco lands in its own
// `editor-vendor` chunk (vite.config.ts manualChunks) that is fetched on demand
// instead of in the initial bundle. A lightweight textarea-shaped fallback is
// shown while the chunk loads so layout doesn't jump.

const MonacoEditor = lazy(() => import('@monaco-editor/react'));

interface JsonEditorProps {
  value: string;
  onChange: (value: string) => void;
  height?: number;
  ariaLabel?: string;
  readOnly?: boolean;
}

function EditorFallback({ height }: { height: number }) {
  return (
    <div
      className="grid place-items-center bg-surface2 text-xs text-muted"
      style={{ height }}
      aria-hidden="true"
    >
      Loading editor…
    </div>
  );
}

export function JsonEditor({
  value,
  onChange,
  height = 160,
  ariaLabel = 'JSON editor',
  readOnly = false,
}: JsonEditorProps) {
  const theme = useAppStore((s) => s.theme);

  return (
    <div
      className="overflow-hidden rounded-md border border-line"
      data-testid="json-editor"
      aria-label={ariaLabel}
    >
      <Suspense fallback={<EditorFallback height={height} />}>
        <MonacoEditor
          height={height}
          language="json"
          theme={theme === 'dark' ? 'vs-dark' : 'light'}
          value={value}
          onChange={(v) => onChange(v ?? '')}
          loading={<EditorFallback height={height} />}
          options={{
            readOnly,
            minimap: { enabled: false },
            lineNumbers: 'off',
            folding: false,
            scrollBeyondLastLine: false,
            fontSize: 12,
            fontFamily: "'JetBrains Mono', 'Fira Code', Consolas, monospace",
            tabSize: 2,
            renderLineHighlight: 'none',
            overviewRulerLanes: 0,
            scrollbar: { vertical: 'auto', horizontal: 'auto' },
            padding: { top: 8, bottom: 8 },
          }}
        />
      </Suspense>
    </div>
  );
}
