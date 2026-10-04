/**
 * Property tests for the send half, and for the round trip through both halves.
 *
 * The round trip is the one worth having: a payload the UI built, serialized by the send
 * path, is the payload a handler is given when the same text comes back through the
 * receive path. That is the shape of the real bridge — the C++ in between carries the
 * string unchanged — so a property holding over it holds over any payload the producer
 * can cause.
 *
 * **Validates: Requirements 13.2, 13.3, 15.2, 15.4**
 */

import fc from 'fast-check';
import { describe, expect, it } from 'vitest';

import {
	allBridgeMessageNames,
	bridgeMessagesFromJavaScript,
	bridgeMessagesToJavaScript,
	confirmationApprovalBridgeMessageName,
	confirmationRejectionBridgeMessageName,
} from './contract';
import {
	payloadArbitraryForBridgeMessage,
	unknownBridgeMessageNameArbitrary,
} from './payload-arbitraries.fixture';
import { createBridgeReceiveDispatcher, type BridgeReceiveLogger } from './receive';
import { sendBridgeMessage, type BridgeSendLogger, type BridgeSendOptions } from './send';
import { sendRawBridgeMessage, type BridgeGlobal } from './transport';

const silentSendLogger: BridgeSendLogger = { warn: () => {} };
const silentReceiveLogger: BridgeReceiveLogger = { warn: () => {}, error: () => {} };

function recordingOptions(sent: { messageName: string; serializedJson: string }[]): BridgeSendOptions {
	const target: BridgeGlobal = {
		seshAiBridgeSend: (messageName: string, serializedJson: string) => {
			sent.push({ messageName, serializedJson });
		},
	};

	return { target, logger: silentSendLogger };
}

describe('every outbound payload survives serialization', () => {
	it('arrives at the binding as JSON that parses back to what was sent', () => {
		for (const messageName of bridgeMessagesFromJavaScript) {
			fc.assert(
				fc.property(payloadArbitraryForBridgeMessage[messageName], (payload) => {
					const sent: { messageName: string; serializedJson: string }[] = [];

					const outcome = sendBridgeMessage(
						messageName,
						payload as never,
						recordingOptions(sent),
					);

					expect(outcome.disposition).toBe('sent');
					expect(sent).toHaveLength(1);
					expect(sent[0]?.messageName).toBe(messageName);

					// Never empty, in either field — the UI Host refuses both.
					expect(sent[0]?.messageName.length).toBeGreaterThan(0);
					expect(sent[0]?.serializedJson.length).toBeGreaterThan(0);

					expect(JSON.parse(sent[0]?.serializedJson ?? '')).toStrictEqual(payload);
				}),
			);
		}
	});

	it('round-trips every inbound payload through both halves without reshaping it', () => {
		// The real bridge carries the string unchanged: the C++ side between the two halves
		// is a `std::string` and a `JSON.parse`. Driving the raw transport in one direction
		// and the dispatcher in the other is the closest this suite can get to that, and it
		// is where a serialisation asymmetry would show up.
		for (const messageName of bridgeMessagesToJavaScript) {
			fc.assert(
				fc.property(payloadArbitraryForBridgeMessage[messageName], (payload) => {
					const sent: { messageName: string; serializedJson: string }[] = [];
					const received: unknown[] = [];
					const target: BridgeGlobal = {
						seshAiBridgeSend: (name: string, serializedJson: string) => {
							sent.push({ messageName: name, serializedJson });
						},
					};

					expect(
						sendRawBridgeMessage(messageName, JSON.stringify(payload), target).disposition,
					).toBe('sent');

					const handlers: Record<string, (received: unknown) => void> = {
						[messageName]: (view) => {
							received.push(view);
						},
					};

					const receive = createBridgeReceiveDispatcher({
						handlers,
						logger: silentReceiveLogger,
					});

					const outcome = receive(messageName, sent[0]?.serializedJson ?? '');

					expect(outcome.disposition).toBe('delivered');
					expect(received[0]).toStrictEqual(payload);
				}),
			);
		}
	});
});

describe('a decision can never disagree with the name carrying it', () => {
	it('refuses the contradicting pair in both directions', () => {
		fc.assert(
			fc.property(
				fc.string({ minLength: 1, maxLength: 64 }),
				fc.boolean(),
				(requestId, approvalName) => {
					const sent: { messageName: string; serializedJson: string }[] = [];
					const messageName = approvalName
						? confirmationApprovalBridgeMessageName
						: confirmationRejectionBridgeMessageName;
					const contradictingDecision = approvalName ? 'rejected' : 'approved';

					const outcome = sendBridgeMessage(
						messageName,
						{ requestId, decision: contradictingDecision },
						recordingOptions(sent),
					);

					expect(outcome.disposition).toBe('decision_contradicts_message_name');
					expect(sent).toHaveLength(0);
				},
			),
		);
	});

	it('accepts the agreeing pair', () => {
		fc.assert(
			fc.property(
				fc.string({ minLength: 1, maxLength: 64 }),
				fc.boolean(),
				(requestId, approvalName) => {
					const sent: { messageName: string; serializedJson: string }[] = [];
					const messageName = approvalName
						? confirmationApprovalBridgeMessageName
						: confirmationRejectionBridgeMessageName;

					const outcome = sendBridgeMessage(
						messageName,
						{ requestId, decision: approvalName ? 'approved' : 'rejected' },
						recordingOptions(sent),
					);

					expect(outcome.disposition).toBe('sent');
					expect(sent).toHaveLength(1);
				},
			),
		);
	});
});

describe('the send path refuses anything outside its half of the contract', () => {
	it('sends nothing for a name that is not outbound', () => {
		fc.assert(
			fc.property(
				fc.constantFrom(...allBridgeMessageNames),
				(messageName) => {
					const sent: { messageName: string; serializedJson: string }[] = [];

					const outcome = sendBridgeMessage(
						messageName as never,
						{ requestId: 'r', decision: 'approved' } as never,
						recordingOptions(sent),
					);

					if ((bridgeMessagesFromJavaScript as readonly string[]).includes(messageName)) {
						// A prompt payload is not a decision payload, so the only outbound name
						// this generated payload is valid for is one of the two confirmations.
						expect(['sent', 'empty_prompt_text', 'decision_contradicts_message_name'])
							.toContain(outcome.disposition);

						return;
					}

					expect(outcome.disposition).toBe('wrong_direction');
					expect(sent).toHaveLength(0);
				},
			),
		);
	});

	it('sends nothing for a name outside the contract entirely', () => {
		fc.assert(
			fc.property(unknownBridgeMessageNameArbitrary, (candidate) => {
				const sent: { messageName: string; serializedJson: string }[] = [];

				const outcome = sendBridgeMessage(
					candidate as never,
					{} as never,
					recordingOptions(sent),
				);

				expect(outcome.disposition).toBe('unknown_message_name');
				expect(sent).toHaveLength(0);
			}),
		);
	});
});
