/**
 * Unit tests for the receive half: the dispositions, and the install that puts the
 * dispatcher where C++ looks for it.
 *
 * **Validates: Requirements 15.2, 15.4**
 */

import {
	describe, expect, it, vi,
} from 'vitest';

import {
	agentResponseBridgeMessageName,
	connectionStateBridgeMessageName,
	producerPromptBridgeMessageName,
} from './contract';
import type { ConnectionStateView } from './payloads';
import {
	createBridgeReceiveDispatcher,
	installBridgeMessageHandlers,
	type BridgeMessageHandlers,
	type BridgeReceiveLogger,
} from './receive';
import type { BridgeGlobal } from './transport';

function recordingLogger(): BridgeReceiveLogger & { lines: string[] } {
	const lines: string[] = [];

	return {
		lines,
		warn: (message: string) => {
			lines.push(`warn ${message}`);
		},
		error: (message: string) => {
			lines.push(`error ${message}`);
		},
	};
}

const connectionState: ConnectionStateView = {
	state: 'reconnecting',
	consecutiveFailedAttempts: 2,
	reconnectionDelayMilliseconds: 2000,
	requiredProducerAction: 'none',
	notice: 'Reconnecting to Sesh.',
};

describe('the receive dispatcher', () => {
	it('delivers a parsed payload to the handler for its name', () => {
		const received: ConnectionStateView[] = [];
		const handlers: BridgeMessageHandlers = {
			[connectionStateBridgeMessageName]: (view) => {
				received.push(view);
			},
		};

		const receive = createBridgeReceiveDispatcher({ handlers, logger: recordingLogger() });
		const outcome = receive(
			connectionStateBridgeMessageName,
			JSON.stringify(connectionState),
		);

		expect(outcome.disposition).toBe('delivered');
		expect(received).toStrictEqual([connectionState]);
	});

	it('logs an unknown name and ignores it rather than throwing', () => {
		const logger = recordingLogger();
		const receive = createBridgeReceiveDispatcher({ handlers: {}, logger });

		const outcome = receive('view:something_this_build_has_never_heard_of', '{}');

		expect(outcome.disposition).toBe('unknown_message_name');
		expect(logger.lines).toHaveLength(1);
		expect(logger.lines[0]).toContain('unknown_message_name');
	});

	it('distinguishes an outbound name echoed back from an unknown one', () => {
		const logger = recordingLogger();
		const receive = createBridgeReceiveDispatcher({ handlers: {}, logger });

		expect(receive(producerPromptBridgeMessageName, '{}').disposition).toBe('wrong_direction');
	});

	it('reports a name in the contract that this UI does not draw yet', () => {
		const logger = recordingLogger();
		const receive = createBridgeReceiveDispatcher({ handlers: {}, logger });

		expect(receive(connectionStateBridgeMessageName, '{}').disposition)
			.toBe('no_handler_registered');
	});

	it('refuses the two things the UI Host itself refuses', () => {
		const logger = recordingLogger();
		const receive = createBridgeReceiveDispatcher({ handlers: {}, logger });

		expect(receive('', '{}').disposition).toBe('empty_message_name');
		expect(receive(connectionStateBridgeMessageName, '').disposition).toBe('empty_payload');
	});

	it('reports a payload that is not JSON, and does not log the payload itself', () => {
		const logger = recordingLogger();
		const receive = createBridgeReceiveDispatcher({ handlers: {}, logger });

		const outcome = receive(connectionStateBridgeMessageName, '{"state": ');

		expect(outcome.disposition).toBe('malformed_payload');
		expect(logger.lines[0]).not.toContain('"state"');
	});

	it('reports a payload that parses as something other than an object', () => {
		const receive = createBridgeReceiveDispatcher({ handlers: {}, logger: recordingLogger() });

		expect(receive(connectionStateBridgeMessageName, '[]').disposition).toBe('malformed_payload');
		expect(receive(connectionStateBridgeMessageName, 'null').disposition)
			.toBe('malformed_payload');
		expect(receive(connectionStateBridgeMessageName, '42').disposition)
			.toBe('malformed_payload');
	});

	it('contains a handler that threw so the next message still lands', () => {
		const logger = recordingLogger();
		const delivered: string[] = [];
		const handlers: BridgeMessageHandlers = {
			[connectionStateBridgeMessageName]: () => {
				throw new Error('a component bug');
			},
			[agentResponseBridgeMessageName]: () => {
				delivered.push('agent response');
			},
		};

		const receive = createBridgeReceiveDispatcher({ handlers, logger });

		const thrown = receive(connectionStateBridgeMessageName, '{}');

		expect(thrown.disposition).toBe('handler_threw');
		expect(thrown.reason).toBe('a component bug');
		expect(receive(agentResponseBridgeMessageName, '{}').disposition).toBe('delivered');
		expect(delivered).toStrictEqual(['agent response']);
	});

	it('reports every outcome to the observer, landed or not', () => {
		const observed: string[] = [];
		const receive = createBridgeReceiveDispatcher({
			handlers: {},
			logger: recordingLogger(),
			onOutcome: (outcome) => {
				observed.push(outcome.disposition);
			},
		});

		receive(connectionStateBridgeMessageName, '{}');
		receive('view:unheard_of', '{}');

		expect(observed).toStrictEqual(['no_handler_registered', 'unknown_message_name']);
	});
});

describe('installing the receiver where C++ looks for it', () => {
	it('exposes seshAiBridge.receive on the target', () => {
		const target: BridgeGlobal = {};
		const received: unknown[] = [];

		const uninstall = installBridgeMessageHandlers(
			{
				handlers: {
					[connectionStateBridgeMessageName]: (view) => {
						received.push(view);
					},
				},
				logger: recordingLogger(),
			},
			target,
		);

		// Exactly the call `CefPanelBrowserHost` evaluates in the frame.
		target.seshAiBridge?.receive(
			connectionStateBridgeMessageName,
			JSON.stringify(connectionState),
		);

		expect(received).toStrictEqual([connectionState]);

		uninstall();

		expect(target.seshAiBridge).toBeUndefined();
	});

	it('restores the previous receiver rather than deleting unconditionally', () => {
		const previous = { receive: vi.fn() };
		const target: BridgeGlobal = { seshAiBridge: previous };

		const uninstall = installBridgeMessageHandlers(
			{ handlers: {}, logger: recordingLogger() },
			target,
		);

		expect(target.seshAiBridge).not.toBe(previous);

		uninstall();

		expect(target.seshAiBridge).toBe(previous);
	});
});
