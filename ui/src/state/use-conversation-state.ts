/**
 * The hook that holds the conversation: the reducer, the installed bridge receiver, and
 * the three things the producer can send.
 *
 * One hook rather than several, because the state is one state — a prompt sent has to land
 * in the same transcript the streamed response finishes into, and the pending confirmation
 * has to clear on the resolution that names it. Splitting it into `useMessages`,
 * `useStreaming`, and `useConfirmation` would mean a shared store or three subscriptions to
 * the same bridge, and the bridge only has room for one `seshAiBridge.receive`.
 *
 * The receiver is installed in an effect and removed on cleanup, so a remount replaces the
 * dispatcher rather than leaving a previous one holding a stale `dispatch`. React 19's
 * StrictMode mounts twice in development, which this survives because the install is
 * idempotent and the cleanup restores what it found.
 */

import {
	useCallback, useEffect, useMemo, useReducer, useRef,
} from 'react';

import {
	agentResponseBridgeMessageName,
	confirmationResolutionBridgeMessageName,
	connectionStateBridgeMessageName,
	pendingConfirmationBridgeMessageName,
	scriptDownloadBridgeMessageName,
	streamErrorBridgeMessageName,
	streamIngestBridgeMessageName,
} from '../bridge/contract';
import type { ProducerPromptPayload } from '../bridge/payloads';
import {
	installBridgeMessageHandlers,
	type BridgeMessageHandlers,
	type BridgeReceiveLogger,
	type BridgeReceiveOutcome,
} from '../bridge/receive';
import {
	sendConfirmationApproval,
	sendConfirmationRejection,
	sendProducerPrompt,
	type BridgeSendOptions,
	type BridgeSendOutcome,
} from '../bridge/send';
import type { BridgeGlobal } from '../bridge/transport';
import {
	conversationReducer,
	initialConversationState,
	type ConversationState,
} from './conversation-reducer';

export interface UseConversationStateOptions {
	/**
	 * The producer's UI language as a BCP 47 tag, carried on every prompt. Pass the
	 * language actually on screen — `useTranslation().i18n.language` — rather than letting
	 * this default, or the agent answers in English to a producer reading German.
	 */
	locale?: string;

	/** The globals to bridge through. The page's own by default; a fake in the suite. */
	target?: BridgeGlobal;

	/** Where the bridge's diagnostics go. `console` by default. */
	logger?: BridgeReceiveLogger;

	/** Epoch milliseconds. Injectable so a test can assert on an ordering it chose. */
	now?: () => number;

	/** Called for every message the bridge handed over, landed or not. For a diagnostics view. */
	onBridgeReceiveOutcome?: (outcome: BridgeReceiveOutcome) => void;
}

export interface ConversationStateApi {
	state: ConversationState;

	/**
	 * Sends the prompt and, when the bridge accepted it, adds it to the transcript. The
	 * text crosses verbatim: no trimming, no framing. Answers the send outcome so the caller
	 * can tell a refused prompt from a sent one.
	 */
	sendPrompt: (promptText: string) => BridgeSendOutcome;

	/** Approves the confirmation with this `requestId`. */
	approveConfirmation: (requestId: string) => BridgeSendOutcome;

	/** Rejects it. */
	rejectConfirmation: (requestId: string) => BridgeSendOutcome;

	/** Clears the error banner. The presenter holds the error; this only stops showing it. */
	dismissStreamError: () => void;
}

/** The locale a prompt carries when the caller passed none. English, matching `fallbackLocale`. */
const defaultLocale = 'en';

let producerMessageCounter = 0;

/**
 * A local identifier for a producer message. Not a server identifier and nothing
 * correlates on it — the transcript needs a stable React key and nothing more, so this
 * does not reach for `crypto.randomUUID`, which is not available on every surface this
 * code is expected to run on.
 */
function nextProducerMessageIdentifier(sentAt: number): string {
	producerMessageCounter += 1;

	return `producer:${sentAt}:${producerMessageCounter}`;
}

export function useConversationState(
	options: UseConversationStateOptions = {},
): ConversationStateApi {
	const [state, dispatch] = useReducer(conversationReducer, initialConversationState);

	const {
		locale = defaultLocale,
		target,
		logger,
		now,
		onBridgeReceiveOutcome,
	} = options;

	// The outcome callback is the one option a component is likely to pass as an inline
	// arrow, so it goes through a ref: depending on it would reinstall the receiver on every
	// render, tearing the dispatcher down between publishes.
	const onOutcomeRef = useRef<((outcome: BridgeReceiveOutcome) => void) | undefined>(
		onBridgeReceiveOutcome,
	);

	useEffect(() => {
		onOutcomeRef.current = onBridgeReceiveOutcome;
	}, [onBridgeReceiveOutcome]);

	useEffect(() => {
		const handlers: BridgeMessageHandlers = {
			[agentResponseBridgeMessageName]: (view) => {
				dispatch({ kind: 'agentResponsePublished', view });
			},
			[pendingConfirmationBridgeMessageName]: (view) => {
				dispatch({ kind: 'confirmationPending', view });
			},
			[confirmationResolutionBridgeMessageName]: (view) => {
				dispatch({ kind: 'confirmationResolved', view });
			},
			[connectionStateBridgeMessageName]: (view) => {
				dispatch({ kind: 'connectionStatePublished', view });
			},
			[streamErrorBridgeMessageName]: (view) => {
				dispatch({ kind: 'streamErrorPublished', view });
			},
			[streamIngestBridgeMessageName]: (view) => {
				dispatch({ kind: 'streamIngestPublished', view });
			},
			[scriptDownloadBridgeMessageName]: (payload) => {
				dispatch({ kind: 'scriptDownloadOffered', payload });
			},
		};

		return installBridgeMessageHandlers(
			{
				handlers,
				onOutcome: (outcome) => {
					onOutcomeRef.current?.(outcome);
				},
				...(logger === undefined ? {} : { logger }),
			},
			target,
		);
	}, [target, logger]);

	const sendOptions = useMemo<BridgeSendOptions>(
		() => ({
			...(target === undefined ? {} : { target }),
			...(logger === undefined ? {} : { logger }),
		}),
		[target, logger],
	);

	const { conversationId } = state;

	const sendPrompt = useCallback(
		(promptText: string): BridgeSendOutcome => {
			// Omitted rather than sent empty on the first turn: its absence is how a client
			// asks the server for a new conversation.
			const payload: ProducerPromptPayload = conversationId === null
				? { promptText, locale }
				: { promptText, locale, conversationId };

			const outcome = sendProducerPrompt(payload, sendOptions);

			if (outcome.disposition === 'sent') {
				const sentAt = (now ?? Date.now)();

				dispatch({
					kind: 'producerPromptSent',
					id: nextProducerMessageIdentifier(sentAt),
					promptText,
					sentAt,
				});
			}

			return outcome;
		},
		[conversationId, locale, now, sendOptions],
	);

	const approveConfirmation = useCallback(
		(requestId: string): BridgeSendOutcome => {
			const outcome = sendConfirmationApproval(requestId, sendOptions);

			if (outcome.disposition === 'sent') {
				dispatch({ kind: 'confirmationAnswerSubmitted', requestId });
			}

			return outcome;
		},
		[sendOptions],
	);

	const rejectConfirmation = useCallback(
		(requestId: string): BridgeSendOutcome => {
			const outcome = sendConfirmationRejection(requestId, sendOptions);

			if (outcome.disposition === 'sent') {
				dispatch({ kind: 'confirmationAnswerSubmitted', requestId });
			}

			return outcome;
		},
		[sendOptions],
	);

	const dismissStreamError = useCallback(() => {
		dispatch({ kind: 'streamErrorDismissed' });
	}, []);

	return {
		state,
		sendPrompt,
		approveConfirmation,
		rejectConfirmation,
		dismissStreamError,
	};
}
