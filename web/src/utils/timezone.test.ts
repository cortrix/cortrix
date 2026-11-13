import { describe, it, expect } from 'vitest';
import {
  TIMEZONES,
  dateToMs,
  formatDateOnly,
  formatInTimezone,
  getSystemTimezone,
  isValidTimezone,
  listTimezones,
  timezoneOffsetMs,
} from './timezone';

describe('formatInTimezone', () => {
  it('formats correctly in UTC', () => {
    const date = new Date(Date.UTC(2026, 0, 1, 12, 0, 0));
    expect(formatInTimezone(date, 'UTC')).toBe('2026-01-01 12:00');
  });

  it('formats correctly in Asia/Shanghai', () => {
    const date = new Date(Date.UTC(2026, 0, 1, 12, 0, 0));
    // UTC 12:00 -> Shanghai 20:00
    expect(formatInTimezone(date, 'Asia/Shanghai')).toBe('2026-01-01 20:00');
  });

  it('formats correctly in a negative-offset zone', () => {
    const date = new Date(Date.UTC(2026, 0, 1, 12, 0, 0));
    // UTC 12:00 -> New York 07:00 (EST, UTC-5)
    expect(formatInTimezone(date, 'America/New_York')).toBe('2026-01-01 07:00');
  });

  it('pads every component to two digits', () => {
    const date = new Date(Date.UTC(2026, 0, 5, 3, 7, 0));
    expect(formatInTimezone(date, 'UTC')).toBe('2026-01-05 03:07');
  });

  it('renders midnight as 00, not 24', () => {
    const date = new Date(Date.UTC(2026, 0, 1, 0, 0, 0));
    expect(formatInTimezone(date, 'UTC')).toBe('2026-01-01 00:00');
    expect(formatInTimezone(date, 'Asia/Shanghai')).toBe('2026-01-01 08:00');
  });

  it('falls back to UTC for an unknown zone', () => {
    const date = new Date(Date.UTC(2026, 0, 1, 12, 0, 0));
    expect(formatInTimezone(date, 'Not/AZone')).toBe('2026-01-01 12:00');
  });
});

describe('getSystemTimezone', () => {
  it('returns a non-empty zone name', () => {
    const tz = getSystemTimezone();
    expect(typeof tz).toBe('string');
    expect(tz.length).toBeGreaterThan(0);
  });
});

describe('isValidTimezone', () => {
  it('accepts real IANA zones', () => {
    expect(isValidTimezone('UTC')).toBe(true);
    expect(isValidTimezone('Asia/Shanghai')).toBe(true);
    expect(isValidTimezone('Europe/Berlin')).toBe(true);
  });

  it('rejects unknown or empty zones', () => {
    expect(isValidTimezone('Not/AZone')).toBe(false);
    expect(isValidTimezone('')).toBe(false);
  });
});

describe('listTimezones', () => {
  it('always includes UTC and the browser zone', () => {
    const zones = listTimezones();
    expect(zones).toContain('UTC');
    expect(zones).toContain(getSystemTimezone());
  });

  it('includes the curated fallback list', () => {
    const zones = listTimezones();
    for (const tz of TIMEZONES) {
      expect(zones).toContain(tz);
    }
  });

  it('has no duplicates', () => {
    const zones = listTimezones();
    expect(new Set(zones).size).toBe(zones.length);
  });
});

describe('timezoneOffsetMs', () => {
  it('reports the offset east of UTC', () => {
    const date = new Date(Date.UTC(2026, 0, 1, 12, 0, 0));
    expect(timezoneOffsetMs(date, 'UTC')).toBe(0);
    expect(timezoneOffsetMs(date, 'Asia/Shanghai')).toBe(8 * 3600_000);
    expect(timezoneOffsetMs(date, 'Asia/Kolkata')).toBe(5.5 * 3600_000);
  });

  it('reports a negative offset for western zones', () => {
    const date = new Date(Date.UTC(2026, 0, 1, 12, 0, 0));
    expect(timezoneOffsetMs(date, 'America/New_York')).toBe(-5 * 3600_000);
  });
});

describe('dateToMs', () => {
  it('returns undefined for an empty or malformed value', () => {
    expect(dateToMs('', false, 'UTC')).toBeUndefined();
    expect(dateToMs('2026-1-1', false, 'UTC')).toBeUndefined();
    expect(dateToMs('not-a-date', false, 'UTC')).toBeUndefined();
  });

  it('maps the start of a day in UTC', () => {
    expect(dateToMs('2026-01-01', false, 'UTC')).toBe(Date.UTC(2026, 0, 1, 0, 0, 0));
  });

  it('maps the start of a day in Asia/Shanghai to the previous UTC day', () => {
    // Shanghai is UTC+8, so local midnight is 16:00 UTC the day before.
    expect(dateToMs('2026-01-01', false, 'Asia/Shanghai')).toBe(
      Date.UTC(2025, 11, 31, 16, 0, 0),
    );
  });

  it('maps the end of a day to the last millisecond', () => {
    const start = dateToMs('2026-01-01', false, 'Asia/Shanghai')!;
    const end = dateToMs('2026-01-01', true, 'Asia/Shanghai')!;
    expect(end - start).toBe(24 * 3600_000 - 1);
  });

  it('keeps a 23-hour DST day inside the calendar day', () => {
    // US spring-forward: 2026-03-08 is only 23 hours long in New York.
    const start = dateToMs('2026-03-08', false, 'America/New_York')!;
    const end = dateToMs('2026-03-08', true, 'America/New_York')!;
    expect(end - start).toBe(23 * 3600_000 - 1);
    // Both ends still land on 2026-03-08 in local time.
    expect(formatInTimezone(new Date(start), 'America/New_York')).toBe('2026-03-08 00:00');
    expect(formatInTimezone(new Date(end), 'America/New_York')).toBe('2026-03-08 23:59');
  });

  it('keeps a 25-hour DST day inside the calendar day', () => {
    // US fall-back: 2026-11-01 is 25 hours long in New York.
    const start = dateToMs('2026-11-01', false, 'America/New_York')!;
    const end = dateToMs('2026-11-01', true, 'America/New_York')!;
    expect(end - start).toBe(25 * 3600_000 - 1);
    expect(formatInTimezone(new Date(start), 'America/New_York')).toBe('2026-11-01 00:00');
    expect(formatInTimezone(new Date(end), 'America/New_York')).toBe('2026-11-01 23:59');
  });

  it('handles a half-hour DST shift zone', () => {
    // Australia/Lord_Howe shifts by 30 minutes, so the day is 24.5 hours.
    const start = dateToMs('2026-04-05', false, 'Australia/Lord_Howe')!;
    const end = dateToMs('2026-04-05', true, 'Australia/Lord_Howe')!;
    expect(end - start).toBe(24.5 * 3600_000 - 1);
    expect(formatInTimezone(new Date(start), 'Australia/Lord_Howe')).toBe('2026-04-05 00:00');
    expect(formatInTimezone(new Date(end), 'Australia/Lord_Howe')).toBe('2026-04-05 23:59');
  });
});

describe('formatDateOnly', () => {
  it('renders the calendar date in the requested zone', () => {
    const date = new Date(Date.UTC(2026, 0, 1, 12, 0, 0));
    expect(formatDateOnly(date.getTime(), 'UTC')).toBe('2026-01-01');
    // UTC 12:00 is already the next day in Shanghai (20:00 on the 1st) —
    // use an instant that crosses the date line.
    expect(formatDateOnly(Date.UTC(2026, 0, 1, 20, 0, 0), 'Asia/Shanghai')).toBe('2026-01-02');
  });

  it('accepts an ISO string', () => {
    expect(formatDateOnly('2026-01-01T12:00:00Z', 'UTC')).toBe('2026-01-01');
  });

  it('treats small numbers as epoch seconds', () => {
    // 1767225600 = 2026-01-01T00:00:00Z
    expect(formatDateOnly(1767225600, 'UTC')).toBe('2026-01-01');
  });

  it('returns an em dash for a missing value', () => {
    expect(formatDateOnly(null, 'UTC')).toBe('—');
    expect(formatDateOnly(undefined, 'UTC')).toBe('—');
    expect(formatDateOnly('', 'UTC')).toBe('—');
  });
});
