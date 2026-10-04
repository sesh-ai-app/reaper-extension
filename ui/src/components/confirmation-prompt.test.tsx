/**
 * @vitest-environment jsdom
 *
 * Tests for the confirmation prompt, its countdown, and the resolution notice.
 *
 * The countdown runs on fake timers, and the two assertions worth having are the ones
 * about what reaching zero does *not* do: it does not dismiss the prompt, and it does not
 * send anything. The extension does not enforce the window; the server does.
 *
 * **Validates: Requirements 13.1, 13.2, 13.3, 13.4, 13.5, 23.7**
 */

import {
	act, cleanup, fireEvent, render, screen,
} from '@testing-library/react';
import {
	afterEach, beforeEach, describe, expect, it, vi,
} from 'vitest';

import type {
	ConfirmationResolutionView,
	PendingConfirmationView,
} from '../bridge/payloads';
import type { BridgeSendOutcome } from '../bridge/send';
import '../i18n';
import { ConfirmationPrompt } from './confirmation-prompt';

afterEach(() => {
	cleanup();
	vi.useRealTimers();
});

const sent: BridgeSendOutcome = { messageName: 'confirm:approve', disposition: 'sent' };

const refused: BridgeSendOutcome = {
	messageName: 'confirm:approve',
	disposition: 'transport_unavailable',
	reason: 'the renderer binding was not installed',
};

function pendingConfirmation(
	overrides: Partial<PendingConfirmationView> = {},
): PendingConfirmationView {
	return {
		requestId: 'request-1',
		actionSummary: 'Delete the four takes under the vocal comp',
		details: 'Affects the track named Lead Vocal and four media items between 1:02 and 1:48.',
		riskLevel: 'high',
		riskLevelReported: true,
		confirmationWindowSeconds: 120,
		descriptionComplete: true,
		...overrides,
	};
}

function resolutionView(
	overrides: Partial<ConfirmationResolutionView> = {},
): ConfirmationResolutionView {
	return {
		requestId: 'request-1',
		resolution: 'approved',
		answeringClient: 'extension',
		answeredByThisClient: true,
		contradictsLocalVerdict: false,
		nothingWasChanged: false,
		...overrides,
	};
}

interface PromptHarnessOptions {
	confirmation?: PendingConfirmationView | null;
	answerSubmitted?: boolean;
	resolution?: ConfirmationResolutionView | null;
	approve?: (requestId: string) => BridgeSendOutcome;
	reject?: (requestId: string) => BridgeSendOutcome;
}

function renderPrompt(options: PromptHarnessOptions = {}) {
	const {
		confirmation = pendingConfirmation(),
		answerSubmitted = false,
		resolution = null,
		approve = () => sent,
		reject = () => sent,
	} = options;

	return render(<ConfirmationPrompt
		confirmation={confirmation}
		answerSubmitted={answerSubmitted}
		resolution={resolution}
		approve={approve}
		reject={reject}
	/>);
}

describe('the pending confirmation prompt', () => {
	it('shows nothing when nothing is pending and nothing has resolved', () => {
		const { container } = renderPrompt({ confirmation: null });

		expect(container.textContent).toBe('');
	});

	it('renders the summary, the risk level, and the details', () => {
		// Requirement 13.1.
		const dialog = renderPrompt().container;

		expect(screen.getByRole('alertdialog')).toBeTruthy();
		expect(dialog.textContent).toContain('Delete the four takes under the vocal comp');
		expect(dialog.textContent).toContain('Risk: high');
		expect(dialog.textContent).toContain('Lead Vocal');
	});

	it('names itself to assistive technology by its heading and its summary', () => {
		renderPrompt();

		const dialog = screen.getByRole('alertdialog');
		const headingIdentifier = dialog.getAttribute('aria-labelledby') ?? '';
		const summaryIdentifier = dialog.getAttribute('aria-describedby') ?? '';

		// `useId` produces identifiers with characters a selector would have to escape, so
		// they are looked up by identifier rather than through `querySelector`.
		expect(document.getElementById(headingIdentifier)?.textContent).toBe('Approve this action?');
		expect(document.getElementById(summaryIdentifier)?.textContent)
			.toContain('Delete the four takes');
	});

	it('takes focus when it appears, on the prompt rather than on a button', () => {
		// A producer mid-keystroke should not be able to approve a destructive operation with
		// a key they had already pressed for something else.
		renderPrompt();

		expect(document.activeElement).toBe(screen.getByRole('alertdialog'));
	});

	it('says when the risk level is the extension assuming the worst rather than an assessment', () => {
		renderPrompt({ confirmation: pendingConfirmation({ riskLevelReported: false }) });

		const notes = screen.getAllByRole('note').map((note) => note.textContent ?? '');

		expect(notes.some((note) => note.includes("extension's own cautious assumption"))).toBe(true);
	});

	it('says nothing about the risk level when the agent reported it', () => {
		const { container } = renderPrompt();

		expect(container.querySelector('.confirmation-prompt__risk-not-reported')).toBeNull();
	});

	it('says when the request could not be fully described', () => {
		renderPrompt({ confirmation: pendingConfirmation({ descriptionComplete: false }) });

		const notes = screen.getAllByRole('note').map((note) => note.textContent ?? '');

		expect(notes.some((note) => note.includes('may not cover everything'))).toBe(true);
	});

	it('says so when no detail arrived rather than leaving the space blank', () => {
		const { container } = renderPrompt({ confirmation: pendingConfirmation({ details: '' }) });

		expect(container.textContent).toContain('No further detail arrived');
	});

	it('sends an approval carrying the requestId', () => {
		// Requirement 13.2.
		const approve = vi.fn(() => sent);

		renderPrompt({ approve });

		fireEvent.click(screen.getByRole('button', { name: 'Approve' }));

		expect(approve).toHaveBeenCalledWith('request-1');
	});

	it('sends a rejection carrying the requestId', () => {
		// Requirement 13.3.
		const reject = vi.fn(() => sent);

		renderPrompt({ reject });

		fireEvent.click(screen.getByRole('button', { name: 'Reject' }));

		expect(reject).toHaveBeenCalledWith('request-1');
	});

	it('quiets the buttons but keeps the prompt up once an answer is in flight', () => {
		// Only `confirm:resolved` establishes how it ended, so the prompt stays until the
		// reducer takes it down.
		renderPrompt({ answerSubmitted: true });

		expect(screen.getByRole('alertdialog')).toBeTruthy();
		expect(screen.getByRole('button', { name: 'Approve' }).hasAttribute('disabled')).toBe(true);
		expect(screen.getByRole('button', { name: 'Reject' }).hasAttribute('disabled')).toBe(true);
		expect(screen.getByRole('alertdialog').textContent).toContain('Your answer has been sent');
	});

	it('says so when the answer never left the panel, and leaves the buttons live', () => {
		renderPrompt({ approve: () => refused });

		fireEvent.click(screen.getByRole('button', { name: 'Approve' }));

		expect(screen.getByRole('alert').textContent).toContain('did not leave the panel');
		expect(screen.getByRole('button', { name: 'Approve' }).hasAttribute('disabled')).toBe(false);
	});
});

describe('the confirmation countdown', () => {
	beforeEach(() => {
		vi.useFakeTimers();
	});

	it('counts down from the window the view carried, not from a constant', () => {
		// Requirement 13.4. A 90-second window counts from 1:30, because the window is the
		// server's and a 120 here would be a second copy of it going stale silently.
		const { container } = renderPrompt({
			confirmation: pendingConfirmation({ confirmationWindowSeconds: 90 }),
		});

		expect(container.textContent).toContain('Time left to answer: 1:30');

		act(() => {
			vi.advanceTimersByTime(31_000);
		});

		expect(container.textContent).toContain('Time left to answer: 0:59');
	});

	it('keeps the prompt and the buttons up when the countdown reaches zero', () => {
		const approve = vi.fn(() => sent);

		const { container } = renderPrompt({
			confirmation: pendingConfirmation({ confirmationWindowSeconds: 5 }),
			approve,
		});

		act(() => {
			vi.advanceTimersByTime(10_000);
		});

		// Nothing was sent and nothing was dismissed: the server owns the window, and a
		// producer tapping Approve as it closes is told their answer did not take effect by
		// the resolution rather than by this component guessing.
		expect(approve).not.toHaveBeenCalled();
		expect(screen.getByRole('alertdialog')).toBeTruthy();
		expect(container.textContent).toContain('The time to answer has run out');
		expect(screen.getByRole('button', { name: 'Approve' }).hasAttribute('disabled')).toBe(false);

		fireEvent.click(screen.getByRole('button', { name: 'Approve' }));

		expect(approve).toHaveBeenCalledWith('request-1');
	});

	it('restarts the countdown when a newer confirmation supersedes the one on screen', () => {
		const { container, rerender } = render(<ConfirmationPrompt
			confirmation={pendingConfirmation({ confirmationWindowSeconds: 60 })}
			answerSubmitted={false}
			resolution={null}
			approve={() => sent}
			reject={() => sent}
		/>);

		act(() => {
			vi.advanceTimersByTime(30_000);
		});

		expect(container.textContent).toContain('Time left to answer: 0:30');

		rerender(<ConfirmationPrompt
			confirmation={pendingConfirmation({
				requestId: 'request-2',
				confirmationWindowSeconds: 60,
			})}
			answerSubmitted={false}
			resolution={null}
			approve={() => sent}
			reject={() => sent}
		/>);

		expect(container.textContent).toContain('Time left to answer: 1:00');
	});

	it('shows no countdown when the view carried no window', () => {
		const { container } = renderPrompt({
			confirmation: pendingConfirmation({ confirmationWindowSeconds: 0 }),
		});

		expect(container.querySelector('.confirmation-prompt__countdown')).toBeNull();
		expect(container.textContent).toContain('nothing in your session changes');
	});
});

describe('the confirmation resolution notice', () => {
	it('shows nothing until a confirmation has resolved', () => {
		const { container } = renderPrompt({ confirmation: null, resolution: null });

		expect(container.textContent).toBe('');
	});

	it('says the action was approved, and who answered', () => {
		renderPrompt({ confirmation: null, resolution: resolutionView() });

		const notice = screen.getByRole('status');

		expect(notice.textContent).toContain('That action was approved');
		expect(notice.textContent).toContain('You answered it here');
	});

	it('says when another device answered first', () => {
		// Requirement 13.5.
		renderPrompt({
			confirmation: null,
			resolution: resolutionView({
				resolution: 'rejected',
				answeringClient: 'pwa',
				answeredByThisClient: false,
				nothingWasChanged: true,
			}),
		});

		const notice = screen.getByRole('status');

		expect(notice.textContent).toContain('That action was rejected');
		expect(notice.textContent).toContain('answered on your phone');
		expect(notice.textContent).toContain('Nothing in your session was changed');
	});

	it('says an expiry changed nothing', () => {
		// Requirement 23.7.
		renderPrompt({
			confirmation: null,
			resolution: resolutionView({
				resolution: 'expired',
				answeredByThisClient: false,
				nothingWasChanged: true,
			}),
		});

		const notice = screen.getByRole('status');

		expect(notice.textContent).toContain('The time to answer ran out');
		expect(notice.textContent).toContain('Nothing in your session was changed');
	});

	it('tells the producer their approval did not take effect', () => {
		// The window closed as they reached for Approve. Without this line they have every
		// reason to believe they authorised something that is now running.
		renderPrompt({
			confirmation: null,
			resolution: resolutionView({
				resolution: 'expired',
				answeredByThisClient: false,
				localVerdict: 'approved',
				contradictsLocalVerdict: true,
				nothingWasChanged: true,
			}),
		});

		expect(screen.getByRole('status').textContent).toContain('your approval did not take effect');
	});

	it('still says the question is settled when the resolution was a word it does not know', () => {
		// The server's resolution string was not one of the three this build knows, so the
		// field is absent rather than present and undefined — `exactOptionalPropertyTypes`
		// keeps the two apart, which is why this view is built without the key rather than
		// with it set to undefined.
		renderPrompt({
			confirmation: null,
			resolution: {
				requestId: 'request-1',
				answeredByThisClient: false,
				contradictsLocalVerdict: false,
				nothingWasChanged: false,
			},
		});

		expect(screen.getByRole('status').textContent)
			.toContain('no longer waiting for an answer');
	});

	it('can be dismissed, and a later resolution appears anyway', () => {
		const { rerender } = render(<ConfirmationPrompt
			confirmation={null}
			answerSubmitted={false}
			resolution={resolutionView()}
			approve={() => sent}
			reject={() => sent}
		/>);

		fireEvent.click(screen.getByRole('button', { name: 'Dismiss' }));

		expect(screen.queryByRole('status')).toBeNull();

		rerender(<ConfirmationPrompt
			confirmation={null}
			answerSubmitted={false}
			resolution={resolutionView({ requestId: 'request-2', resolution: 'rejected' })}
			approve={() => sent}
			reject={() => sent}
		/>);

		expect(screen.getByRole('status').textContent).toContain('That action was rejected');
	});

	it('shows the question in front of the producer rather than the outcome of an older one', () => {
		// A resolution can arrive for a confirmation that is not the one on screen — the
		// reducer keys the dismissal on `requestId` for exactly that reason.
		renderPrompt({
			confirmation: pendingConfirmation({ requestId: 'request-2' }),
			resolution: resolutionView(),
		});

		expect(screen.getByRole('alertdialog')).toBeTruthy();
		expect(screen.queryByRole('status')).toBeNull();
	});
});
