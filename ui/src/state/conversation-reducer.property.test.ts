/**
 * Property tests for the conversation reducer.
 *
 * The properties here are the three the streaming display rests on: the text shown is the
 * last one published and never a concatenation, the presenter's gap detection crosses
 * unchanged, and a resolution only dismisses the prompt it names.
 *
 * **Validates: Requirements 13.1, 13.5, 15.2, 16.1, 23.7**
 */

import fc from 'fast-check';
import { describe, expect, it } from 'vitest';

import {
	activeAgentResponseViewArbitrary,
	agentResponseViewArbitrary,
	confirmationResolutionViewArbitrary,
	connectionStateViewArbitrary,
	pendingConfirmationViewArbitrary,
	streamErrorViewArbitrary,
	streamIngestDisplayArbitrary,
} from '../bridge/payload-arbitraries.fixture';
import {
	conversationReducer,
	initialConversationState,
	type ConversationAction,
	type ConversationState,
} from './conversation-reducer';

function applyAll(actions: readonly ConversationAction[]): ConversationState {
	return actions.reduce(conversationReducer, initialConversationState);
}

describe('the streamed response is shown, never assembled', () => {
	it('shows the last published text for any sequence of publishes', () => {
		fc.assert(
			fc.property(
				fc.array(
					activeAgentResponseViewArbitrary.map((view) => ({ ...view, state: 'streaming' as const })),
					{ minLength: 1, maxLength: 12 },
				),
				(views) => {
					const state = applyAll(
						views.map((view) => ({ kind: 'agentResponsePublished' as const, view })),
					);
					const last = views[views.length - 1];

					expect(state.streamingResponse?.text).toBe(last?.text);

					// Nothing accumulates: the text shown is one publish's text, so its length
					// can never exceed the longest single publish.
					const longest = Math.max(...views.map((view) => view.text.length));
					expect(state.streamingResponse?.text.length ?? 0).toBeLessThanOrEqual(longest);
				},
			),
		);
	});

	it('passes the presenter gap detection through for any view', () => {
		fc.assert(
			fc.property(activeAgentResponseViewArbitrary, (view) => {
				const state = conversationReducer(initialConversationState, {
					kind: 'agentResponsePublished',
					view,
				});

				const shown = view.state === 'ended'
					? state.messages[state.messages.length - 1]
					: state.streamingResponse;

				expect(shown).not.toBeNull();
				expect(shown).toMatchObject({
					textMayBeIncomplete: view.textMayBeIncomplete,
					missingSequences: view.missingSequences,
				});
			}),
		);
	});

	it('draws nothing whenever the view is inactive, whatever else it says', () => {
		fc.assert(
			fc.property(
				agentResponseViewArbitrary.map((view) => ({ ...view, active: false })),
				(view) => {
					const state = conversationReducer(initialConversationState, {
						kind: 'agentResponsePublished',
						view,
					});

					expect(state.streamingResponse).toBeNull();
					expect(state.messages).toHaveLength(0);
				},
			),
		);
	});

	it('never holds a streaming response and an unfinished transcript entry at once', () => {
		fc.assert(
			fc.property(
				fc.array(agentResponseViewArbitrary, { maxLength: 12 }),
				(views) => {
					const state = applyAll(
						views.map((view) => ({ kind: 'agentResponsePublished' as const, view })),
					);

					// A finished response is in the transcript and nowhere else; an unfinished one
					// is the streaming slot and nowhere else. The panel draws both, and drawing
					// one response twice is the bug this rules out.
					if (state.streamingResponse !== null) {
						expect(state.streamingResponse.active).toBe(true);
						expect(state.streamingResponse.state).not.toBe('ended');
					}
				},
			),
		);
	});
});

describe('a confirmation resolution dismisses the prompt it names and no other', () => {
	it('holds for any pending prompt and any resolution', () => {
		fc.assert(
			fc.property(
				pendingConfirmationViewArbitrary,
				confirmationResolutionViewArbitrary,
				(pending, resolution) => {
					const state = applyAll([
						{ kind: 'confirmationPending', view: pending },
						{ kind: 'confirmationResolved', view: resolution },
					]);

					if (resolution.requestId === pending.requestId) {
						expect(state.pendingConfirmation).toBeNull();
					} else {
						expect(state.pendingConfirmation).toStrictEqual(pending);
					}

					// The notice is recorded either way — the producer hears how it ended even
					// when the prompt on screen was a different one.
					expect(state.latestConfirmationResolution).toStrictEqual(resolution);
				},
			),
		);
	});

	it('never leaves an answer recorded against a prompt that is gone', () => {
		fc.assert(
			fc.property(
				pendingConfirmationViewArbitrary,
				confirmationResolutionViewArbitrary,
				(pending, resolution) => {
					const state = applyAll([
						{ kind: 'confirmationPending', view: pending },
						{ kind: 'confirmationAnswerSubmitted', requestId: pending.requestId },
						{ kind: 'confirmationResolved', view: resolution },
					]);

					if (state.pendingConfirmation === null) {
						expect(state.confirmationAnswerSubmitted).toBe(false);
					}
				},
			),
		);
	});
});

describe('the single-slot views replace rather than accumulate', () => {
	it('keeps only the last connection status, stream error, and ingest details', () => {
		fc.assert(
			fc.property(
				fc.array(connectionStateViewArbitrary, { minLength: 1, maxLength: 8 }),
				fc.array(streamErrorViewArbitrary, { minLength: 1, maxLength: 8 }),
				fc.array(streamIngestDisplayArbitrary, { minLength: 1, maxLength: 8 }),
				(connections, errors, ingests) => {
					const state = applyAll([
						...connections.map((view) => ({
							kind: 'connectionStatePublished' as const,
							view,
						})),
						...ingests.map((view) => ({ kind: 'streamIngestPublished' as const, view })),
						...errors.map((view) => ({ kind: 'streamErrorPublished' as const, view })),
					]);

					expect(state.connection).toStrictEqual(connections[connections.length - 1]);
					expect(state.streamError).toStrictEqual(errors[errors.length - 1]);
					expect(state.streamIngest).toStrictEqual(ingests[ingests.length - 1]);
				},
			),
		);
	});
});
