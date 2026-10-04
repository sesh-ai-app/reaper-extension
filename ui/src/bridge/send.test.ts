/**
 * Unit tests for the send half: what reaches `seshAiBridgeSend`, and what is refused
 * before it gets there.
 *
 * **Validates: Requirements 13.2, 13.3, 15.2, 15.4**
 */

import { describe, expect, it } from 'vitest';

import {
	agentResponseBridgeMessageName,
	confirmationApprovalBridgeMessageName,
	confirmationRejectionBridgeMessageName,
	producerPromptBridgeMessageName,
} from './contract';
import {
	maximumPromptTextLength,
	sendBridgeMessage,
	sendConfirmationApproval,
	sendConfirmationRejection,
	sendProducerPrompt,
	type BridgeSendLogger,
	type BridgeSendOptions,
} from './send';
import type { BridgeGlobal } from './transport';

interface RecordingTarget {
	target: BridgeGlobal;
	sent: { messageName: string; serializedJson: string }[];
	options: BridgeSendOptions;
	warnings: string[];
}

function recordingTarget(): RecordingTarget {
	const sent: { messageName: string; serializedJson: string }[] = [];
	const warnings: string[] = [];
	const target: BridgeGlobal = {
		seshAiBridgeSend: (messageName: string, serializedJson: string) => {
			sent.push({ messageName, serializedJson });
		},
	};
	const logger: BridgeSendLogger = {
		warn: (message: string) => {
			warnings.push(message);
		},
	};

	return {
		target, sent, warnings, options: { target, logger },
	};
}

describe('sending the producer prompt', () => {
	it('hands the serialized payload to the renderer binding', () => {
		const recording = recordingTarget();

		const outcome = sendProducerPrompt(
			{ promptText: 'add a bus for the drums', locale: 'en' },
			recording.options,
		);

		expect(outcome.disposition).toBe('sent');
		expect(recording.sent).toHaveLength(1);
		expect(recording.sent[0]?.messageName).toBe(producerPromptBridgeMessageName);
		expect(JSON.parse(recording.sent[0]?.serializedJson ?? '')).toStrictEqual({
			promptText: 'add a bus for the drums',
			locale: 'en',
		});
	});

	it('sends the producer text verbatim, including its whitespace', () => {
		// `promptText` is what the standalone Bedrock Guardrail's `dataPath` resolves to.
		// Trimming it here would mean the guardrail scoring something other than what the
		// producer typed, so the emptiness check reads a trimmed copy and sends the original.
		const recording = recordingTarget();

		sendProducerPrompt({ promptText: '  mix it  ', locale: 'de' }, recording.options);

		expect(JSON.parse(recording.sent[0]?.serializedJson ?? '')).toStrictEqual({
			promptText: '  mix it  ',
			locale: 'de',
		});
	});

	it('refuses a prompt with nothing in it', () => {
		const recording = recordingTarget();

		expect(sendProducerPrompt({ promptText: '   ', locale: 'en' }, recording.options).disposition)
			.toBe('empty_prompt_text');
		expect(recording.sent).toHaveLength(0);
		expect(recording.warnings).toHaveLength(1);
	});

	it('refuses a prompt past the length the schema accepts', () => {
		const recording = recordingTarget();

		const outcome = sendProducerPrompt(
			{ promptText: 'a'.repeat(maximumPromptTextLength + 1), locale: 'en' },
			recording.options,
		);

		expect(outcome.disposition).toBe('prompt_text_too_long');
		expect(recording.sent).toHaveLength(0);
	});

	it('counts code points rather than UTF-16 units, as the schema does', () => {
		// Astral characters are two UTF-16 units each and one code point, and JSON Schema's
		// `maxLength` counts code points. Counting the wrong one refuses a producer whose
		// prompt is emoji-heavy thousands of characters early.
		const recording = recordingTarget();
		const atTheLimit = '🥁'.repeat(maximumPromptTextLength);

		expect(atTheLimit.length).toBe(maximumPromptTextLength * 2);
		expect(sendProducerPrompt({ promptText: atTheLimit, locale: 'en' }, recording.options)
			.disposition).toBe('sent');
	});

	it('refuses a prompt with no locale', () => {
		const recording = recordingTarget();

		expect(sendProducerPrompt({ promptText: 'mix it', locale: '' }, recording.options)
			.disposition).toBe('empty_locale');
		expect(recording.sent).toHaveLength(0);
	});

	it('carries the conversation identifier when there is one', () => {
		const recording = recordingTarget();

		sendProducerPrompt(
			{ promptText: 'and again', locale: 'en', conversationId: '01JQ' },
			recording.options,
		);

		expect(JSON.parse(recording.sent[0]?.serializedJson ?? '')).toStrictEqual({
			promptText: 'and again',
			locale: 'en',
			conversationId: '01JQ',
		});
	});
});

describe('sending a confirmation decision', () => {
	it('builds a payload that agrees with the name', () => {
		const recording = recordingTarget();

		sendConfirmationApproval('request-1', recording.options);
		sendConfirmationRejection('request-2', recording.options);

		expect(recording.sent.map((message) => message.messageName)).toStrictEqual([
			confirmationApprovalBridgeMessageName,
			confirmationRejectionBridgeMessageName,
		]);
		expect(JSON.parse(recording.sent[0]?.serializedJson ?? '')).toStrictEqual({
			requestId: 'request-1',
			decision: 'approved',
		});
		expect(JSON.parse(recording.sent[1]?.serializedJson ?? '')).toStrictEqual({
			requestId: 'request-2',
			decision: 'rejected',
		});
	});

	it('refuses a payload that contradicts its own name', () => {
		// The server refuses the disagreement rather than resolving it by precedence, since
		// neither reading is safe to guess at. Refusing it here saves an error round trip.
		const recording = recordingTarget();

		const outcome = sendBridgeMessage(
			confirmationApprovalBridgeMessageName,
			{ requestId: 'request-3', decision: 'rejected' },
			recording.options,
		);

		expect(outcome.disposition).toBe('decision_contradicts_message_name');
		expect(recording.sent).toHaveLength(0);
	});

	it('refuses a decision with no requestId', () => {
		const recording = recordingTarget();

		expect(sendConfirmationApproval('', recording.options).disposition)
			.toBe('empty_request_id');
		expect(recording.sent).toHaveLength(0);
	});
});

describe('the send path when something is wrong with the transport', () => {
	it('reports a missing binding rather than throwing', () => {
		// The page running outside CEF — `vite dev` in a browser — and the window between
		// the page loading and the renderer installing the global.
		const warnings: string[] = [];
		const outcome = sendProducerPrompt(
			{ promptText: 'mix it', locale: 'en' },
			{
				target: {},
				logger: {
					warn: (message: string) => {
						warnings.push(message);
					},
				},
			},
		);

		expect(outcome.disposition).toBe('transport_unavailable');
		expect(warnings).toHaveLength(1);
	});

	it('reports a binding that threw rather than propagating it', () => {
		const outcome = sendProducerPrompt(
			{ promptText: 'mix it', locale: 'en' },
			{
				target: {
					seshAiBridgeSend: () => {
						throw new Error('the renderer went away');
					},
				},
				logger: { warn: () => {} },
			},
		);

		expect(outcome.disposition).toBe('transport_threw');
		expect(outcome.reason).toBe('the renderer went away');
	});

	it('refuses a payload that cannot be serialized', () => {
		const recording = recordingTarget();
		const cyclic: { promptText: string; locale: string; self?: unknown } = {
			promptText: 'mix it',
			locale: 'en',
		};
		cyclic.self = cyclic;

		const outcome = sendProducerPrompt(
			cyclic,
			recording.options,
		);

		expect(outcome.disposition).toBe('not_serializable');
		expect(recording.sent).toHaveLength(0);
	});

	it('refuses an inbound message name offered to the send path', () => {
		const recording = recordingTarget();

		const outcome = sendBridgeMessage(
			agentResponseBridgeMessageName as never,
			{} as never,
			recording.options,
		);

		expect(outcome.disposition).toBe('wrong_direction');
		expect(recording.sent).toHaveLength(0);
	});

	it('refuses a name that is not in the contract at all', () => {
		const recording = recordingTarget();

		const outcome = sendBridgeMessage(
			'state:something_else' as never,
			{} as never,
			recording.options,
		);

		expect(outcome.disposition).toBe('unknown_message_name');
		expect(recording.sent).toHaveLength(0);
	});
});
