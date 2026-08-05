import i18n from 'i18next';
import { initReactI18next } from 'react-i18next';
import en from './locales/en.json';

// i18n (web UI design § 13). English-only UI: Cortrix ships an English-only
// interface. The i18n layer is retained so all user-facing strings stay
// centralized (and a future localization can be added back as a new locale),
// but no non-English locale is bundled and the active language is locked to
// English regardless of the browser's navigator.language.

i18n.use(initReactI18next).init({
  resources: {
    en: { translation: en },
  },
  lng: 'en',
  fallbackLng: 'en',
  interpolation: {
    escapeValue: false,
  },
});

export default i18n;
