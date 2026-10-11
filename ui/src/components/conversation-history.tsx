/**
 * The transcript: the producer's prompts and the assistant responses that have finished,
 * in the order they happened (requirement 15.2).
 *
 * Producer prompts render as the text that was sent, not as markdown. What the producer
 * typed crosses the bridge verbatim — `promptText` is what the standalone Bedrock
 * Guardrail's `dataPath` resolves to — and rendering it as markdown would mean the
 * transcript showing something other than what was sent, which is the one thing a
 * transcript is for. Assistant text is markdown, through `MarkdownContent`.
 *
 * The response currently streaming is not here. It is `StreamingResponse`, mounted after
 * this list by `App`, because it is one view that is replaced on every publish rather than
 * an entry in a history — and it becomes an entry here, through the reducer, only once the
 * stop arrives.
 */

import { useMemo } from 'react';
import { useTranslation } from 'react-i18next';

import type {
	AssistantMessage,
	ConversationMessage,
	ProducerMessage,
} from '../state/conversation-reducer';
import { MarkdownContent } from './markdown-content';
import { ResponseCompletenessNotice } from './response-completeness-notice';

interface ProducerMessageViewProps {
	message: ProducerMessage;
}

function ProducerMessageView({ message }: ProducerMessageViewProps) {
	const { t, i18n } = useTranslation();
	const sentAt = new Date(message.sentAt);

	return (
		<li className="conversation-history__message conversation-history__message--producer">
			<header className="conversation-history__message-header">
				<span className="conversation-history__author">{t('conversation.producerLabel')}</span>
				<time dateTime={sentAt.toISOString()}>
					{sentAt.toLocaleTimeString(i18n.language, { hour: '2-digit', minute: '2-digit' })}
				</time>
			</header>

			{/* Verbatim, deliberately — see the file comment. `white-space` is a style concern, so
			    the newlines the producer typed survive as the text they are. */}
			<p className="conversation-history__producer-text">{message.promptText}</p>
		</li>
	);
}

interface AssistantMessageViewProps {
	message: AssistantMessage;
}

function AssistantMessageView({ message }: AssistantMessageViewProps) {
	const { t } = useTranslation();

	return (
		<li className="conversation-history__message conversation-history__message--assistant">
			<header className="conversation-history__message-header">
				<span className="conversation-history__author">{t('conversation.assistantLabel')}</span>
			</header>

			<MarkdownContent markdown={message.text} />

			<ResponseCompletenessNotice
				textMayBeIncomplete={message.textMayBeIncomplete}
				missingSequences={message.missingSequences}
			/>
		</li>
	);
}

interface KeyedMessage {
	message: ConversationMessage;

	reactKey: string;
}

/**
 * Pairs each message with a key that is unique within the list.
 *
 * A producer message always has a local identifier, and an assistant message usually has
 * the server's `messageId` or its `turnId`. It can have neither: the reducer files a
 * response that named no identifier under an empty one and appends it, because there is
 * nothing to match on and silently merging two responses is worse than listing both. Two
 * of those in one transcript would share a key, so they are numbered as they are
 * encountered — the count of anonymous responses before this one is the only thing that
 * distinguishes them, and it is stable for as long as the list only grows at the end,
 * which is how the reducer grows it.
 */
function keyedMessagesFor(messages: readonly ConversationMessage[]): KeyedMessage[] {
	let anonymousResponseCount = 0;

	return messages.map((message) => {
		if (message.id.length > 0) {
			return { message, reactKey: `${message.kind}:${message.id}` };
		}

		anonymousResponseCount += 1;

		return { message, reactKey: `${message.kind}:anonymous:${anonymousResponseCount}` };
	});
}

export interface ConversationHistoryProps {
	/** `state.messages` — producer prompts and finished responses, oldest first. */
	messages: readonly ConversationMessage[];
}

export function ConversationHistory({ messages }: ConversationHistoryProps) {
	const { t } = useTranslation();

	const keyedMessages = useMemo(() => keyedMessagesFor(messages), [messages]);

	if (keyedMessages.length === 0) {
		return (
			<p className="conversation-history conversation-history--empty">
				{t('conversation.emptyHistory')}
			</p>
		);
	}

	return (
		<ol
			className="conversation-history"
			aria-label={t('conversation.historyLabel')}
		>
			{keyedMessages.map(({ message, reactKey }) => (message.kind === 'producer'
				? <ProducerMessageView key={reactKey} message={message} />
				: <AssistantMessageView key={reactKey} message={message} />))}
		</ol>
	);
}
