/**
 * Unit tests for the conversation reducer.
 *
 * **Validates: Requirements 13.1, 13.5, 15.2, 16.1, 16.2, 16.3, 23.2, 23.7**
 */

import { describe, expect, it } from 'vitest';

import type {
	AgentResponseView,
	ConfirmationResolutionView,
	ConnectionStateView,
	PendingConfirmationView,
	ScriptDownloadPayload,
	StreamErrorView,
	StreamIngestDisplay,
} from '../bridge/payloads';
import {
	conversationReducer,
	initialConversationState,
	type ConversationAction,
	type ConversationState,
} from './conversation-reducer';

function applyAll(
	actions: readonly ConversationAction[],
	from: ConversationState = initialConversationState,
): ConversationState {
	return actions.reduce(conversationReducer, from);
}

const streamingResponse: AgentResponseView = {
	active: true,
	conversationId: '01JQCONVERSATION',
	turnId: 'turn-1',
	messageId: 'message-1',
	state: 'streaming',
	text: 'Adding a drum bus',
	openedImplicitly: false,
	appliedDeltaCount: 3,
	highestSequenceSeen: 4,
	missingSequences: [2],
	textMayBeIncomplete: true,
	reconciledWithAssembledText: false,
};

const pendingConfirmation: PendingConfirmationView = {
	requestId: 'request-1',
	actionSummary: 'Delete the scratch vocal track',
	details: 'Affects 1 track: Scratch Vox',
	riskLevel: 'high',
	riskLevelReported: true,
	confirmationWindowSeconds: 120,
	descriptionComplete: true,
};

const connectionState: ConnectionStateView = {
	state: 'connected',
	consecutiveFailedAttempts: 0,
	reconnectionDelayMilliseconds: 0,
	requiredProducerAction: 'none',
	notice: 'Connected.',
};

describe('the streamed response', () => {
	it('is replaced on every publish rather than appended to', () => {
		// The presenter publishes the whole current response each time it changes. Appending
		// would duplicate every fragment, and the bug would look like the agent stuttering.
		const state = applyAll([
			{ kind: 'agentResponsePublished', view: { ...streamingResponse, text: 'Add' } },
			{ kind: 'agentResponsePublished', view: { ...streamingResponse, text: 'Adding a' } },
			{ kind: 'agentResponsePublished', view: { ...streamingResponse, text: 'Adding a bus' } },
		]);

		expect(state.streamingResponse?.text).toBe('Adding a bus');
		expect(state.messages).toHaveLength(0);
	});

	it('carries the presenter gap detection through untouched', () => {
		// `textMayBeIncomplete` and `missingSequences` are computed by the C++ Stream
		// Presenter from the deltas, which this side never sees. They are displayed, not
		// recomputed.
		const state = conversationReducer(initialConversationState, {
			kind: 'agentResponsePublished',
			view: { ...streamingResponse, missingSequences: [2, 5], textMayBeIncomplete: true },
		});

		expect(state.streamingResponse?.missingSequences).toStrictEqual([2, 5]);
		expect(state.streamingResponse?.textMayBeIncomplete).toBe(true);
	});

	it('draws nothing for an inactive response', () => {
		const state = applyAll([
			{ kind: 'agentResponsePublished', view: streamingResponse },
			{
				kind: 'agentResponsePublished',
				view: {
					...streamingResponse, active: false, state: 'idle', text: '',
				},
			},
		]);

		expect(state.streamingResponse).toBeNull();
		expect(state.messages).toHaveLength(0);
	});

	it('becomes a message in the transcript once the stop arrives', () => {
		const state = applyAll([
			{ kind: 'agentResponsePublished', view: streamingResponse },
			{
				kind: 'agentResponsePublished',
				view: {
					...streamingResponse,
					state: 'ended',
					text: 'Added a drum bus and routed four tracks into it.',
					missingSequences: [],
					textMayBeIncomplete: false,
					reconciledWithAssembledText: true,
					stopReason: 'end_turn',
				},
			},
		]);

		expect(state.streamingResponse).toBeNull();
		expect(state.messages).toHaveLength(1);
		expect(state.messages[0]).toStrictEqual({
			kind: 'assistant',
			id: 'message-1',
			conversationId: '01JQCONVERSATION',
			turnId: 'turn-1',
			text: 'Added a drum bus and routed four tracks into it.',
			textMayBeIncomplete: false,
			missingSequences: [],
			appliedDeltaCount: 3,
			reconciledWithAssembledText: true,
			stopReason: 'end_turn',
		});
	});

	it('leaves stopReason absent rather than present and undefined', () => {
		// The schema distinguishes a stop whose own reason was null from a response still in
		// flight. Collapsing them would have the UI report a reason for neither.
		const state = conversationReducer(initialConversationState, {
			kind: 'agentResponsePublished',
			view: { ...streamingResponse, state: 'ended' },
		});

		expect(state.messages[0]).not.toHaveProperty('stopReason');
	});

	it('replaces a finished response filed under the same message identifier', () => {
		const ended: AgentResponseView = { ...streamingResponse, state: 'ended', text: 'first' };

		const state = applyAll([
			{ kind: 'agentResponsePublished', view: ended },
			{ kind: 'agentResponsePublished', view: { ...ended, text: 'corrected' } },
		]);

		expect(state.messages).toHaveLength(1);
		expect(state.messages[0]).toMatchObject({ text: 'corrected' });
	});

	it('appends rather than merging when the response carried no identifier', () => {
		const anonymous: AgentResponseView = {
			...streamingResponse,
			state: 'ended',
			messageId: '',
			turnId: '',
		};

		const state = applyAll([
			{ kind: 'agentResponsePublished', view: anonymous },
			{ kind: 'agentResponsePublished', view: { ...anonymous, text: 'another' } },
		]);

		expect(state.messages).toHaveLength(2);
	});

	it('learns the conversation identifier and keeps it once the response goes idle', () => {
		const state = applyAll([
			{ kind: 'agentResponsePublished', view: streamingResponse },
			{
				kind: 'agentResponsePublished',
				view: {
					...streamingResponse, active: false, state: 'idle', conversationId: '',
				},
			},
		]);

		expect(state.conversationId).toBe('01JQCONVERSATION');
	});
});

describe('the transcript', () => {
	it('holds the prompt the producer sent, verbatim', () => {
		const state = conversationReducer(initialConversationState, {
			kind: 'producerPromptSent',
			id: 'producer:1',
			promptText: '  add a drum bus  ',
			sentAt: 1_700_000_000_000,
		});

		expect(state.messages).toStrictEqual([{
			kind: 'producer',
			id: 'producer:1',
			promptText: '  add a drum bus  ',
			sentAt: 1_700_000_000_000,
		}]);
	});

	it('clears a stale error banner when a new turn starts', () => {
		const streamError: StreamErrorView = {
			code: 'agent_unavailable',
			message: 'The assistant is not reachable.',
			envelopeType: 'state:prompt',
			validationErrors: '',
		};

		const state = applyAll([
			{ kind: 'streamErrorPublished', view: streamError },
			{
				kind: 'producerPromptSent',
				id: 'producer:1',
				promptText: 'try again',
				sentAt: 1,
			},
		]);

		expect(state.streamError).toBeNull();
	});
});

describe('the pending confirmation', () => {
	it('is dismissed by a resolution naming it', () => {
		const resolution: ConfirmationResolutionView = {
			requestId: 'request-1',
			resolution: 'approved',
			answeringClient: 'extension',
			answeredByThisClient: true,
			localVerdict: 'approved',
			contradictsLocalVerdict: false,
			nothingWasChanged: false,
		};

		const state = applyAll([
			{ kind: 'confirmationPending', view: pendingConfirmation },
			{ kind: 'confirmationResolved', view: resolution },
		]);

		expect(state.pendingConfirmation).toBeNull();
		expect(state.latestConfirmationResolution).toStrictEqual(resolution);
	});

	it('survives a resolution for a different requestId', () => {
		// The resolution view is keyed by `requestId` rather than meaning "the current
		// prompt": another confirmation may be showing and still be answerable.
		const state = applyAll([
			{ kind: 'confirmationPending', view: pendingConfirmation },
			{
				kind: 'confirmationResolved',
				view: {
					requestId: 'request-other',
					resolution: 'expired',
					answeredByThisClient: false,
					contradictsLocalVerdict: false,
					nothingWasChanged: true,
				},
			},
		]);

		expect(state.pendingConfirmation).toStrictEqual(pendingConfirmation);
		expect(state.latestConfirmationResolution?.requestId).toBe('request-other');
	});

	it('comes down even when the resolution could not be named', () => {
		// A resolution this build cannot read still establishes the confirmation is no longer
		// pending. Keeping the prompt up would let the producer answer something settled.
		const state = applyAll([
			{ kind: 'confirmationPending', view: pendingConfirmation },
			{
				kind: 'confirmationResolved',
				view: {
					requestId: 'request-1',
					answeredByThisClient: false,
					contradictsLocalVerdict: false,
					nothingWasChanged: false,
				},
			},
		]);

		expect(state.pendingConfirmation).toBeNull();
		expect(state.latestConfirmationResolution?.resolution).toBeUndefined();
	});

	it('is superseded by a newer confirmation, which resets the submitted flag', () => {
		const state = applyAll([
			{ kind: 'confirmationPending', view: pendingConfirmation },
			{ kind: 'confirmationAnswerSubmitted', requestId: 'request-1' },
			{
				kind: 'confirmationPending',
				view: { ...pendingConfirmation, requestId: 'request-2' },
			},
		]);

		expect(state.pendingConfirmation?.requestId).toBe('request-2');
		expect(state.confirmationAnswerSubmitted).toBe(false);
	});

	it('records an answer only for the prompt on screen', () => {
		const state = applyAll([
			{ kind: 'confirmationPending', view: pendingConfirmation },
			{ kind: 'confirmationAnswerSubmitted', requestId: 'request-elsewhere' },
		]);

		expect(state.confirmationAnswerSubmitted).toBe(false);
	});
});

describe('the rest of the panel state', () => {
	it('keeps the last connection status, and starts with none rather than a guess', () => {
		expect(initialConversationState.connection).toBeNull();

		const state = conversationReducer(initialConversationState, {
			kind: 'connectionStatePublished',
			view: connectionState,
		});

		expect(state.connection).toStrictEqual(connectionState);
	});

	it('replaces the stream error rather than keeping a history', () => {
		const first: StreamErrorView = {
			code: 'first',
			message: 'one',
			envelopeType: '',
			validationErrors: '',
		};
		const second: StreamErrorView = { ...first, code: 'second', message: 'two' };

		const state = applyAll([
			{ kind: 'streamErrorPublished', view: first },
			{ kind: 'streamErrorPublished', view: second },
		]);

		expect(state.streamError).toStrictEqual(second);
	});

	it('lets the producer dismiss the error banner', () => {
		const state = applyAll([
			{
				kind: 'streamErrorPublished',
				view: {
					code: 'c', message: 'm', envelopeType: '', validationErrors: '',
				},
			},
			{ kind: 'streamErrorDismissed' },
		]);

		expect(state.streamError).toBeNull();
	});

	it('replaces the ingest details when a token is reissued', () => {
		const ingest: StreamIngestDisplay = {
			stageArn: 'arn:aws:ivs:us-east-1:1:stage/abc',
			rtmpsIngestUrl: 'rtmps://ingest.example/app',
			streamKeyPlaintext: 'sk-first',
			participantId: 'participant-1',
			expiresAt: '2026-01-01T00:00:00.000Z',
			reissued: false,
		};

		const state = applyAll([
			{ kind: 'streamIngestPublished', view: ingest },
			{
				kind: 'streamIngestPublished',
				view: { ...ingest, streamKeyPlaintext: 'sk-second', reissued: true },
			},
		]);

		expect(state.streamIngest?.streamKeyPlaintext).toBe('sk-second');
		expect(state.streamIngest?.reissued).toBe(true);
	});

	it('collects the scripts offered this session, oldest first', () => {
		const download: ScriptDownloadPayload = {
			downloadUrl: 'https://uploads.example/one',
			fileName: 'one.lua',
			sizeBytes: 128,
			expiresAt: '2026-01-01T00:00:00.000Z',
		};

		const state = applyAll([
			{ kind: 'scriptDownloadOffered', payload: download },
			{
				kind: 'scriptDownloadOffered',
				payload: { ...download, fileName: 'two.lua' },
			},
		]);

		expect(state.scriptDownloads.map((script) => script.fileName))
			.toStrictEqual(['one.lua', 'two.lua']);
	});
});
