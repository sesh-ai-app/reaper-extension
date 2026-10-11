/**
 * The generated ReaScripts offered this session (requirements 15.6, 26.2, ADR 0011).
 *
 * ---------------------------------------------------------------------------
 * A download, never an execution
 *
 * This is the whole of ADR 0011 as a component. The agent wrote Lua, the server stored it,
 * and the producer is handed a link to a file they read and run themselves. Nothing here
 * fetches the script, nothing evaluates it, and there is no "run" affordance to add later:
 * requirement 26.2 is that the extension executes no received code, and the one place that
 * rule could plausibly be bent is the one place it is written down.
 *
 * ---------------------------------------------------------------------------
 * `downloadUrl` is a credential
 *
 * It is a short-lived signed S3 URL, so anyone holding it can read the object. It goes in
 * an `href` and nowhere else: never into a log line, never into an error message, and not
 * into a React key either — keys do not reach the DOM, but a URL used as an identifier is
 * a URL that ends up in a diff, a test snapshot, or a debug render before long.
 *
 * Past `expiresAt` the link is not rendered as a link. A dead signed URL answers with an
 * S3 error document, which looks like the extension being broken rather than like a link
 * that timed out, so the stale state says what happened and what to do about it.
 */

import { useMemo } from 'react';
import { useTranslation } from 'react-i18next';

import type { ScriptDownloadPayload } from '../bridge/payloads';
import { formatExpiryTimestamp, parseExpiryTimestamp, useExpiryClock } from './expiry';

/** SI, so a producer reading `4.2 kB` sees the same number their file manager shows. */
const bytesPerKilobyte = 1_000;

/**
 * The size as a short string with a unit. The number goes through `Intl` so the decimal
 * separator is the producer's; the unit symbols are SI and the same in every locale the
 * extension ships, which is also why this needs no key with a plural category.
 */
export function describeSizeInBytes(sizeBytes: number, language: string): string {
	const format = (value: number) => new Intl.NumberFormat(language, {
		maximumFractionDigits: 1,
	}).format(value);

	if (sizeBytes < bytesPerKilobyte) {
		return `${format(sizeBytes)} B`;
	}

	if (sizeBytes < bytesPerKilobyte * bytesPerKilobyte) {
		return `${format(sizeBytes / bytesPerKilobyte)} kB`;
	}

	return `${format(sizeBytes / (bytesPerKilobyte * bytesPerKilobyte))} MB`;
}

interface ScriptDownloadProps {
	download: ScriptDownloadPayload;

	/** True once the signed URL's expiry has passed. */
	isStale: boolean;
}

function ScriptDownload({ download, isStale }: ScriptDownloadProps) {
	const { t, i18n } = useTranslation();

	const expiryText = formatExpiryTimestamp(download.expiresAt, i18n.language);

	return (
		<li className="script-download-list__download">
			<p className="script-download-list__file-name">{download.fileName}</p>

			{download.description !== undefined && download.description.length > 0 && (
				<p className="script-download-list__description">{download.description}</p>
			)}

			<p className="script-download-list__size">
				{t('scriptDownload.size', {
					size: describeSizeInBytes(download.sizeBytes, i18n.language),
				})}
			</p>

			{isStale
				? (
					<p className="script-download-list__expired" role="note">
						{t('scriptDownload.expired')}
					</p>
				)
				: (
					<>
						{expiryText !== null && (
							<p className="script-download-list__expiry">
								{t('scriptDownload.expiresAt', { time: expiryText })}
							</p>
						)}

						<a
							className="script-download-list__link"
							href={download.downloadUrl}
							download={download.fileName}
							rel="noreferrer noopener"
							aria-label={t('scriptDownload.downloadNamed', { fileName: download.fileName })}
						>
							{t('scriptDownload.download')}
						</a>
					</>
				)}
		</li>
	);
}

export interface ScriptDownloadListProps {
	/** `state.scriptDownloads` — every script offered this session, oldest first. */
	downloads: readonly ScriptDownloadPayload[];
}

export function ScriptDownloadList({ downloads }: ScriptDownloadListProps) {
	const { t } = useTranslation();

	// Parsed once per change to the list rather than per render, because the clock below
	// re-renders this component every time one of those expiries passes.
	const parsedDownloads = useMemo(
		() => downloads.map((download) => ({
			download,
			expiry: parseExpiryTimestamp(download.expiresAt),
		})),
		[downloads],
	);

	const expiries = useMemo(
		() => parsedDownloads
			.map((parsed) => parsed.expiry)
			.filter((expiry): expiry is number => expiry !== null),
		[parsedDownloads],
	);

	const now = useExpiryClock(expiries);

	if (downloads.length === 0) {
		return null;
	}

	return (
		<section className="script-download-list" aria-label={t('scriptDownload.regionLabel')}>
			<h2 className="script-download-list__heading">{t('scriptDownload.regionLabel')}</h2>

			<p className="script-download-list__explanation">{t('scriptDownload.explanation')}</p>

			<ol className="script-download-list__downloads">
				{parsedDownloads.map(({ download, expiry }, index) => (
					<ScriptDownload
						// Not `downloadUrl`: see the file comment. The list only grows at the end, so
						// the position is stable for as long as an entry exists.
						key={`${String(index)}:${download.fileName}`}
						download={download}
						isStale={expiry !== null && expiry <= now}
					/>
				))}
			</ol>
		</section>
	);
}
