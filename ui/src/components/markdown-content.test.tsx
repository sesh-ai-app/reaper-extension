/**
 * @vitest-environment jsdom
 *
 * Tests for markdown rendering of agent output.
 *
 * The assertions about raw HTML and about URL schemes are the ones worth keeping: they are
 * what fails if `rehype-raw` is ever added, or if the URL transform is relaxed back to the
 * default. The rest is enough markdown to show the parser is wired up.
 *
 * **Validates: Requirements 15.2**
 */

import { cleanup, render, screen } from '@testing-library/react';
import {
	afterEach, describe, expect, it,
} from 'vitest';

import '../i18n';
import { MarkdownContent, restrictiveUrlTransform } from './markdown-content';

afterEach(cleanup);

describe('markdown rendering', () => {
	it('renders headings, emphasis, lists, and code', () => {
		const { container } = render(
			<MarkdownContent markdown={'# Drum bus\n\n**Summed** through a folder.\n\n- kick\n- snare\n\n`CreateTrackSend`'} />,
		);

		expect(screen.getByRole('heading', { level: 1 }).textContent).toBe('Drum bus');
		expect(container.querySelector('strong')?.textContent).toBe('Summed');
		expect(container.querySelectorAll('li')).toHaveLength(2);
		expect(container.querySelector('code')?.textContent).toBe('CreateTrackSend');
	});

	it('renders an unclosed code fence rather than refusing the document', () => {
		// Exactly what the parser is handed mid-stream, on nearly every publish.
		const { container } = render(
			<MarkdownContent markdown={'Here is the script:\n\n```lua\nreaper.Main_OnCommand(40044, 0)'} />,
		);

		expect(container.textContent).toContain('Here is the script:');
		expect(container.querySelector('code')?.textContent).toContain('reaper.Main_OnCommand');
	});

	it('creates no element from raw HTML in the markdown', () => {
		const { container } = render(
			<MarkdownContent markdown={'<img src="x" onerror="alert(1)">\n\n<script>alert(2)</script>\n\n<iframe src="https://example.com"></iframe>'} />,
		);

		expect(container.querySelector('img')).toBeNull();
		expect(container.querySelector('script')).toBeNull();
		expect(container.querySelector('iframe')).toBeNull();

		// Nothing was silently swallowed either: the markup arrived as the characters it is
		// made of and React escaped them on the way to the DOM, so the angle brackets are
		// entities and no attribute of any of the three is live.
		expect(container.innerHTML).not.toContain('<img');
		expect(container.innerHTML).not.toContain('<script');
		expect(container.innerHTML).toContain('&lt;img');
	});

	it('drops the href of a link that is not http, https, or mailto', () => {
		const { container } = render(
			<MarkdownContent markdown="[press me](javascript:alert(1)) and [the notes](./notes.md)" />,
		);

		const anchors = container.querySelectorAll('a');

		expect(anchors).toHaveLength(2);

		anchors.forEach((anchor) => {
			expect(anchor.getAttribute('href')).toBe('');
		});

		// The link text survives, so the response still reads as the agent wrote it.
		expect(container.textContent).toContain('press me');
		expect(container.textContent).toContain('the notes');
	});

	it('keeps an http link and opens it away from the frame', () => {
		const { container } = render(
			<MarkdownContent markdown="[the manual](https://www.reaper.fm/userguide.php)" />,
		);

		const anchor = container.querySelector('a');

		expect(anchor?.getAttribute('href')).toBe('https://www.reaper.fm/userguide.php');
		expect(anchor?.getAttribute('target')).toBe('_blank');
		expect(anchor?.getAttribute('rel')).toContain('noreferrer');
	});

	it('renders an image as its alt text and requests nothing', () => {
		const { container } = render(
			<MarkdownContent markdown="![the kick waveform](https://example.com/kick.png)" />,
		);

		expect(container.querySelector('img')).toBeNull();
		expect(container.textContent).toContain('the kick waveform');
	});
});

describe('the url transform', () => {
	it('permits absolute http, https, and mailto urls', () => {
		expect(restrictiveUrlTransform('https://example.com/a')).toBe('https://example.com/a');
		expect(restrictiveUrlTransform('http://example.com/a')).toBe('http://example.com/a');
		expect(restrictiveUrlTransform('mailto:support@example.com')).toBe('mailto:support@example.com');
	});

	it('drops everything else, including relative urls and schemes the panel has no use for', () => {
		// `no-script-url` exists to stop a script URL being used as one. This is the input to
		// the function whose job is to refuse it, which is the one place the literal has to
		// appear for the refusal to be tested at all.
		// eslint-disable-next-line no-script-url
		expect(restrictiveUrlTransform('javascript:alert(1)')).toBe('');
		expect(restrictiveUrlTransform('data:text/html,<script>alert(1)</script>')).toBe('');
		expect(restrictiveUrlTransform('file:///Users/producer/session.rpp')).toBe('');
		expect(restrictiveUrlTransform('./notes.md')).toBe('');
		expect(restrictiveUrlTransform('/etc/passwd')).toBe('');
		expect(restrictiveUrlTransform('')).toBe('');
	});
});
