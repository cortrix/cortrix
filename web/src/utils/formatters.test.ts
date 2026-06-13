import { describe, it, expect } from 'vitest';
import { formatFileSize, formatEpoch, formatRelativeTime } from './formatters';

describe('formatFileSize', () => {
  it('formats bytes', () => {
    expect(formatFileSize(500)).toBe('500 B');
  });

  it('formats KB', () => {
    expect(formatFileSize(2048)).toBe('2.0 KB');
  });

  it('formats MB', () => {
    expect(formatFileSize(2500000)).toBe('2.4 MB');
  });
});

describe('formatEpoch', () => {
  it('returns a dash for null/undefined', () => {
    expect(formatEpoch(null)).toBe('—');
    expect(formatEpoch(undefined)).toBe('—');
  });

  it('returns a dash for an invalid timestamp', () => {
    expect(formatEpoch(NaN)).toBe('—');
  });

  it('formats a valid epoch to a non-empty locale string', () => {
    const out = formatEpoch(Date.UTC(2026, 0, 1, 12, 0, 0));
    expect(out).not.toBe('—');
    expect(out.length).toBeGreaterThan(0);
  });
});

describe('formatRelativeTime', () => {
  it('reports "just now" within the first minute', () => {
    expect(formatRelativeTime(new Date().toISOString())).toBe('just now');
  });

  it('reports minutes', () => {
    expect(formatRelativeTime(new Date(Date.now() - 5 * 60_000).toISOString())).toBe('5m ago');
  });

  it('reports hours', () => {
    expect(formatRelativeTime(new Date(Date.now() - 3 * 3_600_000).toISOString())).toBe('3h ago');
  });

  it('reports days', () => {
    expect(formatRelativeTime(new Date(Date.now() - 2 * 86_400_000).toISOString())).toBe('2d ago');
  });
});
