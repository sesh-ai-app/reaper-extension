/**
 * @vitest-environment jsdom
 *
 * Tests for the conversation state hook — the wiring between the bridge and the reducer.
 *
 * jsdom is selected per file rather than in a config, matching the monorepo's other
 * suites, which have no vitest config at all. Only this file needs a DOM.
 *
 * **Validates: Requirements 13.2, 13.3, 15.2, 15.4**
 */

import { act, render } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import {
	agentResponseBridgeMessageName,
	confirmationApprovalBridgeMessageName,
	confirmationResolutionBridgeMessageName,
	connectionStateBridgeMessageName,
	pendingConfirmationBridgeMessageName,
	producerPromptBridgeMessageName,
	scriptDownloadBridgeMessageName,
	streamErrorBridgeMessageName,
	streamIngestBridgeMessageName,
} from '../bridge/contract';
import type {
	AgentResponseView,
	ConnectionStateView,
	PendingConfirmationView,
} from '../bridge/payloads';
import type { BridgeGlobal } from '../bridge/transport';
import { useConversationState, type ConversationStateApi } from './use-conversation-state';

interface Harness {
	api: () => ConversationStateApi;
	target: BridgeGlobal;
	sent: { messageName: string; serializedJson: string }[];
	publish: (messageName: string, payload: unknown) => void;
	unmount: () => void;
}

function renderHook(): Harness {
	const sent: { messageName: string; serializedJson: string }[] = [];
	const target: BridgeGlobal = {
		seshAiBridgeSend: (messageName: string, serializedJson: string) => {
			sent.push({ messageName, serializedJson });
		},
	};

	let latest: ConversationStateApi | undefined;

	function Probe() {
		latest = useConversationState({
			target,
			locale: 'de',
			logger: { warn: () => {}, error: () => {} },
			now: () => 1_700_000_000_000,
		});

		return null;
	}

	const { unmount } = render(<Probe />);

	return {
		sent,
		target,
		unmount,
		api: () => {
			if (latest === undefined) {
				throw new Error('the probe component did not render');
			}

			return latest;
		},
		publish: (messageName: string, payload: unknown) => {
			act(() => {
				// Exactly the call `CefPanelBrowserHost` evaluates in the frame.
				target.seshAiBridge?.receive(messageName, JSON.stringify(payload));
			});
		},
	};
}

const connectionState: ConnectionStateView = {
	state: 'reconnecting',
	consecutiveFailedAttempts: 1,
	reconnectionDelayMilliseconds: 500,
	requiredProducerAction: 'none',
	notice: 'Reconnecting.',
};

const streamingResponse: AgentResponseView = {
	active: true,
	conversationId: '01JQCONVERSATION',
	turnId: 'turn-1',
	messageId: 'message-1',
	state: 'streaming',
	text: 'Adding a drum bus',
	openedImplicitly: false,
	appliedDeltaCount: 1,
	missingSequences: [],
	textMayBeIncomplete: false,
	reconciledWithAssembledText: false,
};

const pendingConfirmation: PendingConfirmationView = {
	requestId: 'request-1',
	actionSummary: 'Delete the scratch vocal',
	details: 'Affects 1 track',
	riskLevel: 'high',
	riskLevelReported: true,
	confirmationWindowSeconds: 120,
	descriptionComplete: true,
};

describe('the conversation state hook', () => {
	it('installs the receiver on mount and removes it on unmount', () => {
		const harness = renderHook();

		expect(harness.target.seshAiBridge).toBeDefined();

		harness.unmount();

		expect(harness.target.seshAiBridge).toBeUndefined();
	});

	it('routes every inbound bridge message into the state', () => {
		const harness = renderHook();

		harness.publish(connectionStateBridgeMessageName, connectionState);
		harness.publish(agentResponseBridgeMessageName, streamingResponse);
		harness.publish(pendingConfirmationBridgeMessageName, pendingConfirmation);
		harness.publish(streamErrorBridgeMessageName, {
			code: 'stream_interrupted',
			message: 'The response stopped early.',
			envelopeType: 'stream:agent_response_delta',
			validationErrors: '',
		});
		harness.publish(streamIngestBridgeMessageName, {
			stageArn: 'arn:aws:ivs:us-east-1:1:stage/abc',
			rtmpsIngestUrl: 'rtmps://ingest.example/app',
			streamKeyPlaintext: 'sk-live',
			participantId: 'participant-1',
			expiresAt: '2026-01-01T00:00:00.000Z',
			reissued: false,
		});
		harness.publish(scriptDownloadBridgeMessageName, {
			downloadUrl: 'https://uploads.example/script',
			fileName: 'bus.lua',
			sizeBytes: 512,
			expiresAt: '2026-01-01T00:00:00.000Z',
		});

		const { state } = harness.api();

		expect(state.connection).toStrictEqual(connectionState);
		expect(state.streamingResponse?.text).toBe('Adding a drum bus');
		expect(state.pendingConfirmation?.requestId).toBe('request-1');
		expect(state.streamError?.code).toBe('stream_interrupted');
		expect(state.streamIngest?.participantId).toBe('participant-1');
		expect(state.scriptDownloads).toHaveLength(1);
	});

	it('sends a prompt with the displayed locale and adds it to the transcript', () => {
		const harness = renderHook();

		act(() => {
			harness.api().sendPrompt('add a drum bus');
		});

		expect(harness.sent).toHaveLength(1);
		expect(harness.sent[0]?.messageName).toBe(producerPromptBridgeMessageName);
		expect(JSON.parse(harness.sent[0]?.serializedJson ?? '')).toStrictEqual({
			promptText: 'add a drum bus',
			locale: 'de',
		});
		expect(harness.api().state.messages).toHaveLength(1);
		expect(harness.api().state.messages[0]).toMatchObject({
			kind: 'producer',
			promptText: 'add a drum bus',
			sentAt: 1_700_000_000_000,
		});
		// The identifier is a local React key and nothing correlates on it, so the assertion
		// is on its shape rather than on a counter shared across this file's renders.
		expect(harness.api().state.messages[0]?.id).toMatch(/^producer:1700000000000:\d+$/u);
	});

	it('carries the conversation identifier on the next prompt once the server names one', () => {
		const harness = renderHook();

		harness.publish(agentResponseBridgeMessageName, streamingResponse);

		act(() => {
			harness.api().sendPrompt('and route the overheads too');
		});

		expect(JSON.parse(harness.sent[0]?.serializedJson ?? '')).toMatchObject({
			conversationId: '01JQCONVERSATION',
		});
	});

	it('does not add a refused prompt to the transcript', () => {
		const harness = renderHook();

		act(() => {
			expect(harness.api().sendPrompt('   ').disposition).toBe('empty_prompt_text');
		});

		expect(harness.sent).toHaveLength(0);
		expect(harness.api().state.messages).toHaveLength(0);
	});

	it('sends an approval and waits for the resolution before taking the prompt down', () => {
		const harness = renderHook();

		harness.publish(pendingConfirmationBridgeMessageName, pendingConfirmation);

		act(() => {
			harness.api().approveConfirmation('request-1');
		});

		expect(harness.sent[0]?.messageName).toBe(confirmationApprovalBridgeMessageName);
		expect(JSON.parse(harness.sent[0]?.serializedJson ?? '')).toStrictEqual({
			requestId: 'request-1',
			decision: 'approved',
		});

		// Only the server's `confirm:resolved` establishes how it ended, so the prompt stays
		// up with its buttons quiet until then.
		expect(harness.api().state.pendingConfirmation).not.toBeNull();
		expect(harness.api().state.confirmationAnswerSubmitted).toBe(true);

		harness.publish(confirmationResolutionBridgeMessageName, {
			requestId: 'request-1',
			resolution: 'approved',
			answeringClient: 'extension',
			answeredByThisClient: true,
			localVerdict: 'approved',
			contradictsLocalVerdict: false,
			nothingWasChanged: false,
		});

		expect(harness.api().state.pendingConfirmation).toBeNull();
		expect(harness.api().state.latestConfirmationResolution?.answeredByThisClient).toBe(true);
	});

	it('reports a message it has no handler for without throwing', () => {
		const harness = renderHook();

		expect(() => {
			harness.publish('view:something_this_build_has_never_heard_of', {});
		}).not.toThrow();

		expect(harness.api().state.messages).toHaveLength(0);
	});
});
