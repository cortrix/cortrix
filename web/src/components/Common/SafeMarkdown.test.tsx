import { describe, it, expect } from 'vitest';
import { render, screen } from '@testing-library/react';
import { SafeMarkdown } from './SafeMarkdown';

// SafeMarkdown XSS defense tests (web UI design § 4.7). These assert the sanitize
// chain (DOMPurify + rehypeSanitize) on the markdown surface shared by Chat and
// Search — the LLM-output / indexed-content render path.

describe('SafeMarkdown (§ 4.7 XSS)', () => {
  it('renders safe markdown (bold / code)', () => {
    const { container } = render(<SafeMarkdown content={'**bold** and `code`'} />);
    expect(container.querySelector('strong')?.textContent).toBe('bold');
    expect(container.querySelector('code')?.textContent).toBe('code');
  });

  it('does NOT execute or emit a <script> tag from the content', () => {
    const { container } = render(
      <SafeMarkdown content={'Hello <script>window.__pwned = 1</script> world'} />,
    );
    // No script element survives the sanitizer.
    expect(container.querySelector('script')).toBeNull();
    expect((window as unknown as { __pwned?: number }).__pwned).toBeUndefined();
  });

  it('strips an onerror handler from injected <img>', () => {
    const { container } = render(
      <SafeMarkdown content={'<img src="x" onerror="window.__xss=1" />'} />,
    );
    const img = container.querySelector('img');
    // Either the img is dropped or — if kept — it carries no event handler attr.
    if (img) {
      expect(img.getAttribute('onerror')).toBeNull();
    }
    expect((window as unknown as { __xss?: number }).__xss).toBeUndefined();
  });

  it('forces safe rel/target on rendered links (no reverse-tabnabbing)', () => {
    const { container } = render(<SafeMarkdown content={'[docs](https://cortrix.ai)'} />);
    const a = container.querySelector('a');
    expect(a).not.toBeNull();
    expect(a?.getAttribute('target')).toBe('_blank');
    expect(a?.getAttribute('rel')).toContain('noopener');
    expect(a?.getAttribute('rel')).toContain('noreferrer');
  });

  it('drops a javascript: protocol link href', () => {
    const { container } = render(
      // eslint-disable-next-line no-script-url
      <SafeMarkdown content={'[click](javascript:alert(1))'} />,
    );
    const a = container.querySelector('a');
    // The javascript: href must not pass through the protocol allow-list.
    expect(a?.getAttribute('href') ?? '').not.toContain('javascript:');
  });

  it('renders empty content without crashing', () => {
    render(<SafeMarkdown content={''} />);
    // No throw == pass; nothing asserted beyond a successful render.
    expect(screen.queryByText('undefined')).toBeNull();
  });
});
