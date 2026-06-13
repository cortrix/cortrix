import { describe, it, expect, beforeEach } from 'vitest';
import i18n from './index';
import en from './locales/en.json';

describe('i18n', () => {
  beforeEach(async () => {
    await i18n.changeLanguage('en');
    localStorage.clear();
  });

  it('loads English translations', () => {
    expect(i18n.language).toBe('en');
    expect(i18n.t('header.title')).toBe('Cortrix');
    expect(i18n.t('sidebar.upload')).toBe('Upload');
  });

  it('supports interpolation', () => {
    expect(i18n.t('header.pending', { count: 3 })).toBe('3 pending');
  });

  it('has no empty translations in en', () => {
    const values = flattenValues(en);
    for (const [key, val] of values) {
      expect(val.length, `en key "${key}" is empty`).toBeGreaterThan(0);
    }
  });

  it('covers all component sections', () => {
    const requiredSections = [
      'header', 'sidebar', 'upload', 'search', 'chat',
      'namespace', 'common', 'llm', 'validation',
    ];
    const enSections = Object.keys(en);
    for (const section of requiredSections) {
      expect(enSections, `missing section: ${section}`).toContain(section);
    }
  });
});

function flattenValues(obj: Record<string, any>, prefix = ''): [string, string][] {
  const entries: [string, string][] = [];
  for (const [k, v] of Object.entries(obj)) {
    const full = prefix ? `${prefix}.${k}` : k;
    if (typeof v === 'object' && v !== null) {
      entries.push(...flattenValues(v, full));
    } else {
      entries.push([full, String(v)]);
    }
  }
  return entries;
}
