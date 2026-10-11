/**
 * The send half of the bridge: the three outbound bridge messages the UI originates,
 * serialized and handed to `seshAiBridgeSend`.
 *
 * Three names, two payload shapes, one schema each. The typed functions at the bottom are
 * what callers should reach for — they make the two mistakes this payload set invites
 * unrepresentable rather than merely detectable:
 *
 *   - A `confirm:approve` carrying `decision: 'rejected'`. One schema serves both names
 *     and the decision is stated twice, so the two can disagree. The server refuses the
 *     disagreement rather than resolving it by precedence, since neither reading is safe
 *     to guess at for an operation the producer was asked about. `sendConfirmationApproval`
 *     and `sendConfirmationRejection` build the payload, so there is nothing to get wrong;
 *     `sendBridgeMessage` refuses a disagreeing pair for the caller that assembles its own.
 *
 *   - A prompt whose `promptText` carries framing the UI added. That field is what the
 *     standalone Bedrock Guardrail's `dataPath` resolves to, so the producer's words go in
 *     verbatim and anything else goes in a sibling property. Nothing here trims, wraps, or
 *     normalises the text — the emptiness check reads a trimmed copy and sends the original.
 *
 * Like the receive half, nothing throws: a send answers what it did.
 */

import {
	bridgeMessageDirectionFor,
	confirmationApprovalBridgeMessageName,
	confirmationRejectionBridgeMessageName,
	isBridgeMessageFromJavaScript,
	producerPromptBridgeMessageName,
	type BridgeMessageFromJavaScriptName,
} from './contract';
import type {
	BridgeMessagePayloadFromJavaScript,
	ConfirmationDecision,
	ConfirmationDecisionPayload,
	ProducerPromptPayload,
} from './payloads';
import {
	defaultBridgeGlobal,
	sendRawBridgeMessage,
	type BridgeGlobal,
	type RawBridgeSendDisposition,
} from './transport';

/**
 * `state-prompt.schema.json`'s bound on `promptText`, in Unicode code points — which is
 * what JSON Schema's `maxLength` counts, and not what `String.prototype.length` returns
 * for text outside the basic multilingual plane. A producer whose prompt ends in an emoji
 * should not be refused one character early, nor accepted one character late.
 */
export const maximumPromptTextLength = 8192;

/** Everything the raw transport can answer, plus the refusals this layer adds. */
export type BridgeSendDisposition = RawBridgeSendDisposition

	/** Not one of the ten names in the contract. */
	| 'unknown_message_name'

	/** A real name, but one that only travels from C++ to JavaScript. */
	| 'wrong_direction'

	/** A `confirm:approve` carrying a rejection, or the reverse. */
	| 'decision_contradicts_message_name'

	/** A confirmation decision with no `requestId`. The server could not settle it. */
	| 'empty_request_id'

	/** Nothing but whitespace to send. Not an error worth a banner — the UI simply does not send it. */
	| 'empty_prompt_text'

	/** Past `maximumPromptTextLength`, which the schema would refuse further down the line. */
	| 'prompt_text_too_long'

	/** No locale on a prompt. The schema requires one, and the server fragments nothing without it. */
	| 'empty_locale'

	/** `JSON.stringify` could not produce a payload — a cycle, or a value that serialises to nothing. */
	| 'not_serializable';

export interface BridgeSendOutcome {
	messageName: string;

	disposition: BridgeSendDisposition;

	/** Plain language for a log line, when the disposition is not `sent`. */
	reason?: string;
}

/** Where a refused or failed send is reported. `console` by default. */
export interface BridgeSendLogger {
	warn(message: string): void;
}

const consoleLogger: BridgeSendLogger = {
	warn: (message: string) => {
		console.warn(message);
	},
};

export interface BridgeSendOptions {
	/** The globals to send through. The page's own by default. */
	target?: BridgeGlobal;

	logger?: BridgeSendLogger;
}

/** The decision each confirmation message name asserts. The pair the payload must agree with. */
const decisionForConfirmationMessageName: Readonly<Record<string, ConfirmationDecision>> = {
	[confirmationApprovalBridgeMessageName]: 'approved',
	[confirmationRejectionBridgeMessageName]: 'rejected',
};

function countCodePoints(value: string): number {
	return Array.from(value).length;
}

/** The payload guards that are the schema's own rules, by name. Empty reason means it passed. */
function refusalFor(
	messageName: BridgeMessageFromJavaScriptName,
	payload: BridgeMessagePayloadFromJavaScript[BridgeMessageFromJavaScriptName],
): BridgeSendOutcome | undefined {
	if (messageName === producerPromptBridgeMessageName) {
		const prompt = payload as ProducerPromptPayload;

		if (prompt.promptText === undefined || prompt.promptText.trim().length === 0) {
			return {
				messageName,
				disposition: 'empty_prompt_text',
				reason: 'the prompt carried no text for the agent to answer',
			};
		}

		if (countCodePoints(prompt.promptText) > maximumPromptTextLength) {
			return {
				messageName,
				disposition: 'prompt_text_too_long',
				reason: `the prompt schema accepts ${maximumPromptTextLength} characters and this one carried more`,
			};
		}

		if (prompt.locale === undefined || prompt.locale.length === 0) {
			return {
				messageName,
				disposition: 'empty_locale',
				reason: 'the prompt schema requires the producer locale as a BCP 47 tag',
			};
		}

		return undefined;
	}

	const decision = payload as ConfirmationDecisionPayload;

	if (decision.requestId === undefined || decision.requestId.length === 0) {
		return {
			messageName,
			disposition: 'empty_request_id',
			reason: 'a confirmation decision with no requestId settles nothing',
		};
	}

	const assertedDecision = decisionForConfirmationMessageName[messageName];

	if (assertedDecision !== undefined && decision.decision !== assertedDecision) {
		return {
			messageName,
			disposition: 'decision_contradicts_message_name',
			reason: `${messageName} asserts ${assertedDecision} and the payload said ${String(decision.decision)}`,
		};
	}

	return undefined;
}

/**
 * Serializes an outbound bridge message and hands it to the renderer binding.
 *
 * Generic over the name so each name's payload type is the one the compiler enforces at
 * the call site. The schema's own rules that are cheap to check here — a decision that
 * contradicts its name, an empty prompt, a missing locale — are refused before the send
 * rather than after, because the alternative is an `error:*` envelope coming back for
 * something the UI knew was wrong when it built it.
 */
export function sendBridgeMessage<Name extends BridgeMessageFromJavaScriptName>(
	messageName: Name,
	payload: BridgeMessagePayloadFromJavaScript[Name],
	options: BridgeSendOptions = {},
): BridgeSendOutcome {
	const logger = options.logger ?? consoleLogger;
	const target = options.target ?? defaultBridgeGlobal();

	const report = (outcome: BridgeSendOutcome): BridgeSendOutcome => {
		if (outcome.disposition !== 'sent') {
			const suffix = outcome.reason === undefined ? '' : `: ${outcome.reason}`;

			logger.warn(`[sesh-ai bridge] ${outcome.disposition} for "${outcome.messageName}"${suffix}`);
		}

		return outcome;
	};

	if (!isBridgeMessageFromJavaScript(messageName)) {
		if (bridgeMessageDirectionFor(messageName) === 'toJavaScript') {
			return report({
				messageName,
				disposition: 'wrong_direction',
				reason: 'this message only travels from C++ to JavaScript',
			});
		}

		return report({
			messageName,
			disposition: 'unknown_message_name',
			reason: 'not one of the ten names in the bridge contract',
		});
	}

	const refusal = refusalFor(messageName, payload);

	if (refusal !== undefined) {
		return report(refusal);
	}

	let serializedJson: string;

	try {
		serializedJson = JSON.stringify(payload);
	} catch (error) {
		return report({
			messageName,
			disposition: 'not_serializable',
			reason: error instanceof Error ? error.message : String(error),
		});
	}

	// `JSON.stringify` answers `undefined` rather than throwing for a value with no JSON
	// representation at all, which is a payload the UI Host would refuse as empty.
	if (typeof serializedJson !== 'string' || serializedJson.length === 0) {
		return report({
			messageName,
			disposition: 'not_serializable',
			reason: 'the payload has no JSON representation',
		});
	}

	const raw = sendRawBridgeMessage(messageName, serializedJson, target);

	return report(
		raw.reason === undefined
			? { messageName, disposition: raw.disposition }
			: { messageName, disposition: raw.disposition, reason: raw.reason },
	);
}

/** The producer's prompt. `promptText` crosses verbatim — see the file comment. */
export function sendProducerPrompt(
	payload: ProducerPromptPayload,
	options?: BridgeSendOptions,
): BridgeSendOutcome {
	return sendBridgeMessage(producerPromptBridgeMessageName, payload, options);
}

/** The producer approved. The payload is built here, so it cannot disagree with the name. */
export function sendConfirmationApproval(
	requestId: string,
	options?: BridgeSendOptions,
): BridgeSendOutcome {
	return sendBridgeMessage(
		confirmationApprovalBridgeMessageName,
		{ requestId, decision: 'approved' },
		options,
	);
}

/** The producer rejected. */
export function sendConfirmationRejection(
	requestId: string,
	options?: BridgeSendOptions,
): BridgeSendOutcome {
	return sendBridgeMessage(
		confirmationRejectionBridgeMessageName,
		{ requestId, decision: 'rejected' },
		options,
	);
}
