import { describe, it, expect, vi } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { ErrorDisplay } from './ErrorDisplay';
import type { AgentError } from '../../types/api';

// ErrorDisplay GEN-Agent tests (P02a design § 16.1). Covers code/message,
// category badge, structured_data collapsible, and the retryable + retry button
// contract. The error-metric side effect (§ 23-bis) is a no-op in tests (metrics
// uninitialized) so it does not interfere.

function makeError(over: Partial<AgentError> = {}): AgentError {
  return {
    code: 'CX_ERR_NS_QUOTA_EXCEEDED',
    message: 'Namespace quota exceeded',
    category: 'quota',
    retryable: false,
    ...over,
  } as AgentError;
}

describe('ErrorDisplay (§ 16.1 GEN-Agent)', () => {
  it('renders the code, message and category label', () => {
    render(<ErrorDisplay error={makeError()} />);
    expect(screen.getByText('CX_ERR_NS_QUOTA_EXCEEDED')).toBeInTheDocument();
    expect(screen.getByText('Namespace quota exceeded')).toBeInTheDocument();
    // Exact match for the category badge (the code text also contains "QUOTA").
    expect(screen.getByText('Quota')).toBeInTheDocument();
  });

  it('exposes the error code on the container for DOM scraping', () => {
    render(<ErrorDisplay error={makeError()} />);
    expect(screen.getByTestId('error-display')).toHaveAttribute(
      'data-error-code',
      'CX_ERR_NS_QUOTA_EXCEEDED',
    );
  });

  it('shows a Retry button only when retryable + onRetry given', () => {
    const onRetry = vi.fn();
    render(<ErrorDisplay error={makeError({ retryable: true })} onRetry={onRetry} />);
    const btn = screen.getByTestId('error-retry');
    expect(btn).toBeEnabled();
    fireEvent.click(btn);
    expect(onRetry).toHaveBeenCalledOnce();
  });

  it('hides Retry for a non-retryable error', () => {
    render(<ErrorDisplay error={makeError({ retryable: false })} onRetry={() => {}} />);
    expect(screen.queryByTestId('error-retry')).not.toBeInTheDocument();
  });

  it('renders structured_data inside a collapsible', () => {
    render(
      <ErrorDisplay
        error={makeError({ structured_data: { namespaces_failed: ['legal'] } })}
      />,
    );
    expect(screen.getByText(/Details/i)).toBeInTheDocument();
    expect(screen.getByText(/namespaces_failed/)).toBeInTheDocument();
  });
});
