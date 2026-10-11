/**
 * Requirement 16.1's gap warning, for a response that is missing part of itself.
 *
 * Both values shown here are the Stream Presenter's conclusions, computed in C++ from the
 * sequence numbers on the deltas — which this side never sees, because one
 * `view:agent_response` carries the whole current response rather than one per delta. So
 * they are displayed and never recomputed: a gap is not a property of any one fragment,
 * and approximating it here would mean the panel and the extension disagreeing about
 * whether the producer is looking at a complete answer.
 *
 * Shared by the streaming display and the finished assistant message because both carry
 * the same two fields: a response can stop with sequences still outstanding, and the
 * warning has to survive the stop rather than disappearing with the streaming view.
 */

import { useTranslation } from 'react-i18next';

export interface ResponseCompletenessNoticeProps {
	/** The presenter's own answer to "is anything still outstanding". */
	textMayBeIncomplete: boolean;

	/** The sequence numbers that never arrived, ascending. Empty when nothing is outstanding. */
	missingSequences: readonly number[];
}

export function ResponseCompletenessNotice({
	textMayBeIncomplete,
	missingSequences,
}: ResponseCompletenessNoticeProps) {
	const { t } = useTranslation();

	if (!textMayBeIncomplete && missingSequences.length === 0) {
		return null;
	}

	return (
		<div className="response-completeness-notice" role="note">
			<p className="response-completeness-notice__warning">
				{t('responseCompleteness.mayBeIncomplete')}
			</p>

			{missingSequences.length > 0 && (
				<p className="response-completeness-notice__sequences">
					{t('responseCompleteness.missingSequences', {
						sequences: missingSequences.join(', '),
					})}
				</p>
			)}
		</div>
	);
}
