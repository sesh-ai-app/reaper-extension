/**
 * @vitest-environment jsdom
 *
 * Tests for the error banner.
 *
 * **Validates: Requirements 15.6, 23.2**
 */

import {
	cleanup, fireEvent, render, screen,
} from '@testing-library/react';
import {
	afterEach, describe, expect, it, vi,
} from 'vitest';

import type { StreamErrorView } from '../bridge/payloads';
import i18next from '../i18n';
import { StreamErrorBanner } from './stream-error-banner';

afterEach(cleanup);

afterEach(async () => {
	await i18next.changeLanguage('en');
});

function errorView(overrides: Partial<StreamErrorView> = {}): StreamErrorView {
	return {
		code: 'tool_execution_failed',
		message: 'The track could not be created.',
		envelopeType: 'request:tool',
		validationErrors: '',
		...overrides,
	};
}

describe('the error banner', () => {
	it('shows nothing when there is no error', () => {
		const { container } = render(<StreamErrorBanner error={null} dismiss={vi.fn()} />);

		expect(container.textContent).toBe('');
	});

	it('announces the error assertively', () => {
		render(<StreamErrorBanner error={errorView()} dismiss={vi.fn()} />);

		expect(screen.getByRole('alert').textContent).toContain('Something went wrong');
	});

	it('shows the server sentence for a code it has no translation for, marked as English', () => {
		// The code set is open — the schema bounds the pattern and enumerates nothing — so a
		// specific sentence from the server beats a generic one from here.
		const { container } = render(<StreamErrorBanner
			error={errorView({ code: 'a_code_this_build_has_never_seen' })}
			dismiss={vi.fn()}
		/>);

		const message = container.querySelector('.stream-error-banner__message');

		expect(message?.textContent).toBe('The track could not be created.');
		expect(message?.getAttribute('lang')).toBe('en');
	});

	it('prefers its own translation when it has one for the code', () => {
		const { container } = render(<StreamErrorBanner
			error={errorView({
				code: 'stage_creation_failed',
				message: 'IVS stage creation failed.',
			})}
			dismiss={vi.fn()}
		/>);

		const message = container.querySelector('.stream-error-banner__message');

		expect(message?.textContent).toContain('Audio streaming could not be set up');
		expect(message?.getAttribute('lang')).toBeNull();
	});

	it('uses the active language rather than English when the active language has the code', async () => {
		await i18next.changeLanguage('de');

		const { container } = render(<StreamErrorBanner
			error={errorView({
				code: 'stage_creation_failed',
				message: 'IVS stage creation failed.',
			})}
			dismiss={vi.fn()}
		/>);

		const message = container.querySelector('.stream-error-banner__message');

		expect(message?.textContent).toContain('Audio-Streaming konnte nicht eingerichtet werden');
		expect(message?.getAttribute('lang')).toBeNull();
	});

	it('marks the fallback as English when the active language has the key but no value for it', async () => {
		// The locale files are fully populated and a test in `src/i18n` keeps them that way,
		// so this is the shape of a regression rather than a state the bundle is in. It is
		// tested because the failure is silent: an empty value falls back to English, and
		// without the marker a screen reader reads that English in the panel's own voice.
		i18next.addResource('de', 'translation', 'streamError.byCode.untranslated_in_german', '');
		i18next.addResource('en', 'translation', 'streamError.byCode.untranslated_in_german', 'Streaming stopped.');

		await i18next.changeLanguage('de');

		const { container } = render(<StreamErrorBanner
			error={errorView({
				code: 'untranslated_in_german',
				message: 'The audio stream stopped.',
			})}
			dismiss={vi.fn()}
		/>);

		const message = container.querySelector('.stream-error-banner__message');

		expect(message?.textContent).toBe('The audio stream stopped.');
		expect(message?.getAttribute('lang')).toBe('en');
	});

	it('keeps the code, the envelope type, and the validation errors as diagnosis', () => {
		// Requirement 23.4's validation errors are prose for diagnosis, not news. They are
		// there for whoever wants them, behind a disclosure rather than in the banner.
		const { container } = render(<StreamErrorBanner
			error={errorView({
				code: 'payload_failed_schema',
				envelopeType: 'request:set_track_state',
				validationErrors: '/trackGuid: expected string, received number',
			})}
			dismiss={vi.fn()}
		/>);

		const diagnostics = container.querySelector('.stream-error-banner__diagnostics');

		expect(diagnostics?.textContent).toContain('Code: payload_failed_schema');
		expect(diagnostics?.textContent).toContain('While handling: request:set_track_state');
		expect(diagnostics?.textContent).toContain('/trackGuid: expected string, received number');
	});

	it('leaves out the envelope type and the validation errors when the error carried neither', () => {
		const { container } = render(<StreamErrorBanner
			error={errorView({ envelopeType: '', validationErrors: '' })}
			dismiss={vi.fn()}
		/>);

		expect(container.querySelector('.stream-error-banner__envelope-type')).toBeNull();
		expect(container.querySelector('.stream-error-banner__validation-errors')).toBeNull();
	});

	it('dismisses on request', () => {
		const dismiss = vi.fn();

		render(<StreamErrorBanner error={errorView()} dismiss={dismiss} />);

		fireEvent.click(screen.getByRole('button', { name: 'Dismiss' }));

		expect(dismiss).toHaveBeenCalledOnce();
	});
});
