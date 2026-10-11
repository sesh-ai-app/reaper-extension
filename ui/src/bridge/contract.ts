/**
 * The CEF bridge contract — the TypeScript half.
 *
 * The ten bridge message names, their direction, and the schema each payload answers to
 * are declared in `src/ui/ui_host.h` and documented in design.md under "The CEF bridge
 * contract". This module restates them for the toolchain that cannot read a C++ header,
 * and `contract.test.ts` parses that header and fails when the two disagree — which is
 * the only thing standing between a renamed message and a panel that quietly stops
 * showing one thing. Nothing links the two halves at build time: a C++ side publishing
 * `view:connection_status` to a UI listening for `view:connection_state` produces no
 * error anywhere.
 *
 * So: do not edit a name here without editing it there. The test will tell you.
 */

/**
 * Every field crossing the bridge is camelCase. C++ members stay snake_case and the
 * serialiser translates, so `PendingConfirmationView::action_summary` arrives as
 * `actionSummary`. Stated as a value so the suite can name the rule.
 *
 * The one protocol payload that is not already camelCase —
 * `messages/confirmation-request.schema.json`, with its `action_summary`, `risk_level`,
 * and `details` — never reaches TypeScript in that spelling. The Confirmation
 * Coordinator is decoding it anyway and publishes `view:confirmation_pending` in
 * camelCase. A snake_case field name anywhere under `ui/` is a bug, not a convention.
 */
export const bridgeFieldNamingConvention = 'camelCase';

/**
 * The namespace every view model name carries. Load-bearing rather than decorative:
 * `view:` is not one of the seven namespaces `envelope.schema.json` enumerates, so a
 * view model name can never be mistaken for an envelope type, and a view model pushed
 * onto the outbound queue by accident is refused by the envelope schema rather than
 * arriving at a server with no handler for it.
 */
export const viewBridgeMessageNamespace = 'view:';

/**
 * The protocol version of the vendored schema bundle these types were derived from, as
 * carried by `schemas/MANIFEST.json`. Asserted against the manifest by the suite, so a
 * regenerated bundle that moved a field is visible here rather than at runtime.
 */
export const vendoredSchemaProtocolVersion = '7793985456d6';

// ---------------------------------------------------------------------------
// Protocol payloads, carried verbatim
// ---------------------------------------------------------------------------
//
// The bridge message name *is* the envelope type and the payload is the one
// `messages/*.schema.json` already describes.

/**
 * The generated ReaScript download (requirement 5.7, ADR 0011). The one inbound envelope
 * with no view model: the Message Dispatcher routes it here and the payload crosses
 * untouched. A download, never an execution.
 */
export const scriptDownloadBridgeMessageName = 'script:download';

/**
 * The producer's prompt (requirement 15.2). The UI builds the payload rather than handing
 * bare text to C++ for framing, because the standalone Bedrock Guardrail's `dataPath`
 * resolves to `promptText`.
 */
export const producerPromptBridgeMessageName = 'state:prompt';

/** The producer's approval (requirements 13.2, 13.3). */
export const confirmationApprovalBridgeMessageName = 'confirm:approve';

/** The producer's rejection. One schema serves both, and `decision` must agree with the name. */
export const confirmationRejectionBridgeMessageName = 'confirm:reject';

// ---------------------------------------------------------------------------
// View models
// ---------------------------------------------------------------------------
//
// Already decoded by the C++ component that owns the decision, and carrying fields no
// protocol schema has because they are the extension's conclusions rather than the
// server's words.

/** `PendingConfirmationView` (requirement 13.1). */
export const pendingConfirmationBridgeMessageName = 'view:confirmation_pending';

/** `ConfirmationResolutionView` (requirements 13.5, 23.7). */
export const confirmationResolutionBridgeMessageName = 'view:confirmation_resolved';

/** The Stream Presenter's `ResponseView` (requirement 16.1). */
export const agentResponseBridgeMessageName = 'view:agent_response';

/** `StreamIngestDisplay` (requirements 16.2, 16.3). Carries a credential. */
export const streamIngestBridgeMessageName = 'view:stream_ingest';

/** `StreamErrorView` (requirements 5.5, 23.2). Also where the seven `error:*` types land. */
export const streamErrorBridgeMessageName = 'view:stream_error';

/** The Transport Client's `connection_status` (requirement 3.4). */
export const connectionStateBridgeMessageName = 'view:connection_state';

// ---------------------------------------------------------------------------
// The sets, by direction
// ---------------------------------------------------------------------------

/** C++ to JavaScript. Published from the timer tick and nowhere else (requirement 2.3). */
export const bridgeMessagesToJavaScript = [
	scriptDownloadBridgeMessageName,
	pendingConfirmationBridgeMessageName,
	confirmationResolutionBridgeMessageName,
	agentResponseBridgeMessageName,
	streamIngestBridgeMessageName,
	streamErrorBridgeMessageName,
	connectionStateBridgeMessageName,
] as const;

/** JavaScript to C++. Arrives at `UiHost::receive_from_javascript` and goes on to the dispatcher. */
export const bridgeMessagesFromJavaScript = [
	producerPromptBridgeMessageName,
	confirmationApprovalBridgeMessageName,
	confirmationRejectionBridgeMessageName,
] as const;

/** Both halves, in the order `all_bridge_message_names` declares them. */
export const allBridgeMessageNames = [
	scriptDownloadBridgeMessageName,
	producerPromptBridgeMessageName,
	confirmationApprovalBridgeMessageName,
	confirmationRejectionBridgeMessageName,
	pendingConfirmationBridgeMessageName,
	confirmationResolutionBridgeMessageName,
	agentResponseBridgeMessageName,
	streamIngestBridgeMessageName,
	streamErrorBridgeMessageName,
	connectionStateBridgeMessageName,
] as const;

export type BridgeMessageToJavaScriptName = (typeof bridgeMessagesToJavaScript)[number];
export type BridgeMessageFromJavaScriptName = (typeof bridgeMessagesFromJavaScript)[number];
export type BridgeMessageName = (typeof allBridgeMessageNames)[number];

/** `BridgeMessageDirection` in `ui_host.h`, spelled the way TypeScript spells things. */
export type BridgeMessageDirection = 'toJavaScript' | 'fromJavaScript';

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

/** True when the name carries the `view:` namespace, which is exactly the view model half. */
export function isViewModelBridgeMessage(messageName: string): boolean {
	return messageName.length > viewBridgeMessageNamespace.length
		&& messageName.startsWith(viewBridgeMessageNamespace);
}

/**
 * Which direction a name travels, or `undefined` for a name outside the contract — which
 * is a message neither side should be sending, and the honest answer rather than a default
 * direction that would let one through.
 */
export function bridgeMessageDirectionFor(messageName: string): BridgeMessageDirection | undefined {
	if ((bridgeMessagesToJavaScript as readonly string[]).includes(messageName)) {
		return 'toJavaScript';
	}

	if ((bridgeMessagesFromJavaScript as readonly string[]).includes(messageName)) {
		return 'fromJavaScript';
	}

	return undefined;
}

export function isBridgeMessageName(messageName: string): messageName is BridgeMessageName {
	return bridgeMessageDirectionFor(messageName) !== undefined;
}

export function isBridgeMessageToJavaScript(
	messageName: string,
): messageName is BridgeMessageToJavaScriptName {
	return bridgeMessageDirectionFor(messageName) === 'toJavaScript';
}

export function isBridgeMessageFromJavaScript(
	messageName: string,
): messageName is BridgeMessageFromJavaScriptName {
	return bridgeMessageDirectionFor(messageName) === 'fromJavaScript';
}

/**
 * The schema a bridge message's payload is described by, relative to the vendored bundle,
 * and empty for a name outside the contract.
 *
 * Carried as data for the same reason `bridge_message_schema_path_for` is: a payload plus
 * a guess at its schema is how one document ends up validated against another's rules.
 */
const bridgeMessageSchemaPaths: Readonly<Record<BridgeMessageName, string>> = {
	[scriptDownloadBridgeMessageName]: 'messages/script-download.schema.json',
	[producerPromptBridgeMessageName]: 'messages/state-prompt.schema.json',
	// One schema, both decisions.
	[confirmationApprovalBridgeMessageName]: 'messages/confirm-decision.schema.json',
	[confirmationRejectionBridgeMessageName]: 'messages/confirm-decision.schema.json',
	[pendingConfirmationBridgeMessageName]: 'bridge-messages/confirmation-pending.schema.json',
	[confirmationResolutionBridgeMessageName]: 'bridge-messages/confirmation-resolved.schema.json',
	[agentResponseBridgeMessageName]: 'bridge-messages/agent-response.schema.json',
	[streamIngestBridgeMessageName]: 'bridge-messages/stream-ingest.schema.json',
	[streamErrorBridgeMessageName]: 'bridge-messages/stream-error.schema.json',
	[connectionStateBridgeMessageName]: 'bridge-messages/connection-state.schema.json',
};

export function bridgeMessageSchemaPathFor(messageName: string): string {
	if (!isBridgeMessageName(messageName)) {
		return '';
	}

	return bridgeMessageSchemaPaths[messageName];
}

// ---------------------------------------------------------------------------
// The envelope namespaces, for the invariant the C++ holds with static assertions
// ---------------------------------------------------------------------------

/**
 * The seven namespaces `envelope.schema.json` enumerates. A type outside them is refused
 * at the envelope rather than reaching a dispatcher with no handler for it.
 */
export const envelopeTypeNamespaces = [
	'state',
	'request',
	'response',
	'confirm',
	'stream',
	'script',
	'error',
] as const;

/** True when the name reads as a real envelope type: a known namespace and a member after the colon. */
export function namesAnEnvelopeNamespace(messageName: string): boolean {
	const separator = messageName.indexOf(':');

	if (separator <= 0 || separator + 1 >= messageName.length) {
		return false;
	}

	const messageNamespace = messageName.slice(0, separator);

	return (envelopeTypeNamespaces as readonly string[]).includes(messageNamespace);
}
