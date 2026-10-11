/**
 * Structural tests over the seven locale files (requirement 15.3).
 *
 * These are worth more than reviewing any single translation, because they catch the
 * failures a reviewer reading one language cannot see: a key that exists in English and
 * nowhere else, a value left empty, an interpolation placeholder renamed or dropped in
 * translation so the producer reads a sentence with a hole in it.
 *
 * English is the reference. `fallbackLng` is `en` and `returnEmptyString` is false, so a
 * missing or empty value does not render blank — it renders English. That is a safe
 * failure mode and a silent one, which is exactly why it needs a test rather than a
 * reviewer noticing the panel looks fine.
 *
 * ---------------------------------------------------------------------------
 * No plural keys, deliberately
 *
 * Nothing in this bundle has a plural category, and the no-plural test keeps it that way.
 * Japanese has a single plural category where English has two and Portuguese has two with
 * different boundaries, so a `_one` / `_other` pair authored in English does not survive
 * translation without each locale's own category set being right. The copy avoids the
 * problem instead: countdowns are formatted `m:ss` in `confirmation-prompt.tsx`, byte
 * sizes go through `Intl.NumberFormat` with SI symbols in `script-download-list.tsx`, and
 * the counts that remain are phrased as labelled values ("Failed attempts since the last
 * connection: 3") rather than as sentences that have to agree with a number.
 *
 * A new string that genuinely needs a plural is a decision to make deliberately, with all
 * seven category sets filled in. It is not something to discover from a failing test in a
 * language nobody on the team reads.
 */

import { describe, expect, it } from 'vitest';

import germanTranslation from './locales/de.json';
import englishTranslation from './locales/en.json';
import spanishTranslation from './locales/es.json';
import frenchTranslation from './locales/fr.json';
import italianTranslation from './locales/it.json';
import japaneseTranslation from './locales/ja.json';
import brazilianPortugueseTranslation from './locales/pt-BR.json';

/** i18next's own suffixes for plural categories, which this bundle has none of. */
const pluralCategorySuffixes = ['_zero', '_one', '_two', '_few', '_many', '_other'];

interface TranslationTree {
	[key: string]: string | TranslationTree;
}

/** Every leaf as a dot-joined key, matching the key separator i18next is left defaulted to. */
function flattenTranslations(tree: TranslationTree, prefix = ''): Map<string, string> {
	const flattened = new Map<string, string>();

	Object.entries(tree).forEach(([key, value]) => {
		const path = prefix.length === 0 ? key : `${prefix}.${key}`;

		if (typeof value === 'string') {
			flattened.set(path, value);

			return;
		}

		flattenTranslations(value, path).forEach((nestedValue, nestedPath) => {
			flattened.set(nestedPath, nestedValue);
		});
	});

	return flattened;
}

/** The `{{name}}` placeholders in a string, sorted so two strings compare by set. */
function interpolationPlaceholders(value: string): string[] {
	return [...value.matchAll(/\{\{\s*([^}\s]+)\s*\}\}/g)]
		.map((match) => match[1] ?? '')
		.sort();
}

const english = flattenTranslations(englishTranslation);

/**
 * The six translated locales. English is the reference and is tested separately, for the
 * properties that are about English itself rather than about agreeing with it.
 */
const translatedLocales = [
	['de', germanTranslation],
	['ja', japaneseTranslation],
	['pt-BR', brazilianPortugueseTranslation],
	['es', spanishTranslation],
	['fr', frenchTranslation],
	['it', italianTranslation],
] as const satisfies readonly (readonly [string, TranslationTree])[];

describe('the English reference bundle', () => {
	it('has no empty value', () => {
		const emptyKeys = [...english.entries()]
			.filter(([, value]) => value.trim().length === 0)
			.map(([key]) => key);

		expect(emptyKeys).toEqual([]);
	});

	it('has no plural key', () => {
		const pluralKeys = [...english.keys()]
			.filter((key) => pluralCategorySuffixes.some((suffix) => key.endsWith(suffix)));

		expect(pluralKeys).toEqual([]);
	});

	it('interpolates no count, which is the field a plural category would be selected by', () => {
		const countKeys = [...english.entries()]
			.filter(([, value]) => interpolationPlaceholders(value).includes('count'))
			.map(([key]) => key);

		expect(countKeys).toEqual([]);
	});
});

describe.each(translatedLocales)('the %s bundle', (_locale, translation) => {
	const translated = flattenTranslations(translation);

	it('carries exactly the keys English carries', () => {
		// Sorted rather than compared as sets, so a failure names the keys that differ.
		expect([...translated.keys()].sort()).toEqual([...english.keys()].sort());
	});

	it('has no empty value', () => {
		// An empty value is not a blank panel — `returnEmptyString: false` falls back to
		// English — but it is English shown as though it were a translation. The error banner
		// in particular decides whether to mark its message `lang="en"` from whether this
		// build has a translation for the code, and a key that exists but is empty makes that
		// decision wrong: a screen reader then reads English prose in a German voice.
		const emptyKeys = [...translated.entries()]
			.filter(([, value]) => value.trim().length === 0)
			.map(([key]) => key);

		expect(emptyKeys).toEqual([]);
	});

	it('keeps every interpolation placeholder English uses', () => {
		const mismatched = [...english.entries()]
			.filter(([key, englishValue]) => {
				const translatedValue = translated.get(key) ?? '';

				return interpolationPlaceholders(translatedValue).join(',')
					!== interpolationPlaceholders(englishValue).join(',');
			})
			.map(([key]) => key);

		expect(mismatched).toEqual([]);
	});

	it('has no plural key', () => {
		const pluralKeys = [...translated.keys()]
			.filter((key) => pluralCategorySuffixes.some((suffix) => key.endsWith(suffix)));

		expect(pluralKeys).toEqual([]);
	});

	it('leaves the product name untranslated', () => {
		expect(translated.get('application.name')).toBe('Sesh AI');
		expect(translated.get('conversation.assistantLabel')).toBe('Sesh AI');
	});
});
