/**
 * Markdown rendering for text the agent wrote.
 *
 * This is the one component in `ui/` whose options are a security decision rather than a
 * style one. The text is model output: it is shaped by a prompt the producer typed and by
 * tool results the extension sent, and neither of those is a sanitiser. Requirement 15.2
 * asks for markdown; it does not ask for an HTML renderer, and the difference is the whole
 * of this file's configuration.
 *
 * **Raw HTML is never parsed into elements.** `rehype-raw` is deliberately absent, so an
 * `html` node in the markdown reaches React as the characters it is made of and React
 * escapes them on the way to the DOM. Nothing here uses `dangerouslySetInnerHTML`.
 * `markdown-content.test.tsx` asserts that an `<img src=x onerror=...>` payload creates no
 * element, which is the test that fails if `rehype-raw` is ever added for a feature that
 * seems to need it — and it is the only guard there can be, since the plugin would run
 * before `skipHtml` could drop anything.
 *
 * **Only `http`, `https`, and `mailto` URLs survive.** `defaultUrlTransform` already drops
 * `javascript:`, and this narrows it further: it also drops relative URLs, which matter
 * here because the panel is loaded from `file://` (requirement 15.1) and a relative href
 * resolves to somewhere on the producer's disk. An unusable URL is dropped rather than
 * rewritten, so the link text still reads as the agent wrote it. Links that survive open
 * away from the frame, because navigating the frame itself would replace the UI with
 * whatever was linked and there is no way back to a `file://` application from there.
 *
 * **Images render as their alt text.** A remote `<img>` on a `file://` page is an outbound
 * request to whoever wrote the URL — a tracking vector, and not a feature anything in this
 * spec asked for.
 */

import Markdown, { defaultUrlTransform, type Components } from 'react-markdown';

/** The three URL schemes a link in an agent response may use. */
const permittedUrlSchemes = ['http:', 'https:', 'mailto:'];

/**
 * Drops every URL that is not an absolute `http`, `https`, or `mailto` one. Answers an
 * empty string for the rest, which is how react-markdown is told to render the element
 * without the attribute.
 */
export function restrictiveUrlTransform(url: string): string {
	const defaulted = defaultUrlTransform(url);

	if (defaulted.length === 0) {
		return '';
	}

	// A relative URL has no scheme, so parsing it without a base throws. That is the answer
	// this needs rather than a case to handle: the panel has no origin worth resolving
	// against, so there is nothing a relative URL could usefully point at.
	let parsed: URL;

	try {
		parsed = new URL(defaulted);
	} catch {
		return '';
	}

	return permittedUrlSchemes.includes(parsed.protocol) ? defaulted : '';
}

const markdownComponents: Components = {
	a: function MarkdownAnchor(anchorProperties) {
		const { href, children } = anchorProperties;

		return (
			<a href={href} target="_blank" rel="noreferrer noopener">
				{children}
			</a>
		);
	},

	img: function MarkdownImage(imageProperties) {
		const { alt } = imageProperties;

		return <span className="markdown-content__image-alternative-text">{alt}</span>;
	},
};

export interface MarkdownContentProps {
	/**
	 * The markdown source. Frequently mid-syntax while a response is streaming — an
	 * unclosed fence, half a table — which the parser handles by rendering what is there
	 * so far rather than refusing the document.
	 */
	markdown: string;
}

export function MarkdownContent({ markdown }: MarkdownContentProps) {
	return (
		<div className="markdown-content">
			<Markdown
				components={markdownComponents}
				urlTransform={restrictiveUrlTransform}
			>
				{markdown}
			</Markdown>
		</div>
	);
}
