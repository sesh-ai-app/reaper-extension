/**
 * What the assistant is doing between the prompt and the answer (requirement 15.2).
 *
 * ---------------------------------------------------------------------------
 * What this can say, and why it is not per-tool
 *
 * The bridge carries no tool-level activity. Its ten names are in `src/ui/ui_host.h` and
 * the inbound seven are the ReaScript download and six view models; none of them describes
 * a tool call starting or finishing, and the UI never sees the `request:tool` and
 * `response:tool` envelopes the extension exchanges with the server. So the phase here is
 * derived from the only activity this side can observe — a prompt was sent, a response is
 * streaming, a turn ended — rather than invented from a payload that does not exist. A
 * list of tool names would need a new view model and a C++ publisher to fill it.
 *
 * The derivation is a function of the state rather than a flag in it, so it cannot drift
 * out of agreement with the transcript it sits under.
 */

import { useTranslation } from 'react-i18next';

import type { ConversationState } from '../state/conversation-reducer';

/** `idle` is nothing in flight: no turn open, or the last one ended. Draw nothing for it. */
export type ToolActivityPhase = 'idle'

	/** A prompt is out and nothing has come back yet — the agent is reasoning, or calling tools. */
	| 'awaitingResponse'

	/** Text is arriving. */
	| 'streamingResponse';

/** The three fields the phase is a function of. A view, so a caller need not hold the whole state. */
export type ToolActivityInput = Pick<
	ConversationState,
	'messages' | 'streamingResponse' | 'streamError'
>;

/**
 * Where the current turn is.
 *
 * An error ends the turn as far as this indicator is concerned: the presenter published a
 * failure, so nothing is still being waited for, and leaving a spinner up under the error
 * banner would say the opposite. The error itself is shown elsewhere — the reducer clears
 * it when the next prompt goes out, which is also when this starts waiting again.
 */
export function toolActivityPhaseFor(state: ToolActivityInput): ToolActivityPhase {
	if (state.streamingResponse?.state === 'streaming') {
		return 'streamingResponse';
	}

	if (state.streamError !== null) {
		return 'idle';
	}

	// The transcript grows at the end, so the last entry is the turn's own: a producer
	// prompt with nothing after it is a prompt still unanswered. A finished response is
	// appended to the same list, which is what takes this back to idle.
	return state.messages.at(-1)?.kind === 'producer' ? 'awaitingResponse' : 'idle';
}

export interface ToolActivityIndicatorProps {
	phase: ToolActivityPhase;
}

export function ToolActivityIndicator({ phase }: ToolActivityIndicatorProps) {
	const { t } = useTranslation();

	if (phase === 'idle') {
		return null;
	}

	const noticeKey = phase === 'streamingResponse'
		? 'toolActivity.streamingResponse'
		: 'toolActivity.awaitingResponse';

	return (
		<p
			className={`tool-activity-indicator tool-activity-indicator--${phase}`}
			role="status"
			aria-live="polite"
		>
			{t(noticeKey)}
		</p>
	);
}
