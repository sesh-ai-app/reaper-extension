/**
 * The payload types for the ten bridge messages, derived field for field from the
 * vendored schemas in `reaper-extension/schemas/`.
 *
 * Four are protocol payloads crossing verbatim (`messages/*.schema.json`); six are view
 * models the C++ side already decoded (`bridge-messages/*.schema.json`). A field is
 * optional here only where the schema leaves it out of `required` — which, for the view
 * models, is exactly where the C++ member is a `std::optional` and absence therefore
 * means something. Every other field is written on every publish, so a type that made
 * them optional would have the UI handling an absence the serialiser cannot produce.
 *
 * `schemas/` is generated and digest-verified. These types follow it; they do not lead.
 */

import type {
	BridgeMessageFromJavaScriptName,
	BridgeMessageToJavaScriptName,
} from './contract';

// ---------------------------------------------------------------------------
// Protocol payloads, carried verbatim
// ---------------------------------------------------------------------------

/**
 * `messages/script-download.schema.json` — the generated Lua the agent wrote, offered as
 * a download. Nothing here instructs REAPER to run anything and the UI must not treat it
 * as if it did (ADR 0011, requirement 26.2).
 */
export interface ScriptDownloadPayload {
	/** Short-lived signed S3 URL. A credential: anyone holding it can read the object, so it is never logged. */
	downloadUrl: string;

	/** Suggested file name, always ending in `.lua`. */
	fileName: string;

	/** Size of the generated script in bytes, so the download can be labelled. */
	sizeBytes: number;

	/** ISO-8601 timestamp the signed URL stops working at. Past it, show the download as stale. */
	expiresAt: string;

	/** One-line summary shown beside the download. Optional — the streamed response usually explains the script. */
	description?: string;
}

/**
 * `messages/state-prompt.schema.json` — the producer's prompt, built by the UI.
 *
 * `promptText` is exactly what the producer typed, verbatim and unframed: the standalone
 * Bedrock Guardrail's `dataPath` resolves to this field, so anything the UI wants to add
 * goes in a sibling property and never in here.
 */
export interface ProducerPromptPayload {
	/** Exactly what the producer typed or dictated. 1 to 8192 characters. */
	promptText: string;

	/** The producer's UI language as a BCP 47 tag. Carried per turn so the prompt cache is not fragmented by language. */
	locale: string;

	/** ULID of the conversation this continues. Omitted — never sent empty — to ask for a new conversation. */
	conversationId?: string;
}

/** The two verdicts a producer can give. An expiry is the server's conclusion and never a client's. */
export type ConfirmationDecision = 'approved' | 'rejected';

/**
 * `messages/confirm-decision.schema.json` — one schema for `confirm:approve` and
 * `confirm:reject`. `decision` must agree with the bridge message name; a payload that
 * disagrees with its own name is refused rather than resolved by precedence, since
 * neither reading is safe to guess at for an operation the producer was asked about.
 */
export interface ConfirmationDecisionPayload {
	/** The `requestId` the `confirm:request` carried, echoed back so the server settles the right confirmation. */
	requestId: string;

	/** The producer's verdict, stated so the payload is self-describing apart from its envelope. */
	decision: ConfirmationDecision;
}

// ---------------------------------------------------------------------------
// View models
// ---------------------------------------------------------------------------

export type ConfirmationRiskLevel = 'low' | 'medium' | 'high';

/**
 * `bridge-messages/confirmation-pending.schema.json` — the Confirmation Coordinator's
 * `PendingConfirmationView` (requirement 13.1).
 *
 * `riskLevelReported` and `descriptionComplete` are the coordinator's own conclusions and
 * appear in no protocol schema. They are the reason this is a view model rather than a
 * forwarded `confirm:request`: re-deriving them here would mean re-reading a payload the
 * C++ side already read, and risking a different answer to a question about risk.
 */
export interface PendingConfirmationView {
	/** Correlation identifier of the `confirm:request` being answered. Never empty. */
	requestId: string;

	/** One line of plain language, as the agent wrote it. camelCase here; `action_summary` in the protocol payload. */
	actionSummary: string;

	/** Longer prose, including which tracks, items, or files are affected. Empty when the coordinator could not read it. */
	details: string;

	/** How damaging the operation would be if unintended. `high` when the agent's own assessment was unreadable. */
	riskLevel: ConfirmationRiskLevel;

	/** True when `riskLevel` is the agent's assessment, false when it is the coordinator's conservative default. Say so rather than presenting a guess as a fact. */
	riskLevelReported: boolean;

	/** The server's confirmation window, for the countdown (requirement 13.4). The extension does not enforce it. */
	confirmationWindowSeconds: number;

	/** False when a required field of the request was missing — the producer is being asked to authorise something the extension could not fully describe. */
	descriptionComplete: boolean;
}

/** How a confirmation ended. `expired` is only ever the server's conclusion. */
export type ConfirmationResolution = 'approved' | 'rejected' | 'expired';

/** Which client answered a confirmation. */
export type AnsweringClient = 'extension' | 'pwa';

/**
 * `bridge-messages/confirmation-resolved.schema.json` — the coordinator's
 * `ConfirmationResolutionView` (requirements 13.5, 23.7).
 *
 * Keyed by `requestId` rather than meaning "the current prompt": another confirmation may
 * be showing and still be answerable, so a resolution dismisses the prompt holding this
 * identifier and no other.
 */
export interface ConfirmationResolutionView {
	/** Correlation identifier of the confirmation that stopped being pending. */
	requestId: string;

	/** Absent when the server's resolution string was not one of the three this build knows. The prompt still comes down. */
	resolution?: ConfirmationResolution;

	/** Which client answered, when the server said. Absent for an expiry. */
	answeringClient?: AnsweringClient;

	/** True when this extension sent the answer, so the UI can say "you approved this" rather than "approved on your phone". */
	answeredByThisClient: boolean;

	/** What this extension sent, when it sent anything. Present whether or not the server agreed. */
	localVerdict?: ConfirmationDecision;

	/** True when this extension sent a verdict and the server concluded something else — tapping Approve as the window closed. */
	contradictsLocalVerdict: boolean;

	/** True only when the resolution establishes the session was not touched: rejected, or expired (requirement 23.7). */
	nothingWasChanged: boolean;
}

/** Where the streamed response is. `idle` before anything streamed, `ended` once a stop arrived. */
export type AgentResponseState = 'idle' | 'streaming' | 'ended';

/**
 * `bridge-messages/agent-response.schema.json` — the Stream Presenter's `ResponseView`
 * (requirement 16.1).
 *
 * One message carries the whole current response rather than one per delta: the presenter
 * already holds the assembled text, placed sequence by sequence. **The UI renders `text`
 * and does not append to it** — the next publish carries the full text again.
 *
 * `missingSequences` and `textMayBeIncomplete` are requirement 16.1's gap detection,
 * already computed by the presenter. They are displayed, never recomputed here: a gap is
 * not a property of any one delta and nothing on this side sees the deltas at all.
 */
export interface AgentResponseView {
	/** False before anything has been streamed, and after a reset. Draw nothing rather than an empty assistant message. */
	active: boolean;

	/** ULID of the conversation this response belongs to. Empty while inactive. */
	conversationId: string;

	/** Identifier of the agent turn. Empty while inactive. */
	turnId: string;

	/** Identifier of the assistant message being streamed. Empty while inactive. */
	messageId: string;

	/** Where the response is. */
	state: AgentResponseState;

	/** What to render, as markdown. The fragments in sequence order while streaming; the server's assembled text once stopped. */
	text: string;

	/** True when a delta opened the response because no start envelope was seen. A diagnostic, not something to show the producer. */
	openedImplicitly: boolean;

	/** How many deltas have been placed into the text, including late ones that filled a gap. */
	appliedDeltaCount: number;

	/** The largest sequence number any delta carried. Absent until the first delta arrives. */
	highestSequenceSeen?: number;

	/** Sequence numbers below the highest seen that never arrived, ascending. Empty when nothing is outstanding. */
	missingSequences: number[];

	/** True while sequence numbers are outstanding — the one field a caller needs to read for requirement 16.1's warning. */
	textMayBeIncomplete: boolean;

	/** From the stop payload. Absent while streaming, and absent after a stop whose own `stopReason` was null. */
	stopReason?: string;

	/** True when the stop's assembled text replaced the locally accumulated fragments. */
	reconciledWithAssembledText: boolean;
}

/**
 * `bridge-messages/stream-ingest.schema.json` — the Stream Presenter's
 * `StreamIngestDisplay` (requirements 16.2, 16.3).
 *
 * **This payload is a credential.** `streamKeyPlaintext` is the IVS stream key and
 * requirement 26.4 forbids logging it. The C++ side enforces that structurally by
 * deleting the stream insertion operator for the type; TypeScript has no equivalent, so
 * the rule is stated instead: never pass this object to `console`, never put it in an
 * error message, and never store it anywhere the producer did not ask for.
 */
export interface StreamIngestDisplay {
	/** ARN of the IVS stage being published to. Safe to log and to show. */
	stageArn: string;

	/** ReaCast's destination field. The producer pastes this in. */
	rtmpsIngestUrl: string;

	/** ReaCast's stream key field, in plaintext because the producer transcribes it by hand. Never logged. */
	streamKeyPlaintext: string;

	/** The IVS participant identifier. Safe to log, and what correlates with IVS stage events. */
	participantId: string;

	/** ISO-8601 timestamp the publish token stops working at. Shown beside the token. */
	expiresAt: string;

	/** True when this token replaced an earlier one (requirement 16.3). Update the displayed token rather than adding a second set. */
	reissued: boolean;
}

/**
 * `bridge-messages/stream-error.schema.json` — the Stream Presenter's `StreamErrorView`
 * (requirements 5.5, 23.2). Covers `stream:error` and all seven `error:*` envelope types,
 * which have no bundled payload schema of their own.
 *
 * Only the last error is carried, not a list: the presenter holds one, and a second error
 * replaces the first. That is the right shape for a banner and the wrong one for a history.
 */
export interface StreamErrorView {
	/** Machine-readable code, as the server sent it. Branch and key a translation off this. */
	code: string;

	/** Producer-safe explanation, as the server sent it. English — prefer a translation keyed on `code`. */
	message: string;

	/** The envelope type that triggered the error, when the error answers one. Empty when it does not. */
	envelopeType: string;

	/** Rendered schema validation failures, when that was the cause (requirement 4.1). Prose, for diagnosis. Empty otherwise. */
	validationErrors: string;
}

/** The three states requirement 3.4 names. `reconnecting` covers both the backoff wait and an attempt in flight. */
export type ConnectionState = 'connected' | 'reconnecting' | 'disconnected';

/** Whether the producer has to do something before the extension can reconnect. */
export type RequiredProducerAction = 'none' | 'close_the_other_reaper_instance' | 'sign_in_again';

/** Why the last handshake was refused, classified. `unrecognised` means the server's protocol is ahead of this build. */
export type HandshakeRejection = 'invalid_or_expired_token'
	| 'duplicate_client_type'
	| 'unsupported_client_type'
	| 'authentication_service_unavailable'
	| 'network_or_timeout'
	| 'unrecognised';

/**
 * `bridge-messages/connection-state.schema.json` — the Transport Client's
 * `connection_status` (requirement 3.4). No envelope carries this: the connection's own
 * state cannot arrive over the connection.
 *
 * There is no field here for a token, an expiry, or any part of one, and there must never
 * be: requirement 26.1 keeps token material in memory on the C++ side only.
 */
export interface ConnectionStateView {
	/** Connected, reconnecting, or disconnected. */
	state: ConnectionState;

	/** Failures since the last successful connection, reset to zero by a success. A blip is not an outage. */
	consecutiveFailedAttempts: number;

	/** The delay currently being waited out, in milliseconds. Zero when nothing is scheduled. */
	reconnectionDelayMilliseconds: number;

	/** The field to branch and translate on. */
	requiredProducerAction: RequiredProducerAction;

	/**
	 * Producer-facing English, authored in C++. A **fallback**: render a translation
	 * selected by `state` and `requiredProducerAction` instead, or the extension is
	 * English-only for exactly the messages a producer reads when something is wrong
	 * (requirement 15.3).
	 */
	notice: string;

	/** Why the last handshake was refused. Absent when none has been since the last success. For diagnosis, not display. */
	lastRejection?: HandshakeRejection;
}

// ---------------------------------------------------------------------------
// Name to payload
// ---------------------------------------------------------------------------

/** What arrives for each inbound bridge message name. */
export interface BridgeMessagePayloadToJavaScript {
	'script:download': ScriptDownloadPayload;
	'view:confirmation_pending': PendingConfirmationView;
	'view:confirmation_resolved': ConfirmationResolutionView;
	'view:agent_response': AgentResponseView;
	'view:stream_ingest': StreamIngestDisplay;
	'view:stream_error': StreamErrorView;
	'view:connection_state': ConnectionStateView;
}

/** What the UI sends for each outbound bridge message name. */
export interface BridgeMessagePayloadFromJavaScript {
	'state:prompt': ProducerPromptPayload;
	'confirm:approve': ConfirmationDecisionPayload;
	'confirm:reject': ConfirmationDecisionPayload;
}

/** Either direction, by name. */
export type BridgeMessagePayload = BridgeMessagePayloadToJavaScript
	& BridgeMessagePayloadFromJavaScript;

/** `true` when the two type parameters are the same type, and `never` otherwise. */
type SameType<First, Second> = [First] extends [Second]
	? ([Second] extends [First] ? true : never)
	: never;

/**
 * The static assertions `ui_host.h` writes with `static_assert`: each payload map's keys
 * are exactly the names of its direction, so adding a name to the contract without a
 * payload type — or a payload type for a name that is not in the contract — is a
 * typecheck failure rather than a runtime surprise.
 */
export const toJavaScriptPayloadMapIsExhaustive: SameType<
	keyof BridgeMessagePayloadToJavaScript,
	BridgeMessageToJavaScriptName
> = true;

export const fromJavaScriptPayloadMapIsExhaustive: SameType<
	keyof BridgeMessagePayloadFromJavaScript,
	BridgeMessageFromJavaScriptName
> = true;
