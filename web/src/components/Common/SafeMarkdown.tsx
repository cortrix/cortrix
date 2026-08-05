import ReactMarkdown from 'react-markdown';
import rehypeSanitize, { defaultSchema } from 'rehype-sanitize';
import DOMPurify from 'dompurify';

// SafeMarkdown (web UI design § 4.7 — XSS defense: DOMPurify + rehypeSanitize).
//
// Both the Chat assistant stream and the Search result snippets render
// model-produced / user-supplied content as Markdown. Rendering that through a
// bare <ReactMarkdown> is an XSS sink (a crafted LLM response or an indexed
// document chunk could smuggle markup). This component applies a two-layer
// defense matching the design's allow-list:
//
//   1. DOMPurify.sanitize() strips dangerous markup from the raw string before
//      it is parsed (defends against raw-HTML passthrough, e.g. <img onerror>).
//   2. rehypeSanitize re-sanitizes the produced HAST against an allow-list
//      schema (defense-in-depth on the parsed tree).
//
// react-markdown does NOT enable raw HTML by default (no rehype-raw here), so
// inline HTML in the source is already escaped; the two sanitizers harden the
// pieces that DO pass through (links, code, emphasis) and any future raw-HTML
// plugin from re-opening the hole.
//
// Links: anchors are forced to rel="noopener noreferrer" + target="_blank" to
// block reverse-tabnabbing (§ 4.7). data:/javascript: protocols are dropped by
// the protocol allow-list below.

// Allowed inline/markdown tags (web UI ALLOWED_TAGS).
const ALLOWED_TAGS = [
  'p',
  'strong',
  'em',
  'code',
  'pre',
  'a',
  'ul',
  'ol',
  'li',
  'h1',
  'h2',
  'h3',
  'br',
  // markdown commonly emits these too; keep the surface coherent with prose CSS
  'blockquote',
  'hr',
  'span',
];

// rehype-sanitize schema: start from the library default (a safe GitHub-like
// allow-list) and narrow it to the design's tag set + forced-safe anchors.
const sanitizeSchema = {
  ...defaultSchema,
  tagNames: ALLOWED_TAGS,
  attributes: {
    ...defaultSchema.attributes,
    a: [['href'], ['rel'], ['target'], ['className']],
    code: [['className']],
    span: [['className']],
  },
  // Only allow safe URL protocols on href (drops javascript:, data:, vbscript:).
  protocols: {
    ...defaultSchema.protocols,
    href: ['http', 'https', 'mailto'],
  },
};

export function SafeMarkdown({ content }: { content: string }) {
  // Layer 1 — strip dangerous raw markup before parsing.
  const sanitized = DOMPurify.sanitize(content ?? '', {
    ALLOWED_TAGS,
    ALLOWED_ATTR: ['href', 'rel', 'target', 'class'],
  });

  return (
    <ReactMarkdown
      // Layer 2 — re-sanitize the parsed tree against the allow-list schema.
      rehypePlugins={[[rehypeSanitize, sanitizeSchema]]}
      components={{
        // Force every anchor to open safely (no reverse-tabnabbing, § 4.7).
        a: ({ node: _node, ...props }) => (
          <a {...props} target="_blank" rel="noopener noreferrer" />
        ),
      }}
    >
      {sanitized}
    </ReactMarkdown>
  );
}
