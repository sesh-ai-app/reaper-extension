/**
 * fast-check generators for every bridge message payload, one per name, constrained to
 * what the payload's schema actually accepts.
 *
 * Written per message rather than as one loose object generator for the reason the
 * server's `tool-input-arbitraries.fixture.js` gives: a "valid" case has to really be
 * valid, or a round-trip property proves only that `JSON.parse` undoes `JSON.stringify`.
 * Bounds here are the schemas' own — `minLength`, `maxLength`, `minimum`, the enums — and
 * the optional fields are generated as present or absent, since absence is meaningful for
 * every one of them.
 *
 * Each generator names its payload type, so a type that gains a field is a compile error
 * here until the generator covers it.
 */

import fc from 'fast-check';

import {
	agentResponseBridgeMessageName,
	allBridgeMessageNames,
	confirmationApprovalBridgeMessageName,
	confirmationRejectionBridgeMessageName,
	confirmationResolutionBridgeMessageName,
	connectionStateBridgeMessageName,
	pendingConfirmationBridgeMessageName,
	producerPromptBridgeMessageName,
	scriptDownloadBridgeMessageName,
	streamErrorBridgeMessageName,
	streamIngestBridgeMessageName,
	type BridgeMessageName,
} from './contract';
import type {
	AgentResponseView,
	ConfirmationDecisionPayload,
	ConfirmationResolutionView,
	ConnectionStateView,
	PendingConfirmationView,
	ProducerPromptPayload,
	ScriptDownloadPayload,
	StreamErrorView,
	StreamIngestDisplay,
} from './payloads';

/**
 * A generator for one payload type: an arbitrary per field, every field of the type
 * required here even where the type makes it optional, and the fields whose value came out
 * `undefined` deleted from the generated object.
 *
 * The deletion is what makes an "absent" optional field genuinely absent.
 * `exactOptionalPropertyTypes` draws that distinction in the types and the schemas draw it
 * in `required`, so a present `undefined` would be neither — and a round-trip property
 * comparing against `JSON.parse` output would fail on it.
 *
 * The one cast in this file lives here. TypeScript cannot see through `Object.fromEntries`
 * that `{ description: string | undefined }` has become `{ description?: string }`, and
 * `fc.record`'s own type is the former. Stating it once, in the helper every generator goes
 * through, keeps it out of the generators themselves — where the declared payload type is
 * then free to do its job.
 */
function payloadArbitrary<Payload extends object>(
	fieldArbitraries: { [Key in keyof Payload]-?: fc.Arbitrary<Payload[Key] | undefined> },
): fc.Arbitrary<Payload> {
	return fc
		.record(fieldArbitraries as Record<string, fc.Arbitrary<unknown>>)
		.map((generated) => Object.fromEntries(
			Object.entries(generated).filter(([, value]) => value !== undefined),
		) as Payload);
}

/** Non-empty text within a schema bound. */
function textArbitrary(maxLength: number): fc.Arbitrary<string> {
	return fc.string({ minLength: 1, maxLength: Math.min(maxLength, 64) });
}

/** Text a schema allows to be empty — an unset `details`, an absent `envelopeType`. */
function possiblyEmptyTextArbitrary(maxLength: number): fc.Arbitrary<string> {
	return fc.string({ maxLength: Math.min(maxLength, 64) });
}

/** A ULID-shaped identifier. The schemas bound the length and say nothing else. */
const identifierArbitrary = fc.string({ minLength: 1, maxLength: 26 });

/** ISO-8601, which is what `format: date-time` means here. */
const timestampArbitrary = fc
	.date({
		min: new Date('2024-01-01T00:00:00.000Z'),
		max: new Date('2030-01-01T00:00:00.000Z'),
		// fast-check will otherwise draw an invalid date, which has no ISO-8601 rendering at
		// all and is not a timestamp the C++ serialiser can produce.
		noInvalidDate: true,
	})
	.map((moment) => moment.toISOString());

/** Present or absent, because absence is meaningful for every optional field in this set. */
function optional<Value>(arbitrary: fc.Arbitrary<Value>): fc.Arbitrary<Value | undefined> {
	return fc.oneof(arbitrary, fc.constant(undefined));
}

export const scriptDownloadPayloadArbitrary = payloadArbitrary<ScriptDownloadPayload>({
	downloadUrl: textArbitrary(2048).map((suffix) => `https://uploads.example/${suffix}`),
	fileName: fc
		.string({ minLength: 1, maxLength: 32 })
		.map((stem) => `${stem.replace(/[^A-Za-z0-9 ._-]/gu, '-')}.lua`)
		.map((fileName) => (/^[A-Za-z0-9]/u.test(fileName) ? fileName : `a${fileName}`)),
	sizeBytes: fc.integer({ min: 1, max: 1_048_576 }),
	expiresAt: timestampArbitrary,
	description: optional(textArbitrary(1024)),
});

export const producerPromptPayloadArbitrary = payloadArbitrary<ProducerPromptPayload>({
	// Non-whitespace, because the send path refuses a prompt with nothing in it and that
	// refusal is its own test rather than a round-trip failure.
	promptText: fc.string({ minLength: 1, maxLength: 256 }).map((text) => `${text.trim()}x`),
	locale: fc.constantFrom('en', 'de', 'ja', 'pt-BR', 'es', 'fr', 'it'),
	conversationId: optional(identifierArbitrary),
});

export const confirmationApprovalPayloadArbitrary = payloadArbitrary<ConfirmationDecisionPayload>({
	requestId: fc.string({ minLength: 1, maxLength: 128 }),
	decision: fc.constant('approved' as const),
});

export const confirmationRejectionPayloadArbitrary = payloadArbitrary<ConfirmationDecisionPayload>({
	requestId: fc.string({ minLength: 1, maxLength: 128 }),
	decision: fc.constant('rejected' as const),
});

export const pendingConfirmationViewArbitrary = payloadArbitrary<PendingConfirmationView>({
	requestId: fc.string({ minLength: 1, maxLength: 128 }),
	actionSummary: possiblyEmptyTextArbitrary(1024),
	details: possiblyEmptyTextArbitrary(8192),
	riskLevel: fc.constantFrom('low' as const, 'medium' as const, 'high' as const),
	riskLevelReported: fc.boolean(),
	confirmationWindowSeconds: fc.integer({ min: 1, max: 600 }),
	descriptionComplete: fc.boolean(),
});

export const confirmationResolutionViewArbitrary = payloadArbitrary<ConfirmationResolutionView>({
	requestId: fc.string({ minLength: 1, maxLength: 128 }),
	resolution: optional(
		fc.constantFrom('approved' as const, 'rejected' as const, 'expired' as const),
	),
	answeringClient: optional(fc.constantFrom('extension' as const, 'pwa' as const)),
	answeredByThisClient: fc.boolean(),
	localVerdict: optional(fc.constantFrom('approved' as const, 'rejected' as const)),
	contradictsLocalVerdict: fc.boolean(),
	nothingWasChanged: fc.boolean(),
});

export const agentResponseViewArbitrary = payloadArbitrary<AgentResponseView>({
	active: fc.boolean(),
	conversationId: fc.string({ maxLength: 26 }),
	turnId: fc.string({ maxLength: 26 }),
	messageId: fc.string({ maxLength: 26 }),
	state: fc.constantFrom('idle' as const, 'streaming' as const, 'ended' as const),
	text: possiblyEmptyTextArbitrary(512),
	openedImplicitly: fc.boolean(),
	appliedDeltaCount: fc.nat({ max: 512 }),
	highestSequenceSeen: optional(fc.nat({ max: 512 })),
	// Ascending and distinct, which is what the schema's description states.
	missingSequences: fc
		.uniqueArray(fc.nat({ max: 512 }), { maxLength: 8 })
		.map((sequences) => [...sequences].sort((first, second) => first - second)),
	textMayBeIncomplete: fc.boolean(),
	stopReason: optional(textArbitrary(64)),
	reconciledWithAssembledText: fc.boolean(),
});

/**
 * A response that is actually being drawn, for the state properties that only mean
 * something while one is in flight.
 */
export const activeAgentResponseViewArbitrary = agentResponseViewArbitrary.map((view) => ({
	...view,
	active: true,
	conversationId: view.conversationId.length > 0 ? view.conversationId : 'conversation',
	messageId: view.messageId.length > 0 ? view.messageId : 'message',
}));

export const streamIngestDisplayArbitrary = payloadArbitrary<StreamIngestDisplay>({
	stageArn: textArbitrary(2048).map((suffix) => `arn:aws:ivs:us-east-1:1:stage/${suffix}`),
	rtmpsIngestUrl: textArbitrary(2048).map((suffix) => `rtmps://ingest.example/${suffix}`),
	streamKeyPlaintext: textArbitrary(2048),
	participantId: textArbitrary(128),
	expiresAt: timestampArbitrary,
	reissued: fc.boolean(),
});

export const streamErrorViewArbitrary = payloadArbitrary<StreamErrorView>({
	code: possiblyEmptyTextArbitrary(128),
	message: possiblyEmptyTextArbitrary(2048),
	envelopeType: possiblyEmptyTextArbitrary(128),
	validationErrors: possiblyEmptyTextArbitrary(8192),
});

export const connectionStateViewArbitrary = payloadArbitrary<ConnectionStateView>({
	state: fc.constantFrom('connected' as const, 'reconnecting' as const, 'disconnected' as const),
	consecutiveFailedAttempts: fc.nat({ max: 64 }),
	reconnectionDelayMilliseconds: fc.nat({ max: 30_000 }),
	requiredProducerAction: fc.constantFrom(
		'none' as const,
		'close_the_other_reaper_instance' as const,
		'sign_in_again' as const,
	),
	notice: possiblyEmptyTextArbitrary(1024),
	lastRejection: optional(
		fc.constantFrom(
			'invalid_or_expired_token' as const,
			'duplicate_client_type' as const,
			'unsupported_client_type' as const,
			'authentication_service_unavailable' as const,
			'network_or_timeout' as const,
			'unrecognised' as const,
		),
	),
});

/**
 * One arbitrary per bridge message name, keyed by the name. Typed as a total record over
 * `BridgeMessageName`, so a name added to the contract without a generator here is a
 * typecheck failure — which is the point of the coverage property that reads it.
 */
export const payloadArbitraryForBridgeMessage: Readonly<
	Record<BridgeMessageName, fc.Arbitrary<object>>
> = {
	[scriptDownloadBridgeMessageName]: scriptDownloadPayloadArbitrary,
	[producerPromptBridgeMessageName]: producerPromptPayloadArbitrary,
	[confirmationApprovalBridgeMessageName]: confirmationApprovalPayloadArbitrary,
	[confirmationRejectionBridgeMessageName]: confirmationRejectionPayloadArbitrary,
	[pendingConfirmationBridgeMessageName]: pendingConfirmationViewArbitrary,
	[confirmationResolutionBridgeMessageName]: confirmationResolutionViewArbitrary,
	[agentResponseBridgeMessageName]: agentResponseViewArbitrary,
	[streamIngestBridgeMessageName]: streamIngestDisplayArbitrary,
	[streamErrorBridgeMessageName]: streamErrorViewArbitrary,
	[connectionStateBridgeMessageName]: connectionStateViewArbitrary,
};

/** A name and a payload valid for it, drawn together. */
export const bridgeMessageWithPayloadArbitrary: fc.Arbitrary<{
	messageName: BridgeMessageName;
	payload: object;
}> = fc
	.constantFrom(...allBridgeMessageNames)
	.chain((messageName) => payloadArbitraryForBridgeMessage[messageName].map((payload) => ({
		messageName,
		payload,
	})));

/** A string that is not one of the ten names, for the unknown-name properties. */
export const unknownBridgeMessageNameArbitrary: fc.Arbitrary<string> = fc
	.string({ minLength: 1, maxLength: 48 })
	.filter(
		(candidate) => !(allBridgeMessageNames as readonly string[]).includes(candidate),
	);
