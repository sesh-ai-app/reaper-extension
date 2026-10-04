import { useEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';

import {
	ConfirmationPrompt,
	ConnectionStatus,
	ConversationHistory,
	ProducerPromptInput,
	ScriptDownloadList,
	StreamErrorBanner,
	StreamIngestDetails,
	StreamingResponse,
	ToolActivityIndicator,
	toolActivityPhaseFor,
} from './components';
import { useConversationState } from './state/use-conversation-state';

/**
 * Root component, and the one place the conversation state is held.
 *
 * `useConversationState` installs the bridge receiver, so it is called once, here, rather
 * than in each component that needs part of the state: the bridge has room for one
 * `seshAiBridge.receive`, and a second call would replace the first dispatcher. The
 * components below take exactly the part of the state they render and none of them knows
 * the bridge exists.
 *
 * The producer's locale goes in rather than being left to default: the prompt carries it
 * per turn, and a panel displaying German while telling the agent `en` gets English
 * answers to a producer reading German.
 *
 * ---------------------------------------------------------------------------
 * The layout, and why each piece sits where it does
 *
 * The transcript is the only part that scrolls, which is what decides the rest. Anything
 * the producer reads *about* a message belongs inside it, and anything they act on or copy
 * from belongs outside, where it cannot scroll away mid-task:
 *
 *   - the connection status is in the header, because it qualifies everything below it;
 *   - the error banner is above the transcript, where it is read before the text it
 *     explains the absence of;
 *   - the ReaCast ingest details are a persistent panel rather than a transcript entry.
 *     The producer transcribes the stream key into ReaCast by hand (requirement 16.2), and
 *     a set of credentials that scrolls out of view halfway through being typed is the one
 *     place in this layout where scrolling actively costs something. It appears only while
 *     there is a stream, and the reducer replaces the details when a token is reissued, so
 *     there is never more than one set on screen;
 *   - the ReaScript downloads are the last thing in the transcript, next to the response
 *     that produced them;
 *   - the confirmation prompt is directly above the box the producer is already looking at.
 */
function App() {
	const { t, i18n } = useTranslation();

	const conversation = useConversationState({ locale: i18n.language });
	const { state } = conversation;

	const transcriptRef = useRef<HTMLDivElement>(null);

	// Keep the newest text in view. The streamed response is republished in full on every
	// change, so this runs as it grows — and reads the live `scrollHeight` rather than
	// assuming a height, since the rendered markdown decides it.
	useEffect(() => {
		const transcript = transcriptRef.current;

		if (transcript === null) {
			return;
		}

		transcript.scrollTop = transcript.scrollHeight;
	}, [state.messages, state.streamingResponse]);

	return (
		<main className="application">
			<header className="application__header">
				<h1>{t('application.name')}</h1>
				<p>{t('application.tagline')}</p>

				<ConnectionStatus connection={state.connection} />
			</header>

			<StreamErrorBanner error={state.streamError} dismiss={conversation.dismissStreamError} />

			<StreamIngestDetails ingest={state.streamIngest} />

			<div className="application__transcript" ref={transcriptRef}>
				<ConversationHistory messages={state.messages} />

				<StreamingResponse view={state.streamingResponse} />

				<ToolActivityIndicator phase={toolActivityPhaseFor(state)} />

				<ScriptDownloadList downloads={state.scriptDownloads} />
			</div>

			<ConfirmationPrompt
				confirmation={state.pendingConfirmation}
				answerSubmitted={state.confirmationAnswerSubmitted}
				resolution={state.latestConfirmationResolution}
				approve={conversation.approveConfirmation}
				reject={conversation.rejectConfirmation}
			/>

			<ProducerPromptInput sendPrompt={conversation.sendPrompt} />
		</main>
	);
}

export default App;
