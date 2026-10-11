/**
 * The receive half of the bridge: one dispatcher keyed by message name, installed as
 * `window.seshAiBridge.receive`.
 *
 * ---------------------------------------------------------------------------
 * An unrecognised name is logged and ignored, never thrown
 *
 * `message_dispatcher.h` applies that rule in the other direction for requirement 5.8,
 * and for the same reason it holds here: the C++ side of the bridge ships with the
 * extension and the UI ships inside the extension, but a producer can be running a build
 * whose C++ publishes something this bundle has no handler for — a partially applied
 * update, or a developer running `vite dev` against a newer binary. Throwing out of
 * `receive` puts the exception back inside C++'s `ExecuteJavaScript`, where it is a
 * browser-process log line at best, and takes the panel down at worst.
 *
 * So the dispatcher distinguishes the ways a message can fail to land, because they are
 * different problems and collapsing them would hide the one that matters:
 *
 *   | What is wrong                     | What it means                                   |
 *   |-----------------------------------|-------------------------------------------------|
 *   | the name is not in the contract   | the C++ is ahead of this bundle, or a typo      |
 *   | the name travels the other way    | something echoed an outbound message back       |
 *   | no handler registered for it      | this UI does not draw that yet — not an error   |
 *   | the payload is not JSON           | a serialiser bug on the C++ side                |
 *   | the handler threw                 | a bug in the UI, contained so the next one lands |
 *
 * Each is reported as an outcome and logged; none throws.
 */

import {
	bridgeMessageDirectionFor,
	isBridgeMessageToJavaScript,
	type BridgeMessageToJavaScriptName,
} from './contract';
import type { BridgeMessagePayloadToJavaScript } from './payloads';
import { installBridgeReceiver, type BridgeGlobal } from './transport';

/**
 * One handler per inbound message name, each typed to its own payload. All optional: a UI
 * that does not draw stream ingest details yet registers no handler for them, and that is
 * a disposition rather than a failure.
 */
export type BridgeMessageHandlers = {
	[Name in BridgeMessageToJavaScriptName]?: (
		payload: BridgeMessagePayloadToJavaScript[Name],
	) => void;
};

/** Parsed and handed to a handler, which returned, or one of the seven ways it did not. */
export type BridgeReceiveDisposition = 'delivered'

	/** Not one of the ten names. The C++ side is ahead of this bundle. */
	| 'unknown_message_name'

	/** A real name, travelling the wrong way — an outbound message echoed back. */
	| 'wrong_direction'

	/** A real inbound name with no handler registered. This UI does not draw it yet. */
	| 'no_handler_registered'

	/** `UiHost` refuses to publish either of these, so receiving one is itself the news. */
	| 'empty_message_name'
	| 'empty_payload'

	/** The payload did not parse as JSON, or did not parse as an object. */
	| 'malformed_payload'

	/** The handler threw. Contained so one broken view does not stop the next message. */
	| 'handler_threw';

export interface BridgeReceiveOutcome {
	/** The name as it arrived, whatever it was. */
	messageName: string;

	disposition: BridgeReceiveDisposition;

	/** Plain language for a log line, when the disposition is not `delivered`. */
	reason?: string;
}

/** Where the dispatcher's diagnostics go. `console` by default; injectable for the suite. */
export interface BridgeReceiveLogger {
	warn(message: string): void;
	error(message: string): void;
}

export interface BridgeReceiveDispatcherOptions {
	handlers: BridgeMessageHandlers;

	/**
	 * Called for every message, landed or not. The hook uses it for nothing; it exists so
	 * a diagnostics panel or a test can watch the bridge without wrapping every handler.
	 */
	onOutcome?: (outcome: BridgeReceiveOutcome) => void;

	logger?: BridgeReceiveLogger;
}

/** `console`, narrowed to the two methods used. */
const consoleLogger: BridgeReceiveLogger = {
	warn: (message: string) => {
		console.warn(message);
	},
	error: (message: string) => {
		console.error(message);
	},
};

function describe(outcome: BridgeReceiveOutcome): string {
	const suffix = outcome.reason === undefined ? '' : `: ${outcome.reason}`;

	return `[sesh-ai bridge] ${outcome.disposition} for "${outcome.messageName}"${suffix}`;
}

/**
 * Builds the function C++ calls.
 *
 * The returned function never throws and never returns a value C++ reads — the outcome is
 * for this side. Pass it to `installBridgeReceiver`, or call it directly in a test.
 */
export function createBridgeReceiveDispatcher(
	options: BridgeReceiveDispatcherOptions,
): (messageName: string, serializedJson: string) => BridgeReceiveOutcome {
	const { handlers, onOutcome } = options;
	const logger = options.logger ?? consoleLogger;

	return function receive(messageName: string, serializedJson: string): BridgeReceiveOutcome {
		const report = (
			disposition: BridgeReceiveDisposition,
			reason?: string,
		): BridgeReceiveOutcome => {
			const outcome: BridgeReceiveOutcome = reason === undefined
				? { messageName, disposition }
				: { messageName, disposition, reason };

			if (disposition === 'handler_threw' || disposition === 'malformed_payload') {
				logger.error(describe(outcome));
			} else if (disposition !== 'delivered') {
				logger.warn(describe(outcome));
			}

			onOutcome?.(outcome);

			return outcome;
		};

		if (typeof messageName !== 'string' || messageName.length === 0) {
			return report('empty_message_name', 'the UI Host does not publish an unnamed message');
		}

		if (typeof serializedJson !== 'string' || serializedJson.length === 0) {
			return report('empty_payload', 'the UI Host does not publish an empty payload');
		}

		if (!isBridgeMessageToJavaScript(messageName)) {
			// Two different problems, and the distinction is the whole point of keeping the
			// direction arrays rather than one flat set.
			if (bridgeMessageDirectionFor(messageName) === 'fromJavaScript') {
				return report(
					'wrong_direction',
					'this message only travels from JavaScript to C++ and was received instead',
				);
			}

			return report(
				'unknown_message_name',
				'not one of the ten names in the bridge contract — the extension may be ahead of this UI bundle',
			);
		}

		let payload: unknown;

		try {
			payload = JSON.parse(serializedJson);
		} catch (error) {
			// Neither the payload nor the parser's own message reaches the log. V8 quotes a
			// fragment of the input in its `SyntaxError` text, and the input here can be a
			// stream key (`view:stream_ingest`, requirement 26.4) or a signed download URL
			// (`script:download`, requirement 26.2). A serialiser bug on the C++ side is no
			// reason to put either in a log line, so the diagnostic names the failure and the
			// payload's length and stops there.
			const failure = error instanceof Error ? error.name : 'unknown error';

			return report(
				'malformed_payload',
				`the payload did not parse as JSON (${failure}, ${serializedJson.length} characters)`,
			);
		}

		if (payload === null || typeof payload !== 'object' || Array.isArray(payload)) {
			return report('malformed_payload', 'the payload parsed as JSON but not as an object');
		}

		const handler = handlers[messageName];

		if (handler === undefined) {
			return report(
				'no_handler_registered',
				'a name in the contract that this UI does not draw yet',
			);
		}

		try {
			// The cast is the seam. Nothing on this side validates the payload against its
			// schema: the C++ side serialised it from a type the Catch2 suite holds to the
			// schema, and shipping a JSON Schema validator into the renderer to re-check our
			// own serialiser would be a second implementation of a contract that is already
			// digest-verified. A payload that does not match its type here is a C++ bug, and
			// the handler sees exactly what arrived.
			(handler as (received: unknown) => void)(payload);
		} catch (error) {
			return report(
				'handler_threw',
				error instanceof Error ? error.message : String(error),
			);
		}

		return report('delivered');
	};
}

/**
 * The whole receive half in one call: build the dispatcher, install it as
 * `window.seshAiBridge.receive`, and answer the function that uninstalls it.
 */
export function installBridgeMessageHandlers(
	options: BridgeReceiveDispatcherOptions,
	target?: BridgeGlobal,
): () => void {
	const dispatch = createBridgeReceiveDispatcher(options);

	if (target === undefined) {
		return installBridgeReceiver(dispatch);
	}

	return installBridgeReceiver(dispatch, target);
}
