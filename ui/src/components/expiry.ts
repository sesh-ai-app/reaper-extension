/**
 * Expiry timestamps, shared by the two components that show something which stops working:
 * the ReaScript download's signed URL and the ReaCast publish token.
 *
 * Both are credentials with a server-side lifetime the extension neither sets nor
 * enforces, which is why the rule here is to *show* the expiry rather than act on it. A
 * download whose URL has expired is presented as stale instead of as a link, because the
 * link no longer does anything and a failed click looks like the extension being broken.
 *
 * `useExpiryClock` exists because an expiry passes without anything arriving over the
 * bridge. It schedules one timeout at the earliest expiry still in the future rather than
 * polling: a panel docked in REAPER for a session should not re-render on a timer for the
 * whole of it, and the only moment the display changes is the moment an expiry passes.
 */

import { useEffect, useState } from 'react';

/**
 * `setTimeout` stores its delay in a signed 32-bit integer, so a longer delay wraps and
 * fires immediately. An expiry further out than this is waited for in one hop of this
 * length, after which the effect reschedules — which is correct rather than merely safe,
 * since `Date.now()` is read again on each hop.
 */
const maximumTimeoutDelayMilliseconds = 2_147_483_647;

/**
 * Epoch milliseconds for an ISO-8601 timestamp, or `null` when it does not parse.
 *
 * Null is the honest answer and the callers treat it as "no expiry to show" rather than as
 * expired: a timestamp this build cannot read says nothing about whether the credential
 * still works, and refusing a download over an unparseable string would withhold something
 * the producer asked for on the strength of a guess.
 */
export function parseExpiryTimestamp(isoTimestamp: string): number | null {
	if (isoTimestamp.length === 0) {
		return null;
	}

	const parsed = Date.parse(isoTimestamp);

	return Number.isNaN(parsed) ? null : parsed;
}

/** The expiry as a date and time in the producer's language, or `null` when it does not parse. */
export function formatExpiryTimestamp(isoTimestamp: string, language: string): string | null {
	const parsed = parseExpiryTimestamp(isoTimestamp);

	if (parsed === null) {
		return null;
	}

	return new Date(parsed).toLocaleString(language, { dateStyle: 'short', timeStyle: 'short' });
}

/**
 * The current time, re-read when the next of the given expiries passes.
 *
 * Answers epoch milliseconds so a caller compares rather than branches on a boolean this
 * hook guessed at. Pass only timestamps that parsed; an empty list schedules nothing.
 */
export function useExpiryClock(expiryTimestamps: readonly number[]): number {
	const [now, setNow] = useState(() => Date.now());

	const earliestFutureExpiry = expiryTimestamps.reduce(
		(earliest, expiry) => (expiry > now && expiry < earliest ? expiry : earliest),
		Number.POSITIVE_INFINITY,
	);

	useEffect(() => {
		if (!Number.isFinite(earliestFutureExpiry)) {
			return undefined;
		}

		// The extra millisecond puts the wake-up strictly past the expiry, so the comparison
		// the caller makes on the new time cannot land exactly on the boundary and leave the
		// same expiry scheduled again.
		const delay = Math.min(
			Math.max(earliestFutureExpiry - Date.now(), 0) + 1,
			maximumTimeoutDelayMilliseconds,
		);

		const timeout = setTimeout(() => {
			setNow(Date.now());
		}, delay);

		return () => {
			clearTimeout(timeout);
		};
	}, [earliestFutureExpiry]);

	return now;
}
