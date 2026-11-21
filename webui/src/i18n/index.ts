import i18n from 'i18next';
import { initReactI18next } from 'react-i18next';
import zhHans from './locales/zh-Hans.json';
import en from './locales/en.json';

export const LANGUAGES = [
  { code: 'zh-Hans', label: '简体中文', locale: 'zh-CN' },
  { code: 'en', label: 'English', locale: 'en-US' },
] as const;

function initialLanguage(): string {
  try {
    const saved = localStorage.getItem('catofficial.language');
    if (saved && LANGUAGES.some((l) => l.code === saved)) return saved;
  } catch { /* 隐私模式下读不到存储 */ }
  // 用户基本都是中文开发者：除非浏览器明确是非中文，否则用简体中文。
  const nav = navigator.language.toLowerCase();
  return nav.startsWith('zh') || !nav ? 'zh-Hans' : 'en';
}

i18n.use(initReactI18next).init({
  resources: { 'zh-Hans': { translation: zhHans }, en: { translation: en } },
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
