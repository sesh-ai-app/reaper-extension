/**
 * The decision a destructive operation waits on, and how it ended (requirements 13.1,
 * 13.2, 13.3, 13.4, 13.5, 23.7).
 *
 * ---------------------------------------------------------------------------
 * This component is not the authority on how the confirmation ended
 *
 * Only `confirm:resolved` establishes that, and it arrives as
 * `view:confirmation_resolved` after the server has settled the question — possibly
 * differently from what the producer tapped here, and possibly because another device
 * answered first. So an approval or a rejection sent from these buttons quiets them and
 * leaves the prompt up: `state.confirmationAnswerSubmitted` says an answer is in flight,
 * and the reducer takes the prompt down when the resolution for that `requestId` arrives.
 * Dismissing it locally would mean the panel showing a decision as made while the server
 * was still deciding, and the two disagreeing with no way for the producer to tell.
 *
 * The same reasoning is why **the countdown reaching zero does nothing**. The extension
 * does not enforce the confirmation window; the server does, and requirement 13.4 asks the
 * UI to *display* it so that a confirmation quietly expiring does not look like the
 * assistant ignoring the producer. So at zero the prompt stays up, the buttons stay live,
 * and the text says what is being waited for. A producer who taps Approve as the window
 * closes gets `contradictsLocalVerdict` back, and hears that theirs did not take effect —
 * which is only possible because the extension never decided it for them.
 *
 * `confirmationWindowSeconds` comes from the view rather than being the 120 that
 * requirement 13.4 names: the window is the server's, and a constant here would be a
 * second copy of it that goes stale silently the first time the server's changes.
 *
 * ---------------------------------------------------------------------------
 * Two fields that say the extension is not sure
 *
 * `riskLevelReported: false` means `riskLevel` is the coordinator's conservative default —
 * `high`, because the agent's own assessment was unreadable — and not an assessment of
 * this operation. It is shown as what it is. Presenting a guess as a fact, on the one
 * screen in the product whose whole job is informed consent, is the worst place in the
 * system to do it.
 *
 * `descriptionComplete: false` means a required field of the request was missing, so the
 * producer is being asked to authorise something the extension could not fully describe.
 * Also shown, for the same reason.
 */

import {
	useEffect, useId, useRef, useState,
} from 'react';
import { useTranslation } from 'react-i18next';

import type {
	ConfirmationResolutionView,
	PendingConfirmationView,
} from '../bridge/payloads';
import type { BridgeSendOutcome } from '../bridge/send';

/** How often the countdown is re-read. One second, because it is displayed in seconds. */
const countdownIntervalMilliseconds = 1_000;

/** `m:ss`, so no key needs a plural category for the number of seconds left. */
function formatRemainingTime(totalSeconds: number): string {
	const minutes = Math.floor(totalSeconds / 60);
	const seconds = totalSeconds % 60;

	return `${minutes}:${String(seconds).padStart(2, '0')}`;
}

/**
 * Seconds left in the server's window, counted down from a deadline rather than by
 * decrementing on each tick.
 *
 * A decrement loses time whenever the interval is late, and intervals in a backgrounded
 * renderer are throttled to once a second at best — so a countdown built that way drifts
 * behind the server it is describing, and the gap only ever grows. Reading the clock each
 * tick cannot drift: a late tick shows the right number late rather than the wrong number
 * on time.
 *
 * The deadline is taken from when this prompt appeared, which is a moment after the
 * server opened the window. The countdown is therefore slightly generous, and that is the
 * right direction for the error to run: it never tells the producer time remains after the
 * server has stopped waiting.
 *
 * There is no reset here for a superseding confirmation. `ConfirmationPrompt` keys the
 * prompt on `requestId`, so a new question mounts a new prompt and this starts from the
 * initialiser — which is React's own answer to resetting state when a prop changes, and it
 * keeps the deadline out of reach of a render that merely re-ran.
 */
function useRemainingWindowSeconds(windowSeconds: number): number {
	const [clock, setClock] = useState(() => {
		const startedAt = Date.now();

		return { deadlineAt: startedAt + windowSeconds * 1_000, now: startedAt };
	});

	useEffect(() => {
		const interval = setInterval(() => {
			setClock((previous) => ({ ...previous, now: Date.now() }));
		}, countdownIntervalMilliseconds);

		return () => {
			clearInterval(interval);
		};
	}, []);

	return Math.max(0, Math.ceil((clock.deadlineAt - clock.now) / 1_000));
}

interface PendingConfirmationPromptProps {
	confirmation: PendingConfirmationView;

	answerSubmitted: boolean;

	approve: (requestId: string) => BridgeSendOutcome;

	reject: (requestId: string) => BridgeSendOutcome;
}

function PendingConfirmationPrompt({
	confirmation,
	answerSubmitted,
	approve,
	reject,
}: PendingConfirmationPromptProps) {
	const { t } = useTranslation();

	const headingIdentifier = useId();
	const summaryIdentifier = useId();
	const promptRef = useRef<HTMLDivElement>(null);

	const [answerWasRefused, setAnswerWasRefused] = useState(false);

	const remainingSeconds = useRemainingWindowSeconds(confirmation.confirmationWindowSeconds);

	useEffect(() => {
		// Focus moves to the prompt rather than to a button: a producer mid-keystroke should
		// not be able to approve a destructive operation with a key they had already pressed
		// for something else. The container is focusable only programmatically, so it does not
		// add a stop to the tab order once it has been read.
		//
		// On mount, which is once per confirmation: the parent keys this component on
		// `requestId`, so a superseding question is a new prompt that takes focus of its own.
		promptRef.current?.focus();
	}, []);

	const sendAnswer = (send: (requestId: string) => BridgeSendOutcome) => {
		const outcome = send(confirmation.requestId);

		// A refused send left the server waiting, so the producer has to know their answer did
		// not leave the panel. `confirmationAnswerSubmitted` stays false, so the buttons stay
		// live and they can try again.
		setAnswerWasRefused(outcome.disposition !== 'sent');
	};

	const showsCountdown = confirmation.confirmationWindowSeconds > 0;

	return (
		<div
			className="confirmation-prompt"
			role="alertdialog"
			aria-labelledby={headingIdentifier}
			aria-describedby={summaryIdentifier}
			tabIndex={-1}
			ref={promptRef}
		>
			<h2 className="confirmation-prompt__heading" id={headingIdentifier}>
				{t('confirmation.heading')}
			</h2>

			<p className="confirmation-prompt__summary" id={summaryIdentifier}>
				{confirmation.actionSummary}
			</p>

			<p className={`confirmation-prompt__risk confirmation-prompt__risk--${confirmation.riskLevel}`}>
				{t('confirmation.risk', { level: t(`confirmation.riskLevel.${confirmation.riskLevel}`) })}
			</p>

			{!confirmation.riskLevelReported && (
				<p className="confirmation-prompt__risk-not-reported" role="note">
					{t('confirmation.riskNotReported')}
				</p>
			)}

			<h3 className="confirmation-prompt__details-label">{t('confirmation.detailsLabel')}</h3>

			<p className="confirmation-prompt__details">
				{confirmation.details.length > 0
					? confirmation.details
					: t('confirmation.detailsUnavailable')}
			</p>

			{!confirmation.descriptionComplete && (
				<p className="confirmation-prompt__description-incomplete" role="note">
					{t('confirmation.descriptionIncomplete')}
				</p>
			)}

			{/*
				Not a live region, deliberately: a value that changes every second in an
				`aria-live` region is a screen reader talking over everything else on the panel,
				including the summary of the operation being authorised.
			*/}
			{showsCountdown && (
				<p className="confirmation-prompt__countdown">
					{remainingSeconds > 0
						? t('confirmation.timeRemaining', { remaining: formatRemainingTime(remainingSeconds) })
						: t('confirmation.timeElapsed')}
				</p>
			)}

			<p className="confirmation-prompt__window-explanation">{t('confirmation.windowExplanation')}</p>

			<div className="confirmation-prompt__actions">
				<button
					type="button"
					className="confirmation-prompt__approve"
					disabled={answerSubmitted}
					onClick={() => {
						sendAnswer(approve);
					}}
				>
					{t('confirmation.approve')}
				</button>

				<button
					type="button"
					className="confirmation-prompt__reject"
					disabled={answerSubmitted}
					onClick={() => {
						sendAnswer(reject);
					}}
				>
					{t('confirmation.reject')}
				</button>
			</div>

			{answerSubmitted && (
				<p className="confirmation-prompt__answer-submitted">{t('confirmation.answerSubmitted')}</p>
			)}

			{answerWasRefused && (
				<p className="confirmation-prompt__answer-refused" role="alert">
					{t('confirmation.answerNotSent')}
				</p>
			)}
		</div>
	);
}

/** The line for each resolution, and for a resolution string this build does not know. */
function translationKeyForResolution(resolution: ConfirmationResolutionView): string {
	switch (resolution.resolution) {
		case 'approved':
			return 'confirmation.resolution.approved';

		case 'rejected':
			return 'confirmation.resolution.rejected';

		case 'expired':
			return 'confirmation.resolution.expired';

		default:
			// The server's resolution string was not one of the three. The prompt still came
			// down, so saying nothing would leave the producer with a question that silently
			// stopped being asked.
			return 'confirmation.resolution.unknown';
	}
}

/**
 * Which device answered, or `null` for an expiry — where nobody did, and the resolution
 * line already says so.
 */
function translationKeyForAnsweringClient(resolution: ConfirmationResolutionView): string | null {
	if (resolution.answeredByThisClient) {
		return 'confirmation.resolution.answeredHere';
	}

	if (resolution.answeringClient === 'pwa') {
		return 'confirmation.resolution.answeredOnThePhone';
	}

	if (resolution.answeringClient === 'extension') {
		return 'confirmation.resolution.answeredElsewhere';
	}

	return null;
}

/**
 * What the producer's own tap did, when the server concluded something else.
 *
 * This is the case requirement 13.5 is really about: the window closed as they reached for
 * Approve, the server expired the confirmation, and the resolution comes back saying the
 * operation did not happen. Without this line the producer has every reason to believe
 * they approved something that is now running.
 */
function translationKeyForContradiction(resolution: ConfirmationResolutionView): string | null {
	if (!resolution.contradictsLocalVerdict) {
		return null;
	}

	if (resolution.localVerdict === 'approved') {
		return 'confirmation.resolution.approvalDidNotTakeEffect';
	}

	if (resolution.localVerdict === 'rejected') {
		return 'confirmation.resolution.rejectionDidNotTakeEffect';
	}

	return 'confirmation.resolution.verdictDidNotTakeEffect';
}

export interface ConfirmationResolutionNoticeProps {
	/** `state.latestConfirmationResolution` — how the last confirmation ended. */
	resolution: ConfirmationResolutionView;
}

export function ConfirmationResolutionNotice({ resolution }: ConfirmationResolutionNoticeProps) {
	const { t } = useTranslation();

	// Dismissal is local and keyed by `requestId`, so acknowledging one resolution does not
	// hide the next. The reducer keeps the resolution — it is the record of how the last
	// confirmation ended, and nothing here should be able to erase that.
	const [dismissedRequestId, setDismissedRequestId] = useState<string | null>(null);

	if (dismissedRequestId === resolution.requestId) {
		return null;
	}

	const contradictionKey = translationKeyForContradiction(resolution);
	const answeringClientKey = translationKeyForAnsweringClient(resolution);

	return (
		<div className="confirmation-resolution-notice" role="status" aria-live="polite">
			<p className="confirmation-resolution-notice__outcome">
				{t(translationKeyForResolution(resolution))}
			</p>

			{contradictionKey !== null && (
				<p className="confirmation-resolution-notice__contradiction">{t(contradictionKey)}</p>
			)}

			{resolution.nothingWasChanged && (
				<p className="confirmation-resolution-notice__nothing-was-changed">
					{t('confirmation.resolution.nothingWasChanged')}
				</p>
			)}

			{answeringClientKey !== null && (
				<p className="confirmation-resolution-notice__answering-client">{t(answeringClientKey)}</p>
			)}

			<button
				type="button"
				className="confirmation-resolution-notice__dismiss"
				onClick={() => {
					setDismissedRequestId(resolution.requestId);
				}}
			>
				{t('confirmation.resolution.dismiss')}
			</button>
		</div>
	);
}

export interface ConfirmationPromptProps {
	/** `state.pendingConfirmation` — the confirmation awaiting an answer, or `null`. */
	confirmation: PendingConfirmationView | null;

	/** `state.confirmationAnswerSubmitted` — an answer is in flight, so the buttons go quiet. */
	answerSubmitted: boolean;

	/** `state.latestConfirmationResolution` — shown once no confirmation is pending. */
	resolution: ConfirmationResolutionView | null;

	/** The conversation hook's `approveConfirmation`. Answers the send outcome; never throws. */
	approve: (requestId: string) => BridgeSendOutcome;

	/** Its `rejectConfirmation`. */
	reject: (requestId: string) => BridgeSendOutcome;
}

export function ConfirmationPrompt({
	confirmation,
	answerSubmitted,
	resolution,
	approve,
	reject,
}: ConfirmationPromptProps) {
	// The pending prompt wins while it is up. A resolution for a *different* confirmation
	// can arrive while this one is still answerable — the reducer keys the dismissal on
	// `requestId` for exactly that reason — and the question in front of the producer
	// matters more than the outcome of one they have already answered.
	if (confirmation !== null) {
		return (
			<PendingConfirmationPrompt
				// Keyed so a superseding confirmation mounts a fresh prompt: the countdown
				// restarts from the new window, and an answer refused for the last question does
				// not sit over the new one.
				key={confirmation.requestId}
				confirmation={confirmation}
				answerSubmitted={answerSubmitted}
				approve={approve}
				reject={reject}
			/>
		);
	}

	if (resolution !== null) {
		// Keyed so a second resolution mounts a fresh notice rather than reusing the one the
		// producer may have just dismissed.
		return <ConfirmationResolutionNotice key={resolution.requestId} resolution={resolution} />;
	}

	return null;
}
