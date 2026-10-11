/**
 * @vitest-environment jsdom
 *
 * The chat surface end to end: a prompt out over the bridge, the response drawn as it
 * arrives, the finished response landing in the transcript, and the four surfaces the
 * extension pushes without being asked — the connection status, the error banner, the
 * confirmation decision, and what a turn produced.
 *
 * Driven through the two globals `CefPanelBrowserHost` provides rather than through the
 * hook, because the wiring between them is what this file is for — the pieces have their
 * own tests.
 *
 * **Validates: Requirements 13.1, 13.2, 13.5, 15.2, 15.6, 16.1, 16.2, 23.2, 26.2**
 */

import {
	act, cleanup, fireEvent, render, screen,
} from '@testing-library/react';
import {
	afterEach, beforeEach, describe, expect, it,
} from 'vitest';

import App from './App';
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
} from './bridge/contract';
import type {
	AgentResponseView,
	ConfirmationDecisionPayload,
	ProducerPromptPayload,
} from './bridge/payloads';
import './i18n';

interface SentBridgeMessage {
	messageName: string;

	serializedJson: string;
}

let sentMessages: SentBridgeMessage[] = [];

beforeEach(() => {
	sentMessages = [];

	globalThis.seshAiBridgeSend = (messageName: string, serializedJson: string) => {
		sentMessages.push({ messageName, serializedJson });
	};
});

afterEach(() => {
	cleanup();

	globalThis.seshAiBridgeSend = undefined;
});

/** Exactly the call the C++ adapter evaluates in the frame. */
function publish(messageName: string, payload: unknown): void {
	act(() => {
		globalThis.seshAiBridge?.receive(messageName, JSON.stringify(payload));
	});
}

function responseView(overrides: Partial<AgentResponseView> = {}): AgentResponseView {
	return {
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
		...overrides,
	};
}

function promptPayloadsSent(): ProducerPromptPayload[] {
	return sentMessages
		.filter((message) => message.messageName === producerPromptBridgeMessageName)
		.map((message) => JSON.parse(message.serializedJson) as ProducerPromptPayload);
}

function typeAndSend(promptText: string): void {
	const textInput = screen.getByLabelText(/Ask Sesh AI/);

	fireEvent.change(textInput, { target: { value: promptText } });
	fireEvent.keyDown(textInput, { key: 'Enter' });
}

describe('the chat surface', () => {
	it('sends the prompt, shows it, and says the request is being worked on', () => {
		render(<App />);

		typeAndSend('Add a drum bus');

		expect(promptPayloadsSent()).toEqual([
			{ promptText: 'Add a drum bus', locale: 'en' },
		]);

		expect(screen.getByText('Add a drum bus')).toBeTruthy();
		expect(screen.getByRole('status').textContent).toContain('working on your request');
	});

	it('draws the response as it arrives and files it once it stops', () => {
		const { container } = render(<App />);

		typeAndSend('Add a drum bus');

		publish(agentResponseBridgeMessageName, responseView({ text: 'Adding' }));
		publish(agentResponseBridgeMessageName, responseView({ text: 'Adding a **drum bus**' }));

		expect(screen.getByRole('region', { name: 'Response' })).toBeTruthy();
		expect(container.querySelector('strong')?.textContent).toBe('drum bus');
		expect(screen.getByRole('status').textContent).toContain('responding');

		publish(
			agentResponseBridgeMessageName,
			responseView({
				text: 'Adding a **drum bus**. Done.',
				state: 'ended',
				stopReason: 'end_turn',
				reconciledWithAssembledText: true,
			}),
		);

		// The streaming view is gone and the text is in the transcript, once.
		expect(screen.queryByRole('region', { name: 'Response' })).toBeNull();
		expect(screen.getAllByRole('listitem')).toHaveLength(2);
		expect(screen.queryByRole('status')).toBeNull();

		const rendered = container.textContent ?? '';

		expect(rendered).toContain('Adding a drum bus. Done.');
		expect(rendered.match(/Adding a drum bus/g)).toHaveLength(1);
	});

	it('continues the conversation the first response named', () => {
		render(<App />);

		typeAndSend('Add a drum bus');

		publish(agentResponseBridgeMessageName, responseView({ state: 'ended', text: 'Added it.' }));

		typeAndSend('Name it Drums');

		expect(promptPayloadsSent()[1]).toEqual({
			promptText: 'Name it Drums',
			locale: 'en',
			conversationId: '01JQCONVERSATION',
		});
	});

	it('shows the gap warning the presenter published, on the finished response', () => {
		render(<App />);

		typeAndSend('Add a drum bus');

		publish(
			agentResponseBridgeMessageName,
			responseView({
				state: 'ended',
				text: 'Added it.',
				textMayBeIncomplete: true,
				missingSequences: [3],
			}),
		);

		expect(screen.getByRole('note').textContent).toContain('may be incomplete');
		expect(screen.getByText(/Missing fragments: 3/)).toBeTruthy();
	});
});

describe('the connection status, in the panel', () => {
	it('says nothing until the Transport Client publishes a state', () => {
		const { container } = render(<App />);

		expect(container.querySelector('.connection-status')).toBeNull();
	});

	it('shows the reconnecting state the extension published', () => {
		// Requirement 23.1.
		const { container } = render(<App />);

		publish(connectionStateBridgeMessageName, {
			state: 'reconnecting',
			consecutiveFailedAttempts: 2,
			reconnectionDelayMilliseconds: 4_000,
			requiredProducerAction: 'none',
			notice: 'Reconnecting to the Sesh AI service.',
		});

		expect(container.querySelector('.connection-status')?.textContent)
			.toContain('The connection dropped');
	});

	it('explains a duplicate-client-type refusal in the producer language', () => {
		// Requirement 23.2.
		const { container } = render(<App />);

		publish(connectionStateBridgeMessageName, {
			state: 'disconnected',
			consecutiveFailedAttempts: 1,
			reconnectionDelayMilliseconds: 0,
			requiredProducerAction: 'close_the_other_reaper_instance',
			notice: 'Another REAPER instance holds this session.',
			lastRejection: 'duplicate_client_type',
		});

		expect(container.querySelector('.connection-status')?.textContent)
			.toContain('Another REAPER instance is holding this session');
	});
});

describe('the error banner, in the panel', () => {
	it('shows the error and clears it when dismissed', () => {
		// Requirement 15.6.
		render(<App />);

		publish(streamErrorBridgeMessageName, {
			code: 'stage_creation_failed',
			message: 'IVS stage creation failed.',
			envelopeType: 'request:start_stream',
			validationErrors: '',
		});

		expect(screen.getByRole('alert').textContent).toContain('Audio streaming could not be set up');

		fireEvent.click(screen.getByRole('button', { name: 'Dismiss' }));

		expect(screen.queryByRole('alert')).toBeNull();
	});
});

describe('the confirmation decision, in the panel', () => {
	function publishConfirmation(): void {
		publish(pendingConfirmationBridgeMessageName, {
			requestId: 'request-1',
			actionSummary: 'Delete the four takes under the vocal comp',
			details: 'Affects the track named Lead Vocal.',
			riskLevel: 'high',
			riskLevelReported: true,
			confirmationWindowSeconds: 120,
			descriptionComplete: true,
		});
	}

	function decisionPayloadsSent(): ConfirmationDecisionPayload[] {
		return sentMessages
			.filter((message) => message.messageName === confirmationApprovalBridgeMessageName)
			.map((message) => JSON.parse(message.serializedJson) as ConfirmationDecisionPayload);
	}

	it('asks, sends the approval, and keeps the prompt up until the server settles it', () => {
		// Requirements 13.1, 13.2. The prompt staying up is the point: only `confirm:resolved`
		// establishes how it ended.
		render(<App />);

		publishConfirmation();

		expect(screen.getByRole('alertdialog').textContent).toContain('Delete the four takes');

		fireEvent.click(screen.getByRole('button', { name: 'Approve' }));

		expect(decisionPayloadsSent()).toEqual([
			{ requestId: 'request-1', decision: 'approved' },
		]);

		expect(screen.getByRole('alertdialog')).toBeTruthy();
		expect(screen.getByRole('button', { name: 'Approve' }).hasAttribute('disabled')).toBe(true);
	});

	it('takes the prompt down on the resolution and says what happened', () => {
		// Requirement 13.5.
		render(<App />);

		publishConfirmation();

		publish(confirmationResolutionBridgeMessageName, {
			requestId: 'request-1',
			resolution: 'rejected',
			answeringClient: 'pwa',
			answeredByThisClient: false,
			contradictsLocalVerdict: false,
			nothingWasChanged: true,
		});

		expect(screen.queryByRole('alertdialog')).toBeNull();

		const notice = screen.getByRole('status');

		expect(notice.textContent).toContain('That action was rejected');
		expect(notice.textContent).toContain('answered on your phone');
		expect(notice.textContent).toContain('Nothing in your session was changed');
	});

	it('tells the producer their approval did not take effect when the window had closed', () => {
		// Requirement 23.7. They tapped Approve; the server had already expired it.
		render(<App />);

		publishConfirmation();

		fireEvent.click(screen.getByRole('button', { name: 'Approve' }));

		publish(confirmationResolutionBridgeMessageName, {
			requestId: 'request-1',
			resolution: 'expired',
			answeredByThisClient: false,
			localVerdict: 'approved',
			contradictsLocalVerdict: true,
			nothingWasChanged: true,
		});

		const notice = screen.getByRole('status');

		expect(notice.textContent).toContain('The time to answer ran out');
		expect(notice.textContent).toContain('your approval did not take effect');
	});
});

describe('what a turn produced, in the panel', () => {
	it('offers the generated ReaScript as a download at the end of the transcript', () => {
		// Requirement 15.6, ADR 0011, requirement 26.2.
		const { container } = render(<App />);

		publish(scriptDownloadBridgeMessageName, {
			downloadUrl: 'https://scripts.example.com/generated/take-cleanup.lua?X-Amz-Signature=abc',
			fileName: 'take-cleanup.lua',
			sizeBytes: 4_200,
			expiresAt: new Date(Date.now() + 60 * 60 * 1_000).toISOString(),
			description: 'Removes empty takes across the comp',
		});

		const transcript = container.querySelector('.application__transcript');
		const downloads = transcript?.querySelector('.script-download-list');

		expect(downloads?.textContent).toContain('take-cleanup.lua');
		expect(downloads?.textContent).toContain('The extension does not run it');
		expect(downloads?.querySelector('a')?.getAttribute('download')).toBe('take-cleanup.lua');
	});

	it('shows the ReaCast details outside the transcript, where they cannot scroll away', () => {
		// Requirement 16.2. The producer types the stream key into ReaCast by hand, so the
		// one scrolling region in the panel is the wrong home for it.
		const { container } = render(<App />);

		publish(streamIngestBridgeMessageName, {
			stageArn: 'arn:aws:ivs:us-east-1:111122223333:stage/AbCdEf',
			rtmpsIngestUrl: 'rtmps://a1b2c3.global-contribute.live-video.net:443/app/',
			streamKeyPlaintext: 'sk_us-east-1_AbCdEf_0123456789',
			participantId: 'participant-01JQ',
			expiresAt: new Date(Date.now() + 4 * 60 * 60 * 1_000).toISOString(),
			reissued: false,
		});

		const details = container.querySelector('.stream-ingest-details');

		expect(details?.textContent).toContain('rtmps://a1b2c3.global-contribute.live-video.net:443/app/');
		expect(details?.querySelector('.stream-ingest-details__stream-key')?.textContent)
			.toBe('sk_us-east-1_AbCdEf_0123456789');
		expect(container.querySelector('.application__transcript .stream-ingest-details')).toBeNull();
	});

	it('replaces the displayed token when a reissued one arrives', () => {
		// Requirement 16.3.
		const { container } = render(<App />);

		const baseIngest = {
			stageArn: 'arn:aws:ivs:us-east-1:111122223333:stage/AbCdEf',
			rtmpsIngestUrl: 'rtmps://a1b2c3.global-contribute.live-video.net:443/app/',
			participantId: 'participant-01JQ',
			expiresAt: new Date(Date.now() + 4 * 60 * 60 * 1_000).toISOString(),
		};

		publish(streamIngestBridgeMessageName, {
			...baseIngest,
			streamKeyPlaintext: 'sk_us-east-1_First_0001',
			reissued: false,
		});

		publish(streamIngestBridgeMessageName, {
			...baseIngest,
			streamKeyPlaintext: 'sk_us-east-1_Second_0002',
			reissued: true,
		});

		const streamKeys = [...container.querySelectorAll('.stream-ingest-details__stream-key')]
			.map((element) => element.textContent);

		expect(streamKeys).toEqual(['sk_us-east-1_Second_0002']);
		expect(container.textContent).toContain('These details were refreshed');
	});
});
