import { ApiError } from './client';
import type { AgentError, AgentErrorCategory } from '../types/api';

// GEN-Agent error parsing (CLAUDE.md § 5 — machine-readable errors). The HTTP
// client throws `ApiError(status, body)` where `body` is the raw response text.
// Backends return `{ error: { code, message, retryable, category,
// retry_after_ms, structured_data } }`. This helper normalises any thrown value
// into an `AgentError` so the UI can render a consistent, structured display
// (and, for transient/quota errors, a retry countdown).

const VALID_CATEGORIES: AgentErrorCategory[] = [
  'auth',
  'quota',
  'transient',
  'permanent',
  'timeout',
];

function coerceCategory(value: unknown, status?: number): AgentErrorCategory {
  if (typeof value === 'string' && (VALID_CATEGORIES as string[]).includes(value)) {
    return value as AgentErrorCategory;
  }
  // Heuristic fallback from HTTP status when category is absent.
  if (status === 401 || status === 403) return 'auth';
  if (status === 429) return 'quota';
  if (status === 408 || status === 504) return 'timeout';
  if (status !== undefined && status >= 500) return 'transient';
  return 'permanent';
}

/** Normalise any caught error into the GEN-Agent 5-field shape. */
export function parseAgentError(err: unknown): AgentError {
  if (err instanceof ApiError) {
    let parsed: unknown = null;
    try {
      parsed = JSON.parse(err.message);
    } catch {
      parsed = null;
    }
    const envelope =
      parsed && typeof parsed === 'object' && 'error' in parsed
        ? (parsed as { error: unknown }).error
        : parsed;

    if (envelope && typeof envelope === 'object') {
      const e = envelope as Record<string, unknown>;
      return {
        code: typeof e.code === 'string' ? e.code : `CX_ERR_HTTP_${err.status}`,
        message:
          typeof e.message === 'string' && e.message
            ? e.message
            : err.message || `Request failed (${err.status})`,
        retryable: typeof e.retryable === 'boolean' ? e.retryable : err.status >= 500,
        category: coerceCategory(e.category, err.status),
        retry_after_ms:
          typeof e.retry_after_ms === 'number' ? e.retry_after_ms : null,
        structured_data:
          e.structured_data && typeof e.structured_data === 'object'
            ? (e.structured_data as Record<string, unknown>)
            : undefined,
      };
    }

    return {
      code: `CX_ERR_HTTP_${err.status}`,
      message: err.message || `Request failed (${err.status})`,
      retryable: err.status >= 500,
      category: coerceCategory(undefined, err.status),
      retry_after_ms: null,
    };
  }

  if (err instanceof Error) {
    return {
      code: 'CX_ERR_CLIENT',
      message: err.message,
      retryable: false,
      category: 'permanent',
      retry_after_ms: null,
    };
  }

  return {
    code: 'CX_ERR_UNKNOWN',
    message: String(err),
    retryable: false,
    category: 'permanent',
    retry_after_ms: null,
  };
}

/** Best-effort short message for toasts. */
export function errorMessage(err: unknown): string {
  return parseAgentError(err).message;
}
