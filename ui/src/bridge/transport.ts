/**
 * The transport under the bridge contract. Fixed by `CefPanelBrowserHost`, not ours to
 * choose, and deliberately the only module in `ui/` that touches either global.
 *
 * C++ to JavaScript: the adapter evaluates `window.seshAiBridge.receive(messageName,
 * serializedJson)` in the frame, so that object has to exist on the page before the first
 * publish or the call throws inside C++'s `ExecuteJavaScript` and the message is gone.
 *
 * JavaScript to C++: the global `seshAiBridgeSend(messageName, serializedJson)`, which the
 * CEF renderer process installs and whose two string arguments `OnProcessMessageReceived`
 * reads as the name and the JSON.
 *
 * `UiHost` refuses an empty name and an empty payload in both directions, so neither half
 * can send a message that means nothing. This module refuses the same two things on this
 * side, where the refusal can be reported to the caller rather than vanishing into a
 * browser-process log nobody reads.
 */

/** The object C++ calls into. Installed by this module; shaped by `cef_browser_host.h`. */
export interface SeshAiBridgeReceiver {
	receive(messageName: string, serializedJson: string): void;
}

/**
 * The two globals, as a type. Taken as a parameter rather than read from `window`
 * directly so the suite can drive both directions without a browser and without
 * reaching into a real page's globals.
 */
export interface BridgeGlobal {
	seshAiBridge?: SeshAiBridgeReceiver | undefined;
	seshAiBridgeSend?: ((messageName: string, serializedJson: string) => void) | undefined;
}

declare global {
	// eslint-disable-next-line vars-on-top
	var seshAiBridge: SeshAiBridgeReceiver | undefined;

	// eslint-disable-next-line vars-on-top
	var seshAiBridgeSend: ((messageName: string, serializedJson: string) => void) | undefined;
}

/** The page's own globals. The default for both halves outside a test. */
export function defaultBridgeGlobal(): BridgeGlobal {
	return globalThis;
}

/**
 * Installs `seshAiBridge.receive` and answers a function that removes it again.
 *
 * Installing replaces whatever was there: a second install from a remounted component
 * should win rather than leave the first dispatcher — now holding a stale `dispatch` —
 * receiving the publishes. Removal restores the previous value rather than deleting
 * unconditionally, so a nested install unwinds in order.
 */
export function installBridgeReceiver(
	receive: (messageName: string, serializedJson: string) => void,
	target: BridgeGlobal = defaultBridgeGlobal(),
): () => void {
	const previousReceiver = target.seshAiBridge;

	// Writing a property onto the target is the whole job rather than a side effect to be
	// avoided: `window.seshAiBridge` is where `CefPanelBrowserHost` looks, and there is no
	// other way to put it there. The parameter exists so the suite can pass something that
	// is not the page's globals.
	/* eslint-disable no-param-reassign */
	target.seshAiBridge = { receive };

	return () => {
		target.seshAiBridge = previousReceiver;
	};
	/* eslint-enable no-param-reassign */
}

/** Handed to `seshAiBridgeSend`, or one of the four ways it was not. */
export type RawBridgeSendDisposition = 'sent'

	/** `seshAiBridgeSend` is not installed — the page is running outside CEF, or the renderer binding is not up yet. */
	| 'transport_unavailable'

	/** `UiHost` refuses an empty name, so this side does not spend a message finding that out. */
	| 'empty_message_name'

	/** Likewise an empty payload. */
	| 'empty_payload'

	/** `seshAiBridgeSend` threw. Reported rather than propagated: a failed send is not the caller's bug to catch. */
	| 'transport_threw';

export interface RawBridgeSendOutcome {
	disposition: RawBridgeSendDisposition;

	/** Plain language for a log line, when the disposition is not `sent`. */
	reason?: string;
}

/**
 * Hands an already-serialized message to the renderer binding.
 *
 * Returns the outcome rather than throwing, following the extension's C++ convention that
 * logging is the returned outcome: this codebase's components answer what they did and the
 * caller, which knows how this build reports things, decides. The typed send path in
 * `send.ts` is what callers should use; this is the layer under it.
 */
export function sendRawBridgeMessage(
	messageName: string,
	serializedJson: string,
	target: BridgeGlobal = defaultBridgeGlobal(),
): RawBridgeSendOutcome {
	if (messageName.length === 0) {
		return {
			disposition: 'empty_message_name',
			reason: 'the UI Host refuses a bridge message with no name',
		};
	}

	if (serializedJson.length === 0) {
		return {
			disposition: 'empty_payload',
			reason: `the UI Host refuses an empty payload, and ${messageName} had one`,
		};
	}

	const send = target.seshAiBridgeSend;

	if (typeof send !== 'function') {
		return {
			disposition: 'transport_unavailable',
			reason: 'seshAiBridgeSend is not installed on this page',
		};
	}

	try {
		send(messageName, serializedJson);
	} catch (error) {
		return {
			disposition: 'transport_threw',
			reason: error instanceof Error ? error.message : String(error),
		};
	}

	return { disposition: 'sent' };
}
