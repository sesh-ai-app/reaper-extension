/**
 * @vitest-environment jsdom
 *
 * Tests for the ReaScript downloads.
 *
 * The assertion this file is really for is the negative one: nothing in the rendered
 * output asks REAPER to run anything, and the signed URL appears in exactly one place.
 *
 * **Validates: Requirements 15.6, 26.2**
 */

import { act, cleanup, render } from '@testing-library/react';
import {
	afterEach, beforeEach, describe, expect, it, vi,
} from 'vitest';

import type { ScriptDownloadPayload } from '../bridge/payloads';
import '../i18n';
import { describeSizeInBytes, ScriptDownloadList } from './script-download-list';

afterEach(() => {
	cleanup();
	vi.useRealTimers();
});

/** Stands in for the short-lived signed S3 URL. A credential: it belongs in an `href` and nowhere else. */
const signedUrl = 'https://scripts.example.com/generated/take-cleanup.lua?X-Amz-Signature=abc123';

function scriptDownload(overrides: Partial<ScriptDownloadPayload> = {}): ScriptDownloadPayload {
	return {
		downloadUrl: signedUrl,
		fileName: 'take-cleanup.lua',
		sizeBytes: 4_200,
		expiresAt: '2025-01-01T12:00:00.000Z',
		...overrides,
	};
}

describe('the script size', () => {
	it('reads in bytes, kilobytes, and megabytes', () => {
		expect(describeSizeInBytes(512, 'en')).toBe('512 B');
		expect(describeSizeInBytes(4_200, 'en')).toBe('4.2 kB');
		expect(describeSizeInBytes(2_500_000, 'en')).toBe('2.5 MB');
	});

	it('writes the number with the decimal separator of the producer language', () => {
		expect(describeSizeInBytes(4_200, 'de')).toBe('4,2 kB');
	});
});

describe('the script download list', () => {
	beforeEach(() => {
		vi.useFakeTimers();
		vi.setSystemTime(new Date('2025-01-01T11:00:00.000Z'));
	});

	it('shows nothing when no script has been offered', () => {
		const { container } = render(<ScriptDownloadList downloads={[]} />);

		expect(container.textContent).toBe('');
	});

	it('offers a download, and says the extension does not run it', () => {
		// ADR 0011 and requirement 26.2, as the one thing this component has to get right.
		const { container } = render(<ScriptDownloadList downloads={[scriptDownload()]} />);

		const link = container.querySelector('a');

		expect(link?.getAttribute('href')).toBe(signedUrl);
		expect(link?.getAttribute('download')).toBe('take-cleanup.lua');
		expect(container.textContent).toContain('The extension does not run it');
		expect(container.textContent).toContain('Size: 4.2 kB');
	});

	it('names each download by its file name, for a list of several', () => {
		const { container } = render(<ScriptDownloadList
			downloads={[
				scriptDownload(),
				scriptDownload({ fileName: 'tempo-map.lua', downloadUrl: `${signedUrl}&second` }),
			]}
		/>);

		const labels = [...container.querySelectorAll('a')]
			.map((link) => link.getAttribute('aria-label'));

		expect(labels).toEqual(['Download take-cleanup.lua', 'Download tempo-map.lua']);
	});

	it('keeps the signed URL out of everything except the href', () => {
		const { container } = render(<ScriptDownloadList downloads={[scriptDownload()]} />);

		expect(container.textContent).not.toContain('X-Amz-Signature');

		const elementsCarryingTheUrl = [...container.querySelectorAll('*')].filter(
			(element) => [...element.attributes].some(
				(attribute) => attribute.name !== 'href' && attribute.value.includes(signedUrl),
			),
		);

		expect(elementsCarryingTheUrl).toEqual([]);
	});

	it('shows the description when the payload carried one', () => {
		const { container } = render(<ScriptDownloadList
			downloads={[scriptDownload({ description: 'Removes empty takes across the comp' })]}
		/>);

		expect(container.textContent).toContain('Removes empty takes across the comp');
	});

	it('shows the expiry while the link still works', () => {
		const { container } = render(<ScriptDownloadList downloads={[scriptDownload()]} />);

		expect(container.textContent).toContain('This download link works until');
	});

	it('presents the download as stale once the signed URL has expired', () => {
		const { container } = render(<ScriptDownloadList
			downloads={[scriptDownload({ expiresAt: '2025-01-01T10:00:00.000Z' })]}
		/>);

		expect(container.querySelector('a')).toBeNull();
		expect(container.textContent).toContain('This download link has expired');
	});

	it('goes stale as the expiry passes, with nothing arriving over the bridge', () => {
		const { container } = render(<ScriptDownloadList downloads={[scriptDownload()]} />);

		expect(container.querySelector('a')).toBeTruthy();

		act(() => {
			vi.advanceTimersByTime(60 * 60 * 1_000 + 1_000);
		});

		expect(container.querySelector('a')).toBeNull();
		expect(container.textContent).toContain('This download link has expired');
	});

	it('still offers the download when the expiry does not parse', () => {
		// An unreadable timestamp says nothing about whether the URL still works, so the
		// download is not withheld on the strength of a guess.
		const { container } = render(<ScriptDownloadList
			downloads={[scriptDownload({ expiresAt: 'whenever' })]}
		/>);

		expect(container.querySelector('a')).toBeTruthy();
		expect(container.textContent).not.toContain('works until');
	});
});
