/**
 * @vitest-environment jsdom
 *
 * Tests for the prompt box.
 *
 * The assertion that matters most is the verbatim one: `promptText` is what the standalone
 * Bedrock Guardrail's `dataPath` resolves to, so anything this component added or trimmed
 * would change what the guardrail reads.
 *
 * **Validates: Requirements 15.2**
 */

import {
	cleanup, fireEvent, render, screen,
} from '@testing-library/react';
import {
	afterEach, describe, expect, it,
} from 'vitest';

import { maximumPromptTextLength, type BridgeSendOutcome } from '../bridge/send';
import '../i18n';
import { ProducerPromptInput } from './producer-prompt-input';

afterEach(cleanup);

interface Harness {
	sentPrompts: string[];
	textInput: HTMLTextAreaElement;
	sendButton: HTMLButtonElement;
}

function renderInput(outcome: BridgeSendOutcome = { messageName: 'state:prompt', disposition: 'sent' }): Harness {
	const sentPrompts: string[] = [];

	render(
		<ProducerPromptInput
			sendPrompt={(promptText: string) => {
				sentPrompts.push(promptText);

				return outcome;
			}}
		/>,
	);

	return {
		sentPrompts,
		textInput: screen.getByLabelText(/Ask Sesh AI/),
		sendButton: screen.getByRole('button', { name: 'Send' }),
	};
}

describe('the prompt box', () => {
	it('sends exactly what was typed, untrimmed and unframed', () => {
		const harness = renderInput();
		const typed = '  Add a drum bus\n\nand name it Drums  ';

		fireEvent.change(harness.textInput, { target: { value: typed } });
		fireEvent.click(harness.sendButton);

		expect(harness.sentPrompts).toEqual([typed]);
	});

	it('clears the box once the bridge accepted the prompt', () => {
		const harness = renderInput();

		fireEvent.change(harness.textInput, { target: { value: 'Add a drum bus' } });
		fireEvent.click(harness.sendButton);

		expect(harness.textInput.value).toBe('');
	});

	it('keeps the text and explains the refusal when the send did not go', () => {
		const harness = renderInput({
			messageName: 'state:prompt',
			disposition: 'transport_unavailable',
			reason: 'seshAiBridgeSend is not installed on this page',
		});

		fireEvent.change(harness.textInput, { target: { value: 'Add a drum bus' } });
		fireEvent.click(harness.sendButton);

		expect(harness.textInput.value).toBe('Add a drum bus');
		expect(screen.getByRole('alert').textContent).toContain('could not reach the extension');
	});

	it('drops the refusal notice as soon as the text is edited', () => {
		const harness = renderInput({ messageName: 'state:prompt', disposition: 'transport_threw' });

		fireEvent.change(harness.textInput, { target: { value: 'Add a drum bus' } });
		fireEvent.click(harness.sendButton);

		expect(screen.getByRole('alert')).toBeTruthy();

		fireEvent.change(harness.textInput, { target: { value: 'Add a drum bus please' } });

		expect(screen.queryByRole('alert')).toBeNull();
	});

	it('will not send whitespace', () => {
		const harness = renderInput();

		expect(harness.sendButton.disabled).toBe(true);

		fireEvent.change(harness.textInput, { target: { value: '   \n  ' } });

		expect(harness.sendButton.disabled).toBe(true);

		fireEvent.click(harness.sendButton);

		expect(harness.sentPrompts).toEqual([]);
	});

	it('sends on Enter and inserts a newline on Shift+Enter', () => {
		const harness = renderInput();

		fireEvent.change(harness.textInput, { target: { value: 'Add a drum bus' } });
		fireEvent.keyDown(harness.textInput, { key: 'Enter', shiftKey: true });

		expect(harness.sentPrompts).toEqual([]);

		fireEvent.keyDown(harness.textInput, { key: 'Enter' });

		expect(harness.sentPrompts).toEqual(['Add a drum bus']);
	});

	it('refuses a prompt the schema would refuse, counting code points', () => {
		const harness = renderInput();

		// One code point past the bound, written as a character outside the basic multilingual
		// plane so that counting UTF-16 units would get a different — and wrong — answer.
		const tooLong = '🎛'.repeat(maximumPromptTextLength + 1);

		fireEvent.change(harness.textInput, { target: { value: tooLong } });

		expect(harness.sendButton.disabled).toBe(true);
		expect(screen.getByText(/characters remaining/)).toBeTruthy();

		fireEvent.click(harness.sendButton);

		expect(harness.sentPrompts).toEqual([]);
	});

	it('sends a prompt that is exactly as long as the schema allows', () => {
		const harness = renderInput();
		const atTheBound = '🎛'.repeat(maximumPromptTextLength);

		fireEvent.change(harness.textInput, { target: { value: atTheBound } });

		expect(harness.sendButton.disabled).toBe(false);

		fireEvent.click(harness.sendButton);

		expect(harness.sentPrompts).toEqual([atTheBound]);
	});
});
