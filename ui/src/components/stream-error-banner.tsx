/**
 * The last thing that failed, as a banner the producer can dismiss (requirements 15.6,
 * 23.2).
 *
 * `StreamErrorView` is where `stream:error` and all seven `error:*` envelope types land —
 * the view model half covering a protocol hole, since the `error:*` types have no bundled
 * payload schema of their own. So this is the general error surface, and the two errors
 * with a better home are deliberately not here: a handshake refusal is a connection state
 * and reads as one in `ConnectionStatus`, and a confirmation expiry is a resolution and
 * reads as one in `ConfirmationResolutionNotice`. An expiry shown as an error would say the
 * extension failed, when what happened is that the server concluded nothing should change.
 *
 * ---------------------------------------------------------------------------
 * Translated by `code`, falling back to the server's own sentence
 *
 * `code` is the machine-readable field and `message` is English the server wrote. The
 * schema bounds `code` to a pattern but enumerates nothing, so the set is open and a
 * translation cannot exist for every member of it. Each code this build has a translation
 * for gets it; the rest get the server's sentence, marked `lang="en"` so a screen reader
 * reading a translated panel switches voice rather than pronouncing English as German.
 * The alternative — a generic "something went wrong" for every code without a key — would
 * throw away the only specific thing the producer was told.
 *
 * The question asked is whether the *active language* has a translation of its own, which
 * is not what `i18n.exists` answers. `exists` consults the fallback bundle and counts a
 * present-but-empty value as present, so for a code translated in English and not yet in
 * German it answers true — and the panel would then render English prose with no
 * `lang="en"` on it, which is the one outcome this attribute exists to prevent. Reading
 * the active language's own resource makes the marker follow from what is actually being
 * displayed rather than from an invariant held somewhere else: the locale files are all
 * fully populated and a test keeps them that way, but the accessibility of this one line
 * should not depend on that staying true.
 *
 * `validationErrors` and `envelopeType` are diagnosis rather than news (requirement 23.4
 * asks for the validation errors to be logged, and this is where a producer can read them
 * back to support). They sit inside a collapsed `details`, so the banner says one thing
 * and the detail is there for whoever wants it.
 */

import { useTranslation } from 'react-i18next';

import type { StreamErrorView } from '../bridge/payloads';

/** Where a code's translation lives, when this build has one. */
function translationKeyForErrorCode(code: string): string {
	return `streamError.byCode.${code}`;
}

export interface StreamErrorBannerProps {
	/** `state.streamError` — the presenter's last error, or `null`. One, not a list. */
	error: StreamErrorView | null;

	/**
	 * The conversation hook's `dismissStreamError`. It stops showing the error; the
	 * presenter still holds it, and the reducer drops it when the next prompt goes out.
	 */
	dismiss: () => void;
}

export function StreamErrorBanner({ error, dismiss }: StreamErrorBannerProps) {
	const { t, i18n } = useTranslation();

	if (error === null) {
		return null;
	}

	const codeKey = translationKeyForErrorCode(error.code);

	// `getResource` reads the named language's bundle and nothing else — no fallback, no
	// interpolation — so an absent key and an empty one both answer "not translated here".
	const translationForCode: unknown = i18n.getResource(i18n.language, 'translation', codeKey);
	const isTranslated = typeof translationForCode === 'string' && translationForCode.trim().length > 0;

	return (
		<div className="stream-error-banner" role="alert">
			<p className="stream-error-banner__heading">{t('streamError.heading')}</p>

			<p
				className="stream-error-banner__message"
				lang={isTranslated ? undefined : 'en'}
			>
				{isTranslated ? t(codeKey) : error.message}
			</p>

			<details className="stream-error-banner__diagnostics">
				<summary>{t('streamError.diagnosticsLabel')}</summary>

				<p className="stream-error-banner__code">
					{t('streamError.code', { code: error.code })}
				</p>

				{error.envelopeType.length > 0 && (
					<p className="stream-error-banner__envelope-type">
						{t('streamError.whileHandling', { envelopeType: error.envelopeType })}
					</p>
				)}

				{error.validationErrors.length > 0 && (
					<pre className="stream-error-banner__validation-errors">{error.validationErrors}</pre>
				)}
			</details>

			<button type="button" className="stream-error-banner__dismiss" onClick={dismiss}>
				{t('streamError.dismiss')}
			</button>
		</div>
	);
}
