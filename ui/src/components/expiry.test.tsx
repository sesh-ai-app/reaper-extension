/**
 * @vitest-environment jsdom
 *
 * Tests for the expiry helpers the download and the ingest details share.
 *
 * `useExpiryClock` is exercised through a component, because what it is for is a render
 * that happens without anything arriving over the bridge — and the thing worth asserting
 * is that one timeout is scheduled per expiry rather than a timer polling for the life of
 * the panel.
 *
 * **Validates: Requirements 15.6, 16.2**
 */

import { act, cleanup, render } from '@testing-library/react';
import {
	afterEach, beforeEach, describe, expect, it, vi,
} from 'vitest';

import { formatExpiryTimestamp, parseExpiryTimestamp, useExpiryClock } from './expiry';

afterEach(() => {
	cleanup();
	vi.useRealTimers();
});

describe('parsing an expiry', () => {
	it('reads an ISO-8601 timestamp as epoch milliseconds', () => {
		expect(parseExpiryTimestamp('2025-01-01T12:00:00.000Z')).toBe(1_735_732_800_000);
	});

	it('answers null for a timestamp it cannot read, and for an empty one', () => {
		// Null means "nothing to show" rather than "expired": a string this build cannot read
		// says nothing about whether the credential still works.
		expect(parseExpiryTimestamp('whenever')).toBeNull();
		expect(parseExpiryTimestamp('')).toBeNull();
	});
});

describe('formatting an expiry', () => {
	it('writes it in the producer language', () => {
		const formatted = formatExpiryTimestamp('2025-01-01T12:00:00.000Z', 'en');

		expect(formatted).toContain('1/1/25');
	});

	it('answers null rather than a placeholder date for an unreadable timestamp', () => {
		expect(formatExpiryTimestamp('whenever', 'en')).toBeNull();
	});
});

interface ClockProbeProps {
	expiryTimestamps: readonly number[];
}

function ClockProbe({ expiryTimestamps }: ClockProbeProps) {
	const now = useExpiryClock(expiryTimestamps);

	return <p>{expiryTimestamps.filter((expiry) => expiry <= now).length}</p>;
}

describe('the expiry clock', () => {
	beforeEach(() => {
		vi.useFakeTimers();
		vi.setSystemTime(new Date('2025-01-01T00:00:00.000Z'));
	});

	it('schedules nothing when there is no expiry to wait for', () => {
		render(<ClockProbe expiryTimestamps={[]} />);

		expect(vi.getTimerCount()).toBe(0);
	});

	it('wakes once per expiry, in order, and not in between', () => {
		const firstExpiry = Date.parse('2025-01-01T00:10:00.000Z');
		const secondExpiry = Date.parse('2025-01-01T00:20:00.000Z');

		const { container } = render(
			<ClockProbe expiryTimestamps={[secondExpiry, firstExpiry]} />,
		);

		expect(container.textContent).toBe('0');
		expect(vi.getTimerCount()).toBe(1);

		act(() => {
			vi.advanceTimersByTime(9 * 60 * 1_000);
		});

		expect(container.textContent).toBe('0');

		act(() => {
			vi.advanceTimersByTime(2 * 60 * 1_000);
		});

		expect(container.textContent).toBe('1');

		act(() => {
			vi.advanceTimersByTime(10 * 60 * 1_000);
		});

		expect(container.textContent).toBe('2');
		expect(vi.getTimerCount()).toBe(0);
	});
});
