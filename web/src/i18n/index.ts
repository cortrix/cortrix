import i18n from 'i18next';
import { initReactI18next } from 'react-i18next';
import en from './locales/en.json';
import zh from './locales/zh.json';

// i18n (P02a design § 13 — react-i18next, en + zh bilingual). The active
// language is auto-detected from navigator.language (e.g. "zh-CN" → "zh"),
// falling back to English. en is the fallback so any missing zh key degrades to
// the English string rather than the raw key.

function detectLanguage(): 'en' | 'zh' {
  if (typeof navigator === 'undefined') return 'en';
  const langs = navigator.languages?.length ? navigator.languages : [navigator.language];
  for (const l of langs) {
    if (l?.toLowerCase().startsWith('zh')) return 'zh';
    if (l?.toLowerCase().startsWith('en')) return 'en';
  }
  return 'en';
}

i18n.use(initReactI18next).init({
  resources: {
    en: { translation: en },
    zh: { translation: zh },
  },
  lng: detectLanguage(),
  fallbackLng: 'en',
  interpolation: {
    escapeValue: false,
  },
});

export default i18n;
