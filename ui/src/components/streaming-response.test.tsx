/**
 * @vitest-environment jsdom
 *
 * Tests for the response being drawn.
 *
 * The one that earns its place is "replaces rather than appends": each publish carries the
 * whole current response, so a component that accumulated would duplicate every fragment,
 * and the symptom — an agent that stutters — does not point at this file.
 *
 * **Validates: Requirements 15.2, 16.1**
 */

import { cleanup, render, screen } from '@testing-library/react';
import {
	afterEach, describe, expect, it,
} from 'vitest';

import type { AgentResponseView } from '../bridge/payloads';
import '../i18n';
import { StreamingResponse } from './streaming-response';

afterEach(cleanup);

function responseView(overrides: Partial<AgentResponseView> = {}): AgentResponseView {
	return {
		active: true,
		conversationId: '01JQCONVERSATION',
		turnId: 'turn-1',
		messageId: 'message-1',
		state: 'streaming',
		text: 'Adding',
		openedImplicitly: false,
		appliedDeltaCount: 1,
		missingSequences: [],
		textMayBeIncomplete: false,
		reconciledWithAssembledText: false,
		...overrides,
	};
}

describe('the streaming response', () => {
	it('draws nothing when there is no response in flight', () => {
		const { container } = render(<StreamingResponse view={null} />);

		expect(container.textContent).toBe('');
	});

	it('renders the published text as markdown', () => {
		const { container } = render(
			<StreamingResponse view={responseView({ text: 'Adding a **drum bus**' })} />,
		);

		expect(container.querySelector('strong')?.textContent).toBe('drum bus');
	});

	it('replaces the text on each publish rather than appending to it', () => {
		const { container, rerender } = render(
			<StreamingResponse view={responseView({ text: 'Adding' })} />,
		);

		rerender(<StreamingResponse view={responseView({ text: 'Adding a drum' })} />);
		rerender(<StreamingResponse view={responseView({ text: 'Adding a drum bus' })} />);

		const rendered = container.textContent ?? '';

		expect(rendered).toContain('Adding a drum bus');
		expect(rendered.match(/Adding/g)).toHaveLength(1);
		expect(rendered).not.toContain('AddingAdding');
	});

	it('says a response is being written before any text has arrived', () => {
		render(<StreamingResponse view={responseView({ text: '', state: 'streaming' })} />);

		expect(screen.getByText(/Writing a response/)).toBeTruthy();
	});

	it('shows the presenter gap warning while sequences are outstanding', () => {
		render(
			<StreamingResponse
				view={responseView({
					text: 'Adding a drum bus',
					textMayBeIncomplete: true,
					missingSequences: [2],
				})}
			/>,
		);

		expect(screen.getByRole('note').textContent).toContain('may be incomplete');
		expect(screen.getByText(/Missing fragments: 2/)).toBeTruthy();
	});
});
