import i18n from 'i18next';
import { initReactI18next } from 'react-i18next';
import zhHans from './locales/zh-Hans.json';
import zhHant from './locales/zh-Hant.json';
import en from './locales/en.json';
import ja from './locales/ja.json';

export const LANGUAGES = [
  { code: 'zh-Hans', label: '简体中文', locale: 'zh-CN' },
  { code: 'zh-Hant', label: '繁體中文', locale: 'zh-TW' },
  { code: 'en', label: 'English', locale: 'en-US' },
  { code: 'ja', label: '日本語', locale: 'ja-JP' },
] as const;

function initialLanguage(): string {
  try {
    const saved = localStorage.getItem('catofficial.language');
    if (saved && LANGUAGES.some((l) => l.code === saved)) return saved;
  } catch { /* 隐私模式下读不到存储 */ }
  const nav = navigator.language.toLowerCase();
  if (nav.startsWith('zh-tw') || nav.startsWith('zh-hk') || nav.startsWith('zh-hant')) return 'zh-Hant';
  if (nav.startsWith('zh')) return 'zh-Hans';
  if (nav.startsWith('ja')) return 'ja';
  return 'en';
}

i18n.use(initReactI18next).init({
  resources: { 'zh-Hans': { translation: zhHans }, 'zh-Hant': { translation: zhHant }, en: { translation: en }, ja: { translation: ja } },
  lng: initialLanguage(),
  fallbackLng: 'zh-Hans',
  interpolation: { escapeValue: false },
});

export function setLanguage(code: string) {
  void i18n.changeLanguage(code);
  try { localStorage.setItem('catofficial.language', code); } catch { /* 忽略 */ }
}

export function currentLocale(): string {
  return LANGUAGES.find((l) => l.code === i18n.language)?.locale ?? 'zh-CN';
}

export default i18n;
