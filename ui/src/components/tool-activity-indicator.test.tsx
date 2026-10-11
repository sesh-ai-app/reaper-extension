/**
 * @vitest-environment jsdom
 *
 * Tests for the activity indicator and the phase it derives.
 *
 * **Validates: Requirements 15.2**
 */

import { cleanup, render, screen } from '@testing-library/react';
import {
	afterEach, describe, expect, it,
} from 'vitest';

import type { AgentResponseView, StreamErrorView } from '../bridge/payloads';
import '../i18n';
import {
	initialConversationState,
	type ConversationMessage,
} from '../state/conversation-reducer';
import {
	ToolActivityIndicator,
	toolActivityPhaseFor,
	type ToolActivityInput,
} from './tool-activity-indicator';

afterEach(cleanup);

const producerPrompt: ConversationMessage = {
	kind: 'producer',
	id: 'producer:1',
	promptText: 'Add a drum bus',
	sentAt: 1_700_000_000_000,
};

const finishedResponse: ConversationMessage = {
	kind: 'assistant',
	id: 'message-1',
	conversationId: '01JQCONVERSATION',
	turnId: 'turn-1',
	text: 'Added it.',
	textMayBeIncomplete: false,
	missingSequences: [],
	appliedDeltaCount: 2,
	reconciledWithAssembledText: true,
};

const streamingView: AgentResponseView = {
	active: true,
	conversationId: '01JQCONVERSATION',
	turnId: 'turn-1',
	messageId: 'message-1',
	state: 'streaming',
	text: 'Adding',
	openedImplicitly: false,
	appliedDeltaCount: 1,
	missingSequences: [],
	textMayBeIncomplete: false,
	reconciledWithAssembledText: false,
};

const streamError: StreamErrorView = {
	code: 'tool_execution_failed',
	message: 'The track could not be created.',
	envelopeType: 'request:tool',
	validationErrors: '',
};

function activityInput(overrides: Partial<ToolActivityInput> = {}): ToolActivityInput {
	return {
		messages: initialConversationState.messages,
		streamingResponse: initialConversationState.streamingResponse,
		streamError: initialConversationState.streamError,
		...overrides,
	};
}

describe('the activity phase', () => {
	it('is idle before anything has been asked', () => {
		expect(toolActivityPhaseFor(activityInput())).toBe('idle');
	});

	it('waits once a prompt is out and nothing has come back', () => {
		expect(toolActivityPhaseFor(activityInput({ messages: [producerPrompt] }))).toBe('awaitingResponse');
	});

	it('reports streaming once text is arriving', () => {
		expect(
			toolActivityPhaseFor(activityInput({
				messages: [producerPrompt],
				streamingResponse: streamingView,
			})),
		).toBe('streamingResponse');
	});

	it('returns to idle once the response has landed in the transcript', () => {
		expect(
			toolActivityPhaseFor(activityInput({ messages: [producerPrompt, finishedResponse] })),
		).toBe('idle');
	});

	it('stops waiting when the turn failed', () => {
		// The error is shown elsewhere. A spinner left under it would say the opposite of it.
		expect(
			toolActivityPhaseFor(activityInput({ messages: [producerPrompt], streamError })),
		).toBe('idle');
	});
});

describe('the activity indicator', () => {
	it('shows nothing while idle', () => {
		const { container } = render(<ToolActivityIndicator phase="idle" />);

		expect(container.textContent).toBe('');
	});

	it('announces that the request is being worked on', () => {
		render(<ToolActivityIndicator phase="awaitingResponse" />);

		expect(screen.getByRole('status').textContent).toContain('working on your request');
	});

	it('announces that a response is arriving', () => {
		render(<ToolActivityIndicator phase="streamingResponse" />);

		expect(screen.getByRole('status').textContent).toContain('responding');
	});
});
