/**
 * Property tests for the bridge contract's classification functions — the TypeScript half
 * of the invariants `ui_host.h` holds with `static_assert`.
 *
 * **Validates: Requirements 15.2, 15.4**
 */

import fc from 'fast-check';
import { describe, expect, it } from 'vitest';

import {
	allBridgeMessageNames,
	bridgeMessageDirectionFor,
	bridgeMessageSchemaPathFor,
	bridgeMessagesFromJavaScript,
	bridgeMessagesToJavaScript,
	isBridgeMessageName,
	isViewModelBridgeMessage,
	namesAnEnvelopeNamespace,
	viewBridgeMessageNamespace,
} from './contract';
import { unknownBridgeMessageNameArbitrary } from './payload-arbitraries.fixture';

const bridgeMessageNameArbitrary = fc.constantFrom(...allBridgeMessageNames);

describe('every bridge message name belongs to exactly one direction', () => {
	it('places each of the ten in one of the two sets', () => {
		fc.assert(
			fc.property(bridgeMessageNameArbitrary, (messageName) => {
				const inToJavaScript = (bridgeMessagesToJavaScript as readonly string[])
					.includes(messageName);
				const inFromJavaScript = (bridgeMessagesFromJavaScript as readonly string[])
					.includes(messageName);

				expect(inToJavaScript).not.toBe(inFromJavaScript);
				expect(bridgeMessageDirectionFor(messageName)).toBe(
					inToJavaScript ? 'toJavaScript' : 'fromJavaScript',
				);
			}),
		);
	});

	it('partitions the whole set', () => {
		expect(bridgeMessagesToJavaScript.length + bridgeMessagesFromJavaScript.length)
			.toBe(allBridgeMessageNames.length);
		expect(new Set(allBridgeMessageNames).size).toBe(allBridgeMessageNames.length);
	});
});

describe('a name outside the contract gets the honest answer, not a default', () => {
	it('has no direction and no schema path', () => {
		fc.assert(
			fc.property(unknownBridgeMessageNameArbitrary, (candidate) => {
				expect(bridgeMessageDirectionFor(candidate)).toBeUndefined();
				expect(isBridgeMessageName(candidate)).toBe(false);
				expect(bridgeMessageSchemaPathFor(candidate)).toBe('');
			}),
		);
	});
});

describe('the view namespace splits the contract the way the schemas do', () => {
	it('is a view model exactly when the name carries the namespace', () => {
		fc.assert(
			fc.property(bridgeMessageNameArbitrary, (messageName) => {
				expect(isViewModelBridgeMessage(messageName))
					.toBe(messageName.startsWith(viewBridgeMessageNamespace));
			}),
		);
	});

	it('is a view model exactly when the payload is described under bridge-messages/', () => {
		fc.assert(
			fc.property(bridgeMessageNameArbitrary, (messageName) => {
				const schemaPath = bridgeMessageSchemaPathFor(messageName);

				expect(schemaPath.startsWith('bridge-messages/'))
					.toBe(isViewModelBridgeMessage(messageName));
				expect(schemaPath.startsWith('messages/'))
					.toBe(!isViewModelBridgeMessage(messageName));
			}),
		);
	});

	it('never lets a view model name read as an envelope type', () => {
		// The first of `ui_host.h`'s static assertions: `view:` is not one of the seven
		// namespaces `envelope.schema.json` enumerates, so a view model pushed onto the
		// outbound queue by accident is refused by the envelope schema rather than arriving
		// at a server with no handler for it.
		fc.assert(
			fc.property(bridgeMessageNameArbitrary, (messageName) => {
				if (isViewModelBridgeMessage(messageName)) {
					expect(namesAnEnvelopeNamespace(messageName)).toBe(false);
				}
			}),
		);
	});

	it('keeps every verbatim name a real envelope type', () => {
		// The second assertion, the other way round: forwarding a payload untouched is only
		// correct when the bridge message name *is* the envelope type.
		fc.assert(
			fc.property(bridgeMessageNameArbitrary, (messageName) => {
				if (!isViewModelBridgeMessage(messageName)) {
					expect(namesAnEnvelopeNamespace(messageName)).toBe(true);
				}
			}),
		);
	});

	it('refuses a namespace with nothing after the colon', () => {
		fc.assert(
			fc.property(
				fc.constantFrom('state', 'request', 'response', 'confirm', 'stream', 'script', 'error'),
				(messageNamespace) => {
					expect(namesAnEnvelopeNamespace(messageNamespace)).toBe(false);
					expect(namesAnEnvelopeNamespace(`${messageNamespace}:`)).toBe(false);
					expect(namesAnEnvelopeNamespace(`:${messageNamespace}`)).toBe(false);
				},
			),
		);
	});

	it('does not call the bare namespace a view model', () => {
		expect(isViewModelBridgeMessage(viewBridgeMessageNamespace)).toBe(false);
		expect(isViewModelBridgeMessage('')).toBe(false);
	});
});
