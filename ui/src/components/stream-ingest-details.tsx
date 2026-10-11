/**
 * The ReaCast ingest endpoint and publish token (requirements 16.2, 16.3).
 *
 * The producer types these two values into ReaCast by hand, which is why the token is in
 * full plaintext and why there is no reveal toggle: requirement 16.2 says plaintext by
 * default, and a masked field a producer has to unmask to transcribe is a worse secret and
 * a worse experience at the same time.
 *
 * ---------------------------------------------------------------------------
 * `streamKeyPlaintext` is a credential, and TypeScript cannot stop this file leaking it
 *
 * Requirement 26.4 forbids logging the IVS stream key. The C++ side enforces that
 * structurally — `stream_ingest_display.h` deletes the stream insertion operator for the
 * type, so a `std::cout << display` does not compile. TypeScript has no equivalent: there
 * is no way to make `console.log(ingest)` a type error. So it is a discipline, stated here
 * because this is the one file that holds the value:
 *
 *   - the payload is never passed to `console`, in any form, including while debugging;
 *   - it never appears in an error message, which is read back to support;
 *   - it is never stored anywhere the producer did not ask for — no `localStorage`, no
 *     clipboard write they did not initiate, no analytics.
 *
 * It is rendered on screen on purpose. That is not a contradiction of the above: the
 * producer is looking at their own session on their own machine, and the rule is about the
 * value outliving the moment or travelling somewhere they cannot see.
 *
 * ---------------------------------------------------------------------------
 * A reissued token replaces the displayed one
 *
 * Requirement 16.3, and it is the reducer that does it: `view:stream_ingest` replaces
 * rather than appends, including when `reissued` is true. This component shows one set of
 * details and says when they were refreshed, because two sets on screen is a producer
 * transcribing the wrong one.
 */

import { useTranslation } from 'react-i18next';

import type { StreamIngestDisplay } from '../bridge/payloads';
import { formatExpiryTimestamp, parseExpiryTimestamp, useExpiryClock } from './expiry';

export interface StreamIngestDetailsProps {
	/** `state.streamIngest` — the current details, or `null` when nothing is being streamed. */
	ingest: StreamIngestDisplay | null;
}

export function StreamIngestDetails({ ingest }: StreamIngestDetailsProps) {
	const { t, i18n } = useTranslation();

	const expiry = ingest === null ? null : parseExpiryTimestamp(ingest.expiresAt);
	const now = useExpiryClock(expiry === null ? [] : [expiry]);

	if (ingest === null) {
		return null;
	}

	const expiryText = formatExpiryTimestamp(ingest.expiresAt, i18n.language);
	const hasExpired = expiry !== null && expiry <= now;

	return (
		<section className="stream-ingest-details" aria-label={t('streamIngest.regionLabel')}>
			<h2 className="stream-ingest-details__heading">{t('streamIngest.regionLabel')}</h2>

			<p className="stream-ingest-details__explanation">{t('streamIngest.explanation')}</p>

			{ingest.reissued && (
				<p className="stream-ingest-details__reissued" role="note">
					{t('streamIngest.reissued')}
				</p>
			)}

			<dl className="stream-ingest-details__fields">
				<dt>{t('streamIngest.ingestUrlLabel')}</dt>
				<dd>
					<code className="stream-ingest-details__ingest-url">{ingest.rtmpsIngestUrl}</code>
				</dd>

				<dt>{t('streamIngest.streamKeyLabel')}</dt>
				<dd>
					{/* Plaintext on purpose — requirement 16.2. Never logged — requirement 26.4. */}
					<code className="stream-ingest-details__stream-key">{ingest.streamKeyPlaintext}</code>
				</dd>

				<dt>{t('streamIngest.participantLabel')}</dt>
				<dd>
					<code className="stream-ingest-details__participant">{ingest.participantId}</code>
				</dd>
			</dl>

			{hasExpired && (
				<p className="stream-ingest-details__expired" role="note">
					{t('streamIngest.expired')}
				</p>
			)}

			{!hasExpired && expiryText !== null && (
				<p className="stream-ingest-details__expiry">
					{t('streamIngest.expiresAt', { time: expiryText })}
				</p>
			)}
		</section>
	);
}
