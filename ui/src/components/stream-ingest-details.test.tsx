/**
 * @vitest-environment jsdom
 *
 * Tests for the ReaCast ingest details.
 *
 * The stream key is asserted to be on screen in plaintext, because requirement 16.2 says
 * so and the producer transcribes it by hand. There is no test that it is absent from the
 * console: requirement 26.4 is a discipline on this side rather than something TypeScript
 * can enforce, and a test asserting that one component did not log is a test that passes
 * for every component that has not been written yet.
 *
 * **Validates: Requirements 16.2, 16.3**
 */

import { act, cleanup, render } from '@testing-library/react';
import {
	afterEach, beforeEach, describe, expect, it, vi,
} from 'vitest';

import type { StreamIngestDisplay } from '../bridge/payloads';
import '../i18n';
import { StreamIngestDetails } from './stream-ingest-details';

afterEach(() => {
	cleanup();
	vi.useRealTimers();
});

const streamKey = 'sk_us-east-1_AbCdEf_0123456789';

function ingestDisplay(overrides: Partial<StreamIngestDisplay> = {}): StreamIngestDisplay {
	return {
		stageArn: 'arn:aws:ivs:us-east-1:111122223333:stage/AbCdEf',
		rtmpsIngestUrl: 'rtmps://a1b2c3.global-contribute.live-video.net:443/app/',
		streamKeyPlaintext: streamKey,
		participantId: 'participant-01JQ',
		expiresAt: '2025-01-01T12:00:00.000Z',
		reissued: false,
		...overrides,
	};
}

describe('the ReaCast ingest details', () => {
	beforeEach(() => {
		vi.useFakeTimers();
		vi.setSystemTime(new Date('2025-01-01T11:00:00.000Z'));
	});

	it('shows nothing when nothing is being streamed', () => {
		const { container } = render(<StreamIngestDetails ingest={null} />);

		expect(container.textContent).toBe('');
	});

	it('shows the endpoint and the publish token in full plaintext, with the expiry', () => {
		// Requirement 16.2. No reveal toggle: a masked field a producer has to unmask in
		// order to transcribe it is a worse secret and a worse experience at once.
		const { container } = render(<StreamIngestDetails ingest={ingestDisplay()} />);

		expect(container.textContent).toContain('rtmps://a1b2c3.global-contribute.live-video.net:443/app/');
		expect(container.querySelector('.stream-ingest-details__stream-key')?.textContent)
			.toBe(streamKey);
		expect(container.textContent).toContain('These details work until');
	});

	it('says when a reissued token replaced the one that was showing', () => {
		// Requirement 16.3. The reducer replaces rather than appends, so there is one set of
		// details on screen and this says it changed.
		const { container } = render(<StreamIngestDetails
			ingest={ingestDisplay({ reissued: true, streamKeyPlaintext: 'sk_us-east-1_Refreshed_99' })}
		/>);

		expect(container.textContent).toContain('These details were refreshed');
		expect(container.querySelector('.stream-ingest-details__stream-key')?.textContent)
			.toBe('sk_us-east-1_Refreshed_99');
		expect(container.textContent).not.toContain(streamKey);
	});

	it('says the details have expired once the publish token stops working', () => {
		const { container } = render(<StreamIngestDetails ingest={ingestDisplay()} />);

		expect(container.textContent).not.toContain('have expired');

		act(() => {
			vi.advanceTimersByTime(60 * 60 * 1_000 + 1_000);
		});

		expect(container.textContent).toContain('These streaming details have expired');
		expect(container.textContent).not.toContain('These details work until');
	});

	it('keeps showing the key past the expiry, for a producer part way through typing it', () => {
		const { container } = render(<StreamIngestDetails
			ingest={ingestDisplay({ expiresAt: '2025-01-01T10:00:00.000Z' })}
		/>);

		expect(container.querySelector('.stream-ingest-details__stream-key')?.textContent)
			.toBe(streamKey);
		expect(container.textContent).toContain('These streaming details have expired');
	});
});
