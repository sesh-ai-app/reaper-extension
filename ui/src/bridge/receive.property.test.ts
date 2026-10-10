/**
 * Property tests for the receive half.
 *
 * Two properties matter here. The first is that a valid payload survives the bridge: the
 * C++ serialises, the handler is given back what was serialised, and nothing in between
 * reshapes it. The second is coverage — every name in `all_bridge_message_names` is
 * handled, in the sense that the dispatcher reaches a defined disposition for it and never
 * throws, whichever direction the name travels.
 *
 * **Validates: Requirements 15.2, 15.4**
 */

import fc from 'fast-check';
import { describe, expect, it } from 'vitest';

import {
	allBridgeMessageNames,
	bridgeMessageDirectionFor,
	bridgeMessagesToJavaScript,
} from './contract';
import {
	bridgeMessageWithPayloadArbitrary,
	payloadArbitraryForBridgeMessage,
	unknownBridgeMessageNameArbitrary,
} from './payload-arbitraries.fixture';
import {
	createBridgeReceiveDispatcher,
	type BridgeMessageHandlers,
	type BridgeReceiveLogger,
} from './receive';

const silentLogger: BridgeReceiveLogger = {
	warn: () => {},
	error: () => {},
};

/** A handler for every inbound name, recording what it was given. */
function recordingHandlers(received: { messageName: string; payload: unknown }[]): {
	handlers: BridgeMessageHandlers;
} {
	const handlers: BridgeMessageHandlers = {};

	for (const messageName of bridgeMessagesToJavaScript) {
		// Each handler's parameter type differs, and the record is deliberately untyped —
		// the property is about the value arriving unchanged, not about its type.
		(handlers as Record<string, (payload: unknown) => void>)[messageName] = (payload) => {
			received.push({ messageName, payload });
		};
	}

	return { handlers };
}

describe('a valid payload round-trips across the bridge unchanged', () => {
	it('hands the handler exactly what was serialized, for every inbound name', () => {
		// One assertion per name rather than a name drawn alongside its payload: each name
		// gets its own full run of cases, so a shape that only breaks for one of the six view
		// models is not diluted by the other nine names' runs.
		for (const messageName of bridgeMessagesToJavaScript) {
			fc.assert(
				fc.property(payloadArbitraryForBridgeMessage[messageName], (payload) => {
					const received: { messageName: string; payload: unknown }[] = [];
					const receive = createBridgeReceiveDispatcher({
						...recordingHandlers(received),
						logger: silentLogger,
					});

					const outcome = receive(messageName, JSON.stringify(payload));

					expect(outcome.disposition).toBe('delivered');
					expect(received).toHaveLength(1);
					expect(received[0]?.messageName).toBe(messageName);
					expect(received[0]?.payload).toStrictEqual(payload);
				}),
			);
		}
	});
});

describe('every name in the contract is handled', () => {
	it('reaches a defined disposition and never throws, in either direction', () => {
		fc.assert(
			fc.property(bridgeMessageWithPayloadArbitrary, ({ messageName, payload }) => {
				const received: { messageName: string; payload: unknown }[] = [];
				const receive = createBridgeReceiveDispatcher({
					...recordingHandlers(received),
					logger: silentLogger,
				});

				const outcome = receive(messageName, JSON.stringify(payload));

				// The three outbound names are refused as `wrong_direction` rather than
				// delivered, which is the point of keeping the direction arrays: a prompt
				// arriving from C++ is not a message with a missing handler, it is a message
				// that should not exist.
				expect(outcome.disposition).toBe(
					bridgeMessageDirectionFor(messageName) === 'toJavaScript'
						? 'delivered'
						: 'wrong_direction',
				);
			}),
		);
	});

	it('covers the whole contract with no name left unaccounted for', () => {
		const accountedFor = new Set<string>();
		const received: { messageName: string; payload: unknown }[] = [];
		const receive = createBridgeReceiveDispatcher({
			...recordingHandlers(received),
			logger: silentLogger,
			onOutcome: (outcome) => {
				accountedFor.add(outcome.messageName);
			},
		});

		for (const messageName of allBridgeMessageNames) {
			receive(messageName, '{}');
		}

		expect([...accountedFor].sort()).toStrictEqual([...allBridgeMessageNames].sort());
	});
});

describe('an unrecognised message is logged and ignored, never thrown', () => {
	it('answers unknown_message_name for any string outside the contract', () => {
		fc.assert(
			fc.property(unknownBridgeMessageNameArbitrary, (candidate) => {
				const warnings: string[] = [];
				const received: { messageName: string; payload: unknown }[] = [];
				const receive = createBridgeReceiveDispatcher({
					...recordingHandlers(received),
					logger: {
						warn: (message: string) => {
							warnings.push(message);
						},
						error: () => {},
					},
				});

				const outcome = receive(candidate, '{"anything": true}');

				expect(outcome.disposition).toBe('unknown_message_name');
				expect(warnings).toHaveLength(1);
				expect(received).toHaveLength(0);
			}),
		);
	});

	it('answers malformed_payload for truncated JSON without logging what was in it', () => {
		// `view:stream_ingest` carries a stream key (requirement 26.4) and `script:download`
		// a signed URL (requirement 26.2). A truncated payload is the case where a naive
		// diagnostic leaks one: V8 quotes a fragment of the input in its own `SyntaxError`
		// message, so passing that message through would put the credential in the log of
		// whichever producer hit the serialiser bug. The property is that the secret does not
		// appear in any line, for a payload truncated at any point after it.
		fc.assert(
			fc.property(
				fc.constantFrom(...bridgeMessagesToJavaScript),
				// Hexadecimal, so the secret needs no JSON escaping and the truncation offset
				// below is an offset into the characters the payload actually carries.
				fc.array(fc.constantFrom(...'0123456789abcdef'), { minLength: 8, maxLength: 24 })
					.map((characters) => characters.join('')),
				// 0 to 20: the text after the secret is 21 characters, so every offset in
				// range leaves the payload genuinely truncated rather than complete.
				fc.nat({ max: 20 }),
				(messageName, secret, truncationOffset) => {
					const logged: string[] = [];
					const received: { messageName: string; payload: unknown }[] = [];
					const receive = createBridgeReceiveDispatcher({
						...recordingHandlers(received),
						logger: {
							warn: (message: string) => {
								logged.push(message);
							},
							error: (message: string) => {
								logged.push(message);
							},
						},
					});

					const wholePayload = `{"streamKeyPlaintext": "${secret}", "reissued": false}`;
					const truncated = wholePayload.slice(
						0,
						wholePayload.indexOf(secret) + secret.length + truncationOffset,
					);

					expect(receive(messageName, truncated).disposition).toBe('malformed_payload');
					expect(received).toHaveLength(0);

					for (const line of logged) {
						expect(line).not.toContain(secret);
					}
				},
			),
		);
	});
});
