import { describe, it, expect } from 'vitest';
import { formatInTimezone } from './timezone';

describe('formatInTimezone', () => {
  it('formats correctly in UTC', () => {
    const date = new Date(Date.UTC(2026, 0, 1, 12, 0, 0));
    const result = formatInTimezone(date, 'UTC');
    expect(result).toBe('2026-01-01 12:00');
  });

  it('formats correctly in Asia/Shanghai', () => {
    const date = new Date(Date.UTC(2026, 0, 1, 12, 0, 0));
    const result = formatInTimezone(date, 'Asia/Shanghai');
    // UTC 12:00 -> Shanghai 20:00
    expect(result).toBe('2026-01-01 20:00');
  });
});
