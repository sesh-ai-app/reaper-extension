/**
 * Where the producer types a prompt.
 *
 * The text crosses verbatim. `promptText` is what the standalone Bedrock Guardrail's
 * `dataPath` resolves to, so nothing here trims, wraps, or normalises what was typed —
 * the emptiness check reads a trimmed copy and sends the original, which is the same rule
 * `send.ts` follows one layer down.
 *
 * The length is counted in code points rather than UTF-16 units, because that is what
 * JSON Schema's `maxLength` counts: a producer whose prompt ends in an emoji should not be
 * refused one character early, nor accepted one character late and told so by the server.
 *
 * A send answers what it did rather than throwing (the bridge's convention throughout), so
 * a refusal is state here: the text stays in the box, the reason is shown, and the producer
 * can try again without retyping. Enter sends and Shift+Enter inserts a newline, which is
 * the convention in every chat surface a producer already uses; the newlines survive into
 * `promptText` unchanged.
 */

import {
	useId,
	useState,
	type ChangeEvent,
	type FormEvent,
	type KeyboardEvent,
} from 'react';
import { useTranslation } from 'react-i18next';

import {
	maximumPromptTextLength,
	type BridgeSendDisposition,
	type BridgeSendOutcome,
} from '../bridge/send';

/**
 * How close to the limit the counter appears. A permanent count against a bound of 8192 is
 * noise for every prompt a producer actually types.
 */
const promptLengthCounterThreshold = maximumPromptTextLength - 256;

/**
 * The refusals worth explaining, and what to say. Everything else gets the general key:
 * `unknown_message_name`, `wrong_direction`, and the confirmation-decision refusals cannot
 * happen for a prompt this component built, and a producer has no use for the distinction
 * between a payload that would not serialise and a transport that threw.
 */
const refusalTranslationKeys: Readonly<Partial<Record<BridgeSendDisposition, string>>> = {
	prompt_text_too_long: 'producerPrompt.refusal.promptTextTooLong',
	transport_unavailable: 'producerPrompt.refusal.transportUnavailable',
	transport_threw: 'producerPrompt.refusal.transportUnavailable',
};

function countCodePoints(value: string): number {
	return Array.from(value).length;
}

export interface ProducerPromptInputProps {
	/** The conversation hook's `sendPrompt`. Answers the outcome; never throws. */
	sendPrompt: (promptText: string) => BridgeSendOutcome;
}

export function ProducerPromptInput({ sendPrompt }: ProducerPromptInputProps) {
	const { t } = useTranslation();
	const inputIdentifier = useId();

	const [promptText, setPromptText] = useState('');
	const [refusal, setRefusal] = useState<BridgeSendDisposition | null>(null);

	const codePointCount = countCodePoints(promptText);
	const isEmpty = promptText.trim().length === 0;
	const isTooLong = codePointCount > maximumPromptTextLength;
	const canSend = !isEmpty && !isTooLong;

	const submitPrompt = () => {
		if (!canSend) {
			return;
		}

		const outcome = sendPrompt(promptText);

		if (outcome.disposition === 'sent') {
			setPromptText('');
			setRefusal(null);

			return;
		}

		setRefusal(outcome.disposition);
	};

	const handleSubmit = (event: FormEvent<HTMLFormElement>) => {
		event.preventDefault();
		submitPrompt();
	};

	const handleChange = (event: ChangeEvent<HTMLTextAreaElement>) => {
		setPromptText(event.target.value);

		// A refusal describes the text that was refused. Once it has been edited, the notice
		// is about something that no longer exists.
		setRefusal(null);
	};

	const handleKeyDown = (event: KeyboardEvent<HTMLTextAreaElement>) => {
		const isPlainEnter = event.key === 'Enter'
			&& !event.shiftKey
			&& !event.altKey
			&& !event.ctrlKey
			&& !event.metaKey;

		if (!isPlainEnter) {
			return;
		}

		event.preventDefault();
		submitPrompt();
	};

	const refusalKey = refusal === null
		? null
		: refusalTranslationKeys[refusal] ?? 'producerPrompt.refusal.notSent';

	return (
		<form className="producer-prompt-input" onSubmit={handleSubmit}>
			<label className="producer-prompt-input__label" htmlFor={inputIdentifier}>
				{t('producerPrompt.inputLabel')}
			</label>

			<textarea
				id={inputIdentifier}
				className="producer-prompt-input__text"
				value={promptText}
				placeholder={t('producerPrompt.inputPlaceholder')}
				rows={3}
				onChange={handleChange}
				onKeyDown={handleKeyDown}
			/>

			<div className="producer-prompt-input__footer">
				{codePointCount >= promptLengthCounterThreshold && (
					<p className="producer-prompt-input__counter">
						{t('producerPrompt.charactersRemaining', {
							remaining: maximumPromptTextLength - codePointCount,
						})}
					</p>
				)}

				<button
					type="submit"
					className="producer-prompt-input__send"
					disabled={!canSend}
				>
					{t('producerPrompt.send')}
				</button>
			</div>

			{refusalKey !== null && (
				<p className="producer-prompt-input__refusal" role="alert">
					{t(refusalKey)}
				</p>
			)}
		</form>
	);
}
