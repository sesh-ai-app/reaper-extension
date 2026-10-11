/**
 * The response being drawn right now (requirement 16.1).
 *
 * **It renders `view.text` and never appends to it.** `view:agent_response` carries the
 * Stream Presenter's whole current response, not a delta: the presenter holds the
 * assembled text, places each fragment by its sequence number, and publishes the view
 * again each time it changes. So the component is a function of the latest view and holds
 * no accumulated text of its own. Appending would duplicate every fragment, and the bug
 * would look like the agent stuttering rather than like a bug here.
 *
 * Rendered as markdown while it arrives, which means the parser is routinely handed an
 * unclosed fence or half a list. That is fine — it renders what is there and the next
 * publish completes it.
 *
 * The region is `aria-live="polite"`: a screen reader should hear the response as it
 * lands, but not have each fragment interrupt the last.
 */

import { useTranslation } from 'react-i18next';

import type { AgentResponseView } from '../bridge/payloads';
import { MarkdownContent } from './markdown-content';
import { ResponseCompletenessNotice } from './response-completeness-notice';

export interface StreamingResponseProps {
	/**
	 * `state.streamingResponse` — the latest published view, or `null` when there is
	 * nothing being drawn. Null rather than an inactive view, so this component never has
	 * to know that `active: false` means "draw nothing".
	 */
	view: AgentResponseView | null;
}

export function StreamingResponse({ view }: StreamingResponseProps) {
	const { t } = useTranslation();

	if (view === null) {
		return null;
	}

	return (
		<section
			className="streaming-response"
			aria-label={t('streamingResponse.regionLabel')}
			aria-live="polite"
			aria-busy
		>
			<header className="streaming-response__header">
				<span className="streaming-response__author">{t('conversation.assistantLabel')}</span>
			</header>

			{view.text.length === 0
				? (
					<p className="streaming-response__awaiting-text">
						{t('streamingResponse.awaitingFirstText')}
					</p>
				)
				: <MarkdownContent markdown={view.text} />}

			<ResponseCompletenessNotice
				textMayBeIncomplete={view.textMayBeIncomplete}
				missingSequences={view.missingSequences}
			/>
		</section>
	);
}
