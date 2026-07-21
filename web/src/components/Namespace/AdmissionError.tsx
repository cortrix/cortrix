import type { AgentError } from '../../types/api';
import { ErrorDisplay } from '../Common/ErrorDisplay';

// F05 admission error friendly handling (P02a design § 8.4). Maps the three F05
// admission codes to a remediation hint, then renders the standard GEN-Agent
// ErrorDisplay (which already shows structured_data + retry countdown). Any
// other error falls through to a plain ErrorDisplay.

const REMEDIATION: Record<string, string> = {
  CX_ERR_NS_QUOTA_EXCEEDED:
    'Namespace quota exceeded. Delete unused namespaces or request a higher quota.',
  CX_ERR_NS_RESOURCE_BUDGET_EXCEEDED:
    'Resource budget exceeded. Disable optional features or raise the resource budget.',
  CX_ERR_NS_POOL_INTERNAL:
    'The namespace pool hit a transient error. Try again shortly.',
};

interface AdmissionErrorProps {
  error: AgentError;
  onRetry?: () => void;
}

export function AdmissionError({ error, onRetry }: AdmissionErrorProps) {
  const hint = REMEDIATION[error.code];
  // Augment the message with the remediation hint when we recognize the code.
  const display: AgentError = hint
    ? { ...error, message: `${error.message} — ${hint}` }
    : error;

  return (
    <ErrorDisplay
      error={display}
      onRetry={error.code === 'CX_ERR_NS_POOL_INTERNAL' ? onRetry : undefined}
    />
  );
}
