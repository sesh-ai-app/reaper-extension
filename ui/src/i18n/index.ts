import i18next from 'i18next';
import { initReactI18next } from 'react-i18next';

import germanTranslation from './locales/de.json';
import englishTranslation from './locales/en.json';
import spanishTranslation from './locales/es.json';
import frenchTranslation from './locales/fr.json';
import italianTranslation from './locales/it.json';
import japaneseTranslation from './locales/ja.json';
import brazilianPortugueseTranslation from './locales/pt-BR.json';

/**
 * The seven locales the extension UI ships (requirement 15.3). Tags are BCP 47, matching
 * the `locale` field the outbound prompt envelope carries.
 */
export const supportedLocales = ['en', 'de', 'ja', 'pt-BR', 'es', 'fr', 'it'] as const;

export type SupportedLocale = (typeof supportedLocales)[number];

export const fallbackLocale: SupportedLocale = 'en';

const resources = {
	en: { translation: englishTranslation },
	de: { translation: germanTranslation },
	ja: { translation: japaneseTranslation },
	'pt-BR': { translation: brazilianPortugueseTranslation },
	es: { translation: spanishTranslation },
	fr: { translation: frenchTranslation },
	it: { translation: italianTranslation },
} as const;

// Resources are bundled rather than fetched, so initialisation completes synchronously and
// the returned promise carries nothing the caller needs.
i18next.use(initReactI18next).init({
	resources,
	lng: fallbackLocale,
	fallbackLng: fallbackLocale,
	supportedLngs: [...supportedLocales],
	// All seven locale files are fully populated and `locales.test.ts` keeps them that way,
	// so this is a safety net rather than a working mechanism: should a value ever be empty,
	// English is a better thing to render than nothing. The one place that distinction is
	// visible to a producer is the error banner, which reads the active language's own
	// resource rather than trusting the fallback, so an English fallback shown there is
	// marked `lang="en"` instead of passing as a translation.
	returnEmptyString: false,
	interpolation: {
		// React escapes interpolated values already.
		escapeValue: false,
	},
});

/**
 * Switch the displayed language. The producer's locale arrives from the C++ side over the
 * CEF bridge rather than being detected in the browser, so no detector plugin is installed.
 */
export function changeLocale(locale: SupportedLocale): Promise<unknown> {
	return i18next.changeLanguage(locale);
}

export default i18next;
