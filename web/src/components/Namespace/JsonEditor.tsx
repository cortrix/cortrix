// Lightweight, self-contained JSON editor for the per-config blobs.
//
// Replaces the prior @monaco-editor/react integration: Monaco loads its core from
// a CDN at runtime (the package ships no bundled core, and no loader.config points
// it at a local copy). In the self-hosted deployment (offline / CSP-restricted) that
// fetch never resolves, so the editor was stuck forever on "Loading editor…". The
// config blobs are small JSON and ConfigSection already validates JSON syntax, so a
// monospace textarea is sufficient and always works — no CDN, worker, or CSP need.

interface JsonEditorProps {
  value: string;
  onChange: (value: string) => void;
  height?: number;
  ariaLabel?: string;
  readOnly?: boolean;
}

export function JsonEditor({
  value,
  onChange,
  height = 160,
  ariaLabel = 'JSON editor',
  readOnly = false,
}: JsonEditorProps) {
  return (
    <textarea
      data-testid="json-editor"
      aria-label={ariaLabel}
      value={value}
      readOnly={readOnly}
      spellCheck={false}
      autoCapitalize="off"
      autoCorrect="off"
      onChange={(e) => onChange(e.target.value)}
      style={{ height }}
      className="w-full resize-y rounded-md border border-line bg-codebg px-3 py-2 font-mono text-[12px] leading-relaxed text-txt outline-none focus:border-magma"
    />
  );
}
