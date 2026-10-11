/**
 * Whether the panel is talking to the session, and what the producer has to do when it is
 * not (requirements 15.2, 23.1, 23.2, 23.3).
 *
 * ---------------------------------------------------------------------------
 * The translation is selected by `state` and `requiredProducerAction`, never by `notice`
 *
 * `ConnectionStateView.notice` is producer-facing English authored in C++, and the schema
 * calls it a fallback for a reason: rendering it as the message would make the extension
 * English-only for exactly the sentences a producer reads when something is wrong, which
 * is the opposite of what requirement 15.3 asks for. So the line shown is a translation
 * keyed on the two fields that classify the situation — the state, and whether the
 * producer has to do something before a reconnect can succeed.
 *
 * `requiredProducerAction` is where the two refusals a producer can act on arrive:
 * `close_the_other_reaper_instance` is requirement 23.2's duplicate-client-type
 * explanation, and `sign_in_again` is requirement 23.3's case after the token refresh and
 * its one retry both failed. Both are stated as what to do rather than as what went wrong,
 * because neither resolves by waiting — the extension deliberately stops retrying for the
 * first, and cannot retry at all for the second.
 *
 * The notice is still rendered, in one case only: a disconnection with no action for the
 * producer to take. `unsupported_client_type` and `authentication_service_unavailable`
 * land there, and for those the C++ side's sentence is the only thing anywhere that says
 * what happened. It is marked `lang="en"` so a screen reader reading a German panel
 * switches voice for it rather than pronouncing English as German — which is the honest
 * presentation of a fallback, and is visibly a fallback rather than a translation that was
 * forgotten.
 *
 * `lastRejection` is not rendered at all. The schema is explicit that it is for diagnosis,
 * and the producer has no use for the difference between `network_or_timeout` and
 * `authentication_service_unavailable` when the extension's own behaviour is identical for
 * both. It goes on the element as a data attribute, where a developer with the panel's
 * inspector open can read it.
 */

import { useTranslation } from 'react-i18next';

import type {
	ConnectionState,
	ConnectionStateView,
	RequiredProducerAction,
} from '../bridge/payloads';

/** The line for each state, when the producer has nothing to do about it. */
const translationKeyForState: Readonly<Record<ConnectionState, string>> = {
	connected: 'connectionStatus.connected',
	reconnecting: 'connectionStatus.reconnecting',
	disconnected: 'connectionStatus.disconnected',
};

/**
 * The line for each action the producer can take. `none` has none, which is what makes the
 * server's English notice worth showing for a disconnection — there is nothing else to say.
 */
const translationKeyForRequiredAction: Readonly<Record<RequiredProducerAction, string | null>> = {
	none: null,
	close_the_other_reaper_instance: 'connectionStatus.closeTheOtherReaperInstance',
	sign_in_again: 'connectionStatus.signInAgain',
};

export interface ConnectionStatusProps {
	/**
	 * `state.connection` — the last `view:connection_state` published, or `null` before the
	 * first one. Null renders nothing: the connection's own state cannot arrive over the
	 * connection, so until the Transport Client has published one there is nothing known,
	 * and "disconnected" would be a guess shown during a perfectly healthy startup.
	 */
	connection: ConnectionStateView | null;
}

export function ConnectionStatus({ connection }: ConnectionStatusProps) {
	const { t } = useTranslation();

	if (connection === null) {
		return null;
	}

	const requiredActionKey = translationKeyForRequiredAction[connection.requiredProducerAction];

	// One case only: the extension has stopped, and there is no action to describe. A
	// reconnection is excluded even though it is also "something wrong", because the
	// translated line already says what is happening and what will happen next — appending
	// an English sentence to it adds nothing but English. See the file comment.
	const showsServerNotice = requiredActionKey === null
		&& connection.state === 'disconnected'
		&& connection.notice.length > 0;

	return (
		<div
			className={`connection-status connection-status--${connection.state}`}
			role="status"
			aria-live="polite"
			data-last-rejection={connection.lastRejection}
		>
			<p className="connection-status__state">{t(translationKeyForState[connection.state])}</p>

			{requiredActionKey !== null && (
				<p className="connection-status__required-action">{t(requiredActionKey)}</p>
			)}

			{showsServerNotice && (
				<p className="connection-status__server-notice" lang="en">{connection.notice}</p>
			)}

			{connection.consecutiveFailedAttempts > 0 && (
				<p className="connection-status__failed-attempts">
					{t('connectionStatus.failedAttempts', {
						attempts: connection.consecutiveFailedAttempts,
					})}
				</p>
			)}
		</div>
	);
}
