// Timezone helpers.
//
// The backend stores every timestamp as Unix epoch-ms in UTC (see
// api/paths/observability.yaml — `from_timestamp` / `to_timestamp` are
// int64 Unix-ms). This module is the single place where that UTC instant is
// projected into a user-visible wall clock, so that every screen agrees on
// the same zone and the same "day" boundaries.

// Curated, always-available zones. `Intl.supportedValuesOf('timeZone')` is
// the full IANA list, but it is only declared in the ES2022 lib while this
// project compiles against ES2020 — so it is reached through a narrow cast
// and this list stays as the fallback / minimum guarantee.
export const TIMEZONES = [
  'UTC',
  'Asia/Shanghai',
  'Asia/Hong_Kong',
  'Asia/Taipei',
  'Asia/Tokyo',
  'Asia/Seoul',
  'Asia/Singapore',
  'Asia/Kolkata',
  'Asia/Dubai',
  'Europe/London',
  'Europe/Berlin',
  'Europe/Paris',
  'Europe/Madrid',
  'Europe/Moscow',
  'America/New_York',
  'America/Chicago',
  'America/Denver',
  'America/Los_Angeles',
  'America/Toronto',
  'America/Sao_Paulo',
  'Australia/Sydney',
  'Pacific/Auckland',
];

type SupportedValuesOf = (key: string) => string[];

function allIanaTimezones(): string[] {
  try {
    const supportedValuesOf = (
      Intl as unknown as { supportedValuesOf?: SupportedValuesOf }
    ).supportedValuesOf;
    if (typeof supportedValuesOf === 'function') {
      return supportedValuesOf.call(Intl, 'timeZone');
    }
  } catch {
    // Older runtimes: fall through to the curated list.
  }
  return [];
}

/** The browser's own timezone, with a safe fallback. */
export function getSystemTimezone(): string {
  try {
    const tz = Intl.DateTimeFormat().resolvedOptions().timeZone;
    return tz || 'UTC';
  } catch {
    return 'UTC';
  }
}

/** True when `tz` is a zone this runtime can actually format with. */
export function isValidTimezone(tz: string): boolean {
  if (!tz) return false;
  try {
    new Intl.DateTimeFormat('en-US', { timeZone: tz }).format(new Date());
    return true;
  } catch {
    return false;
  }
}

/**
 * The zones offered in the Settings picker: the full IANA list when the
 * runtime exposes it, always including UTC, the curated list, and the
 * browser's own zone (the user's current zone must always be selectable,
 * even when the runtime does not expose the full list).
 *
 * `Intl.supportedValuesOf('timeZone')` returns canonical names only, so
 * aliases such as `Asia/Kolkata` (canonical: `Asia/Calcutta`) would be
 * missing — hence the curated list is always merged in.
 */
export function listTimezones(): string[] {
  const zones = new Set<string>(['UTC', getSystemTimezone()]);
  for (const tz of TIMEZONES) zones.add(tz);
  for (const tz of allIanaTimezones()) zones.add(tz);
  return Array.from(zones);
}

interface DateParts {
  year: number;
  month: number;
  day: number;
  hour: number;
  minute: number;
  second: number;
}

function partsInZone(date: Date, timeZone: string): DateParts {
  const parts = new Intl.DateTimeFormat('en-US', {
    timeZone,
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hourCycle: 'h23',
  }).formatToParts(date);
  const map: Record<string, string> = {};
  for (const part of parts) {
    if (part.type !== 'literal') map[part.type] = part.value;
  }
  return {
    year: Number(map.year),
    month: Number(map.month),
    day: Number(map.day),
    hour: Number(map.hour),
    minute: Number(map.minute),
    second: Number(map.second),
  };
}

/**
 * Offset of `timeZone` at `date`, in milliseconds east of UTC
 * (Asia/Shanghai => +8h).
 */
export function timezoneOffsetMs(date: Date, timeZone: string): number {
  const p = partsInZone(date, timeZone);
  const asUtc = Date.UTC(p.year, p.month - 1, p.day, p.hour, p.minute, p.second);
  // formatToParts drops milliseconds, so compare against the truncated instant.
  return asUtc - Math.floor(date.getTime() / 1000) * 1000;
}

/**
 * Convert a wall-clock time in `timeZone` to the corresponding UTC instant.
 *
 * `wall` is expressed as its UTC-epoch pseudo value (i.e. the same fields the
 * picker shows). Two correction passes are enough because a zone's offset is
 * a step function of the instant; the second pass lands on the correct side
 * of a DST transition.
 */
function zonedWallClockToUtc(wall: number, timeZone: string): number {
  let ts = wall;
  for (let i = 0; i < 2; i += 1) {
    const next = wall - timezoneOffsetMs(new Date(ts), timeZone);
    if (next === ts) break;
    ts = next;
  }
  return ts;
}

/**
 * Format an instant as `YYYY-MM-DD HH:mm` in `timeZone`.
 *
 * `formatToParts` is used rather than `format()` because `format()` output
 * is locale-shaped (`Jan 15, 2026, 08:00`) and cannot be sliced safely.
 */
export function formatInTimezone(date: Date, timeZone: string): string {
  const zone = isValidTimezone(timeZone) ? timeZone : 'UTC';
  const p = partsInZone(date, zone);
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${p.year}-${pad(p.month)}-${pad(p.day)} ${pad(p.hour)}:${pad(p.minute)}`;
}

/**
 * Format a server timestamp that may arrive either as epoch-ms (the C++ API
 * sends `last_used_at` as an int64 Unix-ms) or as an ISO-8601 string.
 *
 * Returns only the calendar date, matching the column it fills.
 */
export function formatDateOnly(
  value: string | number | null | undefined,
  timeZone: string,
): string {
  if (value == null || value === '') return '—';
  let date: Date;
  if (typeof value === 'number') {
    // Values below 1e12 are seconds (the API mixes both units).
    date = new Date(value < 1_000_000_000_000 ? value * 1000 : value);
  } else {
    date = new Date(value);
    if (Number.isNaN(date.getTime())) {
      // Not a parseable date — fall back to the raw prefix rather than '—',
      // so an unexpected shape is still readable.
      return value.slice(0, 10);
    }
  }
  if (Number.isNaN(date.getTime())) return '—';
  return formatInTimezone(date, timeZone).slice(0, 10);
}

/**
 * Convert a `YYYY-MM-DD` input value to the UTC instant of that calendar day
 * in `timeZone` (start of day, or the last millisecond of the day).
 *
 * The date filter must use the same zone as the table, otherwise "today"
 * would select a different window than the one displayed.
 *
 * The end of day is derived from the *next* day's start rather than by
 * adding 24h to this day's start: on a DST transition day the local day is
 * 23 or 25 hours long, and a fixed 24h offset would spill into the next
 * calendar day (or cut the day short).
 */
export function dateToMs(
  value: string,
  endOfDay: boolean,
  timeZone: string,
): number | undefined {
  if (!value) return undefined;
  const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(value);
  if (!match) return undefined;
  const zone = isValidTimezone(timeZone) ? timeZone : 'UTC';
  const year = Number(match[1]);
  const month = Number(match[2]);
  const day = Number(match[3]);
  const base = Date.UTC(year, month - 1, day, 0, 0, 0);
  if (Number.isNaN(base)) return undefined;
  if (!endOfDay) return zonedWallClockToUtc(base, zone);
  const nextDayStart = zonedWallClockToUtc(base + 86_400_000, zone);
  return nextDayStart - 1;
}
