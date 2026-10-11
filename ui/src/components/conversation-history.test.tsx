/**
 * @vitest-environment jsdom
 *
 * Tests for the transcript.
 *
 * **Validates: Requirements 15.2, 16.1**
 */

import { cleanup, render, screen } from '@testing-library/react';
import {
	afterEach, describe, expect, it,
} from 'vitest';

import '../i18n';
import type { AssistantMessage, ProducerMessage } from '../state/conversation-reducer';
import { ConversationHistory } from './conversation-history';

afterEach(cleanup);

function producerMessage(overrides: Partial<ProducerMessage> = {}): ProducerMessage {
	return {
		kind: 'producer',
		id: 'producer:1',
		promptText: 'Add a drum bus',
		sentAt: 1_700_000_000_000,
		...overrides,
	};
}

function assistantMessage(overrides: Partial<AssistantMessage> = {}): AssistantMessage {
	return {
		kind: 'assistant',
		id: 'message-1',
		conversationId: '01JQCONVERSATION',
		turnId: 'turn-1',
		text: 'Done — the **drum bus** is in.',
		textMayBeIncomplete: false,
		missingSequences: [],
		appliedDeltaCount: 3,
		reconciledWithAssembledText: true,
		...overrides,
	};
}

describe('the conversation history', () => {
	it('invites a first prompt when there is nothing to show', () => {
		render(<ConversationHistory messages={[]} />);

		expect(screen.queryByRole('list')).toBeNull();
		expect(screen.getByText(/Ask for something/)).toBeTruthy();
	});

	it('keeps producer prompts and responses in the order they happened', () => {
		render(
			<ConversationHistory
				messages={[
					producerMessage({ id: 'producer:1', promptText: 'Add a drum bus' }),
					assistantMessage({ id: 'message-1', text: 'Added it.' }),
					producerMessage({ id: 'producer:2', promptText: 'Name it Drums' }),
				]}
			/>,
		);

		const entries = screen.getAllByRole('listitem');

		expect(entries).toHaveLength(3);
		expect(entries[0]?.textContent).toContain('Add a drum bus');
		expect(entries[1]?.textContent).toContain('Added it.');
		expect(entries[2]?.textContent).toContain('Name it Drums');
	});

	it('shows the producer prompt verbatim rather than as markdown', () => {
		// What was typed is what crossed the bridge, and the transcript has to agree with it.
		const { container } = render(
			<ConversationHistory messages={[producerMessage({ promptText: '**not bold** and `not code`' })]} />,
		);

		expect(container.querySelector('strong')).toBeNull();
		expect(container.querySelector('code')).toBeNull();
		expect(screen.getByText('**not bold** and `not code`')).toBeTruthy();
	});

	it('renders the assistant response as markdown', () => {
		const { container } = render(
			<ConversationHistory messages={[assistantMessage({ text: 'Done — the **drum bus** is in.' })]} />,
		);

		expect(container.querySelector('strong')?.textContent).toBe('drum bus');
	});

	it('carries the presenter gap warning onto a finished response', () => {
		render(
			<ConversationHistory
				messages={[assistantMessage({ textMayBeIncomplete: true, missingSequences: [4, 7] })]}
			/>,
		);

		expect(screen.getByRole('note').textContent).toContain('may be incomplete');
		expect(screen.getByText(/4, 7/)).toBeTruthy();
	});

	it('shows no warning for a response that arrived whole', () => {
		render(<ConversationHistory messages={[assistantMessage()]} />);

		expect(screen.queryByRole('note')).toBeNull();
	});

	it('lists two responses that named no identifier rather than collapsing them', () => {
		// The reducer appends an unidentified response rather than matching it against an
		// existing one, so the list has to be able to hold more than one of them.
		render(
			<ConversationHistory
				messages={[
					assistantMessage({ id: '', text: 'First answer.' }),
					assistantMessage({ id: '', text: 'Second answer.' }),
				]}
			/>,
		);

		const entries = screen.getAllByRole('listitem');

		expect(entries).toHaveLength(2);
		expect(entries[0]?.textContent).toContain('First answer.');
		expect(entries[1]?.textContent).toContain('Second answer.');
	});
});
