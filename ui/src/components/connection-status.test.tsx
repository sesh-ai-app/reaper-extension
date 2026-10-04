/**
 * @vitest-environment jsdom
 *
 * Tests for the connection status indicator.
 *
 * The assertions that matter are about *what the translation was selected by*: a test that
 * only checked the text would pass against a component rendering `notice`, which is the
 * mistake this file exists to catch.
 *
 * **Validates: Requirements 15.2, 23.1, 23.2, 23.3**
 */

import { cleanup, render, screen } from '@testing-library/react';
import {
	afterEach, describe, expect, it,
} from 'vitest';

import type { ConnectionStateView } from '../bridge/payloads';
import '../i18n';
import { ConnectionStatus } from './connection-status';

afterEach(cleanup);

/** The English the C++ side authors. Present on every publish, and never the message shown. */
const serverAuthoredNotice = 'Reconnecting to the Sesh AI service.';

function connectionView(overrides: Partial<ConnectionStateView> = {}): ConnectionStateView {
	return {
		state: 'connected',
		consecutiveFailedAttempts: 0,
		reconnectionDelayMilliseconds: 0,
		requiredProducerAction: 'none',
		notice: serverAuthoredNotice,
		...overrides,
	};
}

describe('the connection status', () => {
	it('shows nothing before a connection state has been published', () => {
		// The connection's own state cannot arrive over the connection, so there is nothing
		// known yet — and "disconnected" would be a guess shown during a healthy startup.
		const { container } = render(<ConnectionStatus connection={null} />);

		expect(container.textContent).toBe('');
	});

	it('says it is connected', () => {
		render(<ConnectionStatus connection={connectionView()} />);

		expect(screen.getByRole('status').textContent).toContain('Connected to your session');
	});

	it('says it is reconnecting, without rendering the English the server sent', () => {
		render(<ConnectionStatus connection={connectionView({ state: 'reconnecting' })} />);

		const status = screen.getByRole('status');

		expect(status.textContent).toContain('The connection dropped');
		expect(status.textContent).not.toContain(serverAuthoredNotice);
	});

	it('explains that another REAPER instance holds the session, and translates it', () => {
		// Requirement 23.2. The explanation is selected by `requiredProducerAction`, so it is
		// a translated string rather than the server's sentence.
		render(<ConnectionStatus
			connection={connectionView({
				state: 'disconnected',
				requiredProducerAction: 'close_the_other_reaper_instance',
				lastRejection: 'duplicate_client_type',
				notice: 'Another REAPER instance holds this session.',
			})}
		/>);

		const status = screen.getByRole('status');

		expect(status.textContent).toContain('Another REAPER instance is holding this session');
		expect(status.textContent).toContain('will not keep retrying on its own');
		expect(status.textContent).not.toContain('Another REAPER instance holds this session.');
	});

	it('asks the producer to sign in again', () => {
		// Requirement 23.3, after the refresh and its one retry both failed.
		render(<ConnectionStatus
			connection={connectionView({
				state: 'disconnected',
				requiredProducerAction: 'sign_in_again',
				lastRejection: 'invalid_or_expired_token',
			})}
		/>);

		expect(screen.getByRole('status').textContent).toContain('Sign in again');
	});

	it('falls back to the server sentence only when there is no action to describe', () => {
		// `unsupported_client_type` leaves the producer nothing to do, and the C++ side's
		// sentence is the only thing anywhere that says what happened. Marked `lang="en"` so
		// it is visibly a fallback rather than a translation that was forgotten.
		const { container } = render(<ConnectionStatus
			connection={connectionView({
				state: 'disconnected',
				notice: 'This build of the extension is not supported by the server.',
				lastRejection: 'unsupported_client_type',
			})}
		/>);

		const fallback = container.querySelector('.connection-status__server-notice');

		expect(fallback?.textContent).toBe('This build of the extension is not supported by the server.');
		expect(fallback?.getAttribute('lang')).toBe('en');
	});

	it('keeps the server sentence out of a connected status', () => {
		const { container } = render(<ConnectionStatus connection={connectionView()} />);

		expect(container.querySelector('.connection-status__server-notice')).toBeNull();
	});

	it('counts the failed attempts since the last connection', () => {
		render(<ConnectionStatus
			connection={connectionView({ state: 'reconnecting', consecutiveFailedAttempts: 3 })}
		/>);

		expect(screen.getByRole('status').textContent).toContain('Failed attempts since the last connection: 3');
	});

	it('says nothing about attempts when there have been none', () => {
		// A blip is not an outage, and a count of zero is not news.
		const { container } = render(<ConnectionStatus connection={connectionView()} />);

		expect(container.querySelector('.connection-status__failed-attempts')).toBeNull();
	});

	it('carries the handshake rejection as a data attribute rather than as prose', () => {
		// The schema is explicit that `lastRejection` is for diagnosis. A producer has no use
		// for the difference between a timeout and an unavailable auth service; a developer
		// with the inspector open does.
		const { container } = render(<ConnectionStatus
			connection={connectionView({
				state: 'reconnecting',
				lastRejection: 'authentication_service_unavailable',
			})}
		/>);

		const status = screen.getByRole('status');

		expect(status.getAttribute('data-last-rejection')).toBe('authentication_service_unavailable');
		expect(container.textContent).not.toContain('authentication_service_unavailable');
	});
});
