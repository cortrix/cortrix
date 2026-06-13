import { describe, it, expect, vi, afterEach } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { AppErrorBoundary } from './AppErrorBoundary';

// AppErrorBoundary tests (P02a design § 16.3 / § 16.4 — JS exception placement).
// A child that throws on its first render shows the fallback; the reset button
// re-renders the children (which then succeed) so the app recovers.

let shouldThrow = true;
function Boom() {
  if (shouldThrow) throw new Error('kaboom');
  return <div data-testid="recovered">recovered</div>;
}

describe('AppErrorBoundary (§ 16.3)', () => {
  afterEach(() => {
    shouldThrow = true;
  });

  it('renders the fallback with a reset affordance when a child throws', () => {
    // Silence the expected React error-boundary console noise for this case.
    const spy = vi.spyOn(console, 'error').mockImplementation(() => {});
    render(
      <AppErrorBoundary>
        <Boom />
      </AppErrorBoundary>,
    );
    expect(screen.getByTestId('app-error-boundary')).toBeInTheDocument();
    expect(screen.getByTestId('error-boundary-reset')).toBeInTheDocument();
    // The thrown message is surfaced for debugging.
    expect(screen.getByText('kaboom')).toBeInTheDocument();
    spy.mockRestore();
  });

  it('recovers (re-renders children) when reset is clicked', () => {
    const spy = vi.spyOn(console, 'error').mockImplementation(() => {});
    render(
      <AppErrorBoundary>
        <Boom />
      </AppErrorBoundary>,
    );
    // Next render will succeed.
    shouldThrow = false;
    fireEvent.click(screen.getByTestId('error-boundary-reset'));
    expect(screen.getByTestId('recovered')).toBeInTheDocument();
    expect(screen.queryByTestId('app-error-boundary')).not.toBeInTheDocument();
    spy.mockRestore();
  });

  it('renders children unchanged on the happy path', () => {
    shouldThrow = false;
    render(
      <AppErrorBoundary>
        <div data-testid="ok">ok</div>
      </AppErrorBoundary>,
    );
    expect(screen.getByTestId('ok')).toBeInTheDocument();
    expect(screen.queryByTestId('app-error-boundary')).not.toBeInTheDocument();
  });
});
