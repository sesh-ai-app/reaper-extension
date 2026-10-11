/**
 * Conversation state, as a reducer over the bridge messages that change it.
 *
 * Pure and React-free on purpose: every rule worth defending — what replaces what, which
 * resolution dismisses which prompt, when a streamed response becomes a message in the
 * history — is a function of the state and the message, and a function is testable without
 * a renderer. `use-conversation-state.ts` is the thin part that wires this to the bridge.
 *
 * ---------------------------------------------------------------------------
 * The streamed response is replaced, never appended to
 *
 * `view:agent_response` carries the Stream Presenter's whole current response, not a
 * delta: the presenter already holds the assembled text, placed sequence by sequence, and
 * publishes the view each time it changes. So the reducer stores the latest view and the
 * UI renders `text`. Appending here would duplicate every fragment, and the bug would look
 * like the agent stuttering.
 *
 * For the same reason `textMayBeIncomplete` and `missingSequences` are carried through
 * untouched. They are requirement 16.1's gap detection, computed by the presenter from the
 * deltas — which this side never sees. Recomputing them here is not possible, and
 * approximating them would mean the panel and the extension disagreeing about whether the
 * producer is looking at a complete answer.
 *
 * ---------------------------------------------------------------------------
 * A resolution dismisses the prompt holding its `requestId`, and no other
 *
 * `ConfirmationResolutionView` is keyed by `requestId` rather than meaning "the current
 * prompt", because another confirmation may be showing and still be answerable. A
 * resolution for a confirmation that is not the one on screen updates the notice and
 * leaves the prompt up.
 */

import type {
	AgentResponseView,
	ConfirmationResolutionView,
	ConnectionStateView,
	PendingConfirmationView,
	ScriptDownloadPayload,
	StreamErrorView,
	StreamIngestDisplay,
} from '../bridge/payloads';

/** A prompt the producer sent. Held locally — the server does not echo it back. */
export interface ProducerMessage {
	kind: 'producer';

	/** Local identifier. Not a server identifier; nothing correlates on it. */
	id: string;

	/** Exactly what was sent, which is exactly what the producer typed. */
	promptText: string;

	/** Epoch milliseconds, for ordering and for a timestamp in the transcript. */
	sentAt: number;
}

/** A finished assistant response, as it stood when the stop arrived. */
export interface AssistantMessage {
	kind: 'assistant';

	/** The server's `messageId`, or the `turnId` when the response carried no message identifier. */
	id: string;

	conversationId: string;

	turnId: string;

	/** The server's assembled text. Rendered as markdown. */
	text: string;

	/** Requirement 16.1's warning, as the presenter computed it. Shown, never recomputed. */
	textMayBeIncomplete: boolean;

	/** The sequence numbers that never arrived. Empty when nothing was outstanding. */
	missingSequences: readonly number[];

	/** How many deltas were placed into the text, including late ones that filled a gap. */
	appliedDeltaCount: number;

	/** True when the stop's assembled text replaced the locally accumulated fragments. */
	reconciledWithAssembledText: boolean;

	/** From the stop payload. Absent when the stop carried no reason. */
	stopReason?: string;
}

export type ConversationMessage = ProducerMessage | AssistantMessage;

export interface ConversationState {
	/** The transcript, in the order things happened. Producer prompts and finished responses. */
	messages: readonly ConversationMessage[];

	/**
	 * The response currently being drawn, or `null` when there is nothing to draw. Null
	 * rather than an inactive view, because `active: false` means "draw nothing" and a
	 * component should not have to know that.
	 */
	streamingResponse: AgentResponseView | null;

	/**
	 * The conversation the server is keeping, learned from the first response that named
	 * one. Null until then, which is how the next prompt asks for a new conversation.
	 */
	conversationId: string | null;

	/** The last connection status published, or `null` before the first one. Not a guess at "disconnected". */
	connection: ConnectionStateView | null;

	/** The confirmation awaiting the producer, or `null`. Superseded by a newer one. */
	pendingConfirmation: PendingConfirmationView | null;

	/**
	 * True once this UI has sent an answer for the pending confirmation and is waiting for
	 * the resolution to come back. The buttons go quiet; the prompt stays up, because only
	 * the server's `confirm:resolved` establishes how it ended.
	 */
	confirmationAnswerSubmitted: boolean;

	/** How the last confirmation ended, for the notice the UI shows afterwards. */
	latestConfirmationResolution: ConfirmationResolutionView | null;

	/** The last stream error, or `null`. One, not a list — the presenter holds one. */
	streamError: StreamErrorView | null;

	/** The current ReaCast ingest details, or `null`. A reissued token replaces the displayed one. */
	streamIngest: StreamIngestDisplay | null;

	/** Generated scripts offered this session, oldest first. A download, never an execution. */
	scriptDownloads: readonly ScriptDownloadPayload[];
}

export const initialConversationState: ConversationState = {
	messages: [],
	streamingResponse: null,
	conversationId: null,
	connection: null,
	pendingConfirmation: null,
	confirmationAnswerSubmitted: false,
	latestConfirmationResolution: null,
	streamError: null,
	streamIngest: null,
	scriptDownloads: [],
};

/** Everything that changes the conversation: the seven inbound messages, and what the UI did. */
export type ConversationAction = { kind: 'producerPromptSent'; id: string; promptText: string; sentAt: number }

	/** `view:agent_response`. */
	| { kind: 'agentResponsePublished'; view: AgentResponseView }

	/** `view:confirmation_pending`. */
	| { kind: 'confirmationPending'; view: PendingConfirmationView }

	/** `view:confirmation_resolved`. */
	| { kind: 'confirmationResolved'; view: ConfirmationResolutionView }

	/** The UI sent an approval or a rejection for the confirmation on screen. */
	| { kind: 'confirmationAnswerSubmitted'; requestId: string }

	/** `view:connection_state`. */
	| { kind: 'connectionStatePublished'; view: ConnectionStateView }

	/** `view:stream_error`. */
	| { kind: 'streamErrorPublished'; view: StreamErrorView }

	/** The producer dismissed the error banner. */
	| { kind: 'streamErrorDismissed' }

	/** `view:stream_ingest`. */
	| { kind: 'streamIngestPublished'; view: StreamIngestDisplay }

	/** `script:download`. */
	| { kind: 'scriptDownloadOffered'; payload: ScriptDownloadPayload };

/**
 * The identifier a finished response is filed under. The server's `messageId` when it sent
 * one, the `turnId` as a fallback, and empty when it sent neither — in which case the
 * response is appended rather than matched against an existing one, since there is nothing
 * to match on and silently merging two responses is worse than listing both.
 */
function assistantMessageIdentifierFor(view: AgentResponseView): string {
	if (view.messageId.length > 0) {
		return view.messageId;
	}

	if (view.turnId.length > 0) {
		return `turn:${view.turnId}`;
	}

	return '';
}

function assistantMessageFrom(view: AgentResponseView): AssistantMessage {
	const message: AssistantMessage = {
		kind: 'assistant',
		id: assistantMessageIdentifierFor(view),
		conversationId: view.conversationId,
		turnId: view.turnId,
		text: view.text,
		textMayBeIncomplete: view.textMayBeIncomplete,
		missingSequences: [...view.missingSequences],
		appliedDeltaCount: view.appliedDeltaCount,
		reconciledWithAssembledText: view.reconciledWithAssembledText,
	};

	// `exactOptionalPropertyTypes` is on, so an absent `stopReason` stays absent rather than
	// becoming a present `undefined`. The schema distinguishes the two: a stop whose own
	// reason was null is not the same as a response still in flight.
	if (view.stopReason === undefined) {
		return message;
	}

	return { ...message, stopReason: view.stopReason };
}

/** Appends the finished response, or replaces the one already filed under its identifier. */
function withFinishedResponse(
	messages: readonly ConversationMessage[],
	view: AgentResponseView,
): readonly ConversationMessage[] {
	const finished = assistantMessageFrom(view);

	if (finished.id.length === 0) {
		return [...messages, finished];
	}

	const existingIndex = messages.findIndex(
		(message) => message.kind === 'assistant' && message.id === finished.id,
	);

	if (existingIndex < 0) {
		return [...messages, finished];
	}

	const replaced = [...messages];
	replaced[existingIndex] = finished;

	return replaced;
}

export function conversationReducer(
	state: ConversationState,
	action: ConversationAction,
): ConversationState {
	switch (action.kind) {
		case 'producerPromptSent':
			return {
				...state,
				messages: [
					...state.messages,
					{
						kind: 'producer',
						id: action.id,
						promptText: action.promptText,
						sentAt: action.sentAt,
					},
				],
				// A banner from the previous turn sitting over a new one reads as the new turn
				// having failed. The error is the presenter's last one, and the producer has
				// moved on from it.
				streamError: null,
			};

		case 'agentResponsePublished': {
			const { view } = action;

			// Whatever else the view says, a conversation identifier is worth keeping: it is
			// how the next prompt continues this conversation instead of starting one.
			const conversationId = view.conversationId.length > 0
				? view.conversationId
				: state.conversationId;

			if (!view.active) {
				return { ...state, streamingResponse: null, conversationId };
			}

			if (view.state === 'ended') {
				return {
					...state,
					messages: withFinishedResponse(state.messages, view),
					streamingResponse: null,
					conversationId,
				};
			}

			return { ...state, streamingResponse: view, conversationId };
		}

		case 'confirmationPending':
			return {
				...state,
				pendingConfirmation: action.view,
				// A new prompt is a new question. Whatever was submitted was for the last one.
				confirmationAnswerSubmitted: false,
			};

		case 'confirmationAnswerSubmitted':
			if (state.pendingConfirmation?.requestId !== action.requestId) {
				// An answer for something that is no longer on screen changes nothing here. The
				// send already happened; the server will say what it did with it.
				return state;
			}

			return { ...state, confirmationAnswerSubmitted: true };

		case 'confirmationResolved': {
			const { view } = action;
			const resolvesTheVisiblePrompt = state.pendingConfirmation?.requestId === view.requestId;

			return {
				...state,
				latestConfirmationResolution: view,
				pendingConfirmation: resolvesTheVisiblePrompt ? null : state.pendingConfirmation,
				confirmationAnswerSubmitted: resolvesTheVisiblePrompt
					? false
					: state.confirmationAnswerSubmitted,
			};
		}

		case 'connectionStatePublished':
			return { ...state, connection: action.view };

		case 'streamErrorPublished':
			return { ...state, streamError: action.view };

		case 'streamErrorDismissed':
			return { ...state, streamError: null };

		case 'streamIngestPublished':
			// Replaced rather than appended, including when `reissued` is true: requirement
			// 16.3 is one set of details showing the current token, not a history of tokens.
			return { ...state, streamIngest: action.view };

		case 'scriptDownloadOffered':
			return { ...state, scriptDownloads: [...state.scriptDownloads, action.payload] };

		default:
			// Total over the action union. An action added without a case here is a typecheck
			// failure rather than a silently dropped update.
			return state;
	}
}
