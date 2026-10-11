// The two ends of the browser leg of the authorization code flow: the URL the managed
// login page is opened at, and the redirect that comes back (requirements 17.1, 17.3).
//
// Both halves are string work over public values, so both live here rather than in the
// token manager — which leaves the manager to the part that holds secrets and tracks
// time, and leaves this to the part a test can state exactly.
//
// ---------------------------------------------------------------------------
// Nothing in the authorization request narrows how the producer signs in
//
// Requirement 17.3 says the managed login page always offers email/password and social
// sign-in as fallbacks, *regardless of whether the platform authenticator is available*.
// Cognito honours that by rendering every method the user pool has configured — unless
// the authorization request tells it not to. `identity_provider` and `idp_identifier`
// jump straight to one provider; `prompt` and `login_hint` change what the page offers.
//
// So the request built here carries exactly seven parameters and no others:
//
//   response_type  client_id  redirect_uri  scope  state  code_challenge
//   code_challenge_method
//
// There is no configuration field that could add an eighth, and
// `authorization_url_parameter_names` states the set so the suite asserts it rather than
// trusting it. That is the whole of requirement 17.3 on this side: the fallbacks are
// available because nothing here can suppress them.
//
// Passkey-first (requirement 17.2) is not a URL parameter either. It is the user pool's
// authentication-method configuration plus Chromium's own WebAuthn support inside CEF,
// and design.md carries it as an open item — whether the platform authenticator path
// works on all three of macOS Touch ID, Windows Hello, and Linux FIDO2 needs validating
// before passkey is treated as the only route. Nothing in this component assumes it
// succeeded: the flow is identical whichever method the producer picks, because all of
// them end in the same redirect carrying the same authorization code.
//
// ---------------------------------------------------------------------------
// No client secret, anywhere
//
// `cognito_configuration` has no field for one, and the token exchange has no parameter
// for one. The extension is a public client — it ships as a binary from a public
// repository, so a secret compiled into it is a secret published (requirement 26.5).
// PKCE is what replaces it. A Cognito app client for this extension must be created
// without a secret; with one, the token exchange fails on a missing `client_secret`
// rather than silently doing something weaker, which is the direction worth failing in.

#ifndef SESH_AI_AUTH_AUTHORIZATION_REQUEST_H
#define SESH_AI_AUTH_AUTHORIZATION_REQUEST_H

#include <auth/pkce.h>

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sesh_ai::auth
{
	// Where the user pool lives and which app client this is. All of it is
	// deployment-specific and none of it is secret, but none of it is hard-coded either:
	// a client ID or pool domain in the source would be a production identifier in a
	// public repository, and the development, staging, and production pools differ.
	//
	// The values arrive through `cognito_configuration_from_lookup` below.
	struct cognito_configuration
	{
		// `https://<domain>/oauth2/authorize`
		std::string authorization_endpoint;

		// `https://<domain>/oauth2/token`
		std::string token_endpoint;

		std::string client_id;

		// Where Cognito sends the producer back to. Watched by the CEF browser; the
		// navigation is intercepted rather than followed, so nothing needs to be
		// listening at the other end.
		std::string redirect_uri;

		std::vector<std::string> scopes;
	};

	struct configuration_problem
	{
		std::string field;
		std::string reason;
	};

	namespace detail
	{
		inline bool has_prefix(std::string_view value, std::string_view prefix)
		{
			return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
		}

		inline bool contains_whitespace(std::string_view value)
		{
			for (const char character : value)
			{
				if (character == ' ' || character == '\t' || character == '\r' || character == '\n')
				{
					return true;
				}
			}

			return false;
		}

		// TLS on every outbound connection (requirement 26.3). The one exception is a
		// loopback redirect URI, which never leaves the machine and which Cognito itself
		// permits over http for exactly that reason.
		inline bool is_acceptable_redirect_uri_scheme(std::string_view redirect_uri)
		{
			return has_prefix(redirect_uri, "https://") ||
				has_prefix(redirect_uri, "http://localhost") ||
				has_prefix(redirect_uri, "http://127.0.0.1");
		}
	}

	// Everything wrong with a configuration, in one pass. A caller that has to fix three
	// fields should be told about three fields, not discover them one sign-in attempt at
	// a time.
	inline std::vector<configuration_problem> find_configuration_problems(
		const cognito_configuration& configuration)
	{
		std::vector<configuration_problem> problems;

		if (configuration.authorization_endpoint.empty())
		{
			problems.push_back({"authorization_endpoint", "is not configured"});
		}
		else if (!detail::has_prefix(configuration.authorization_endpoint, "https://"))
		{
			problems.push_back({"authorization_endpoint", "must be an https URL"});
		}

		if (configuration.token_endpoint.empty())
		{
			problems.push_back({"token_endpoint", "is not configured"});
		}
		else if (!detail::has_prefix(configuration.token_endpoint, "https://"))
		{
			problems.push_back({"token_endpoint", "must be an https URL"});
		}

		if (configuration.client_id.empty())
		{
			problems.push_back({"client_id", "is not configured"});
		}
		else if (detail::contains_whitespace(configuration.client_id))
		{
			problems.push_back({"client_id", "must not contain whitespace"});
		}

		if (configuration.redirect_uri.empty())
		{
			problems.push_back({"redirect_uri", "is not configured"});
		}
		else if (!detail::is_acceptable_redirect_uri_scheme(configuration.redirect_uri))
		{
			problems.push_back({"redirect_uri", "must be an https URL, or http on loopback"});
		}
		else if (configuration.redirect_uri.find('?') != std::string::npos ||
			configuration.redirect_uri.find('#') != std::string::npos)
		{
			problems.push_back({"redirect_uri", "must carry no query string and no fragment"});
		}

		if (configuration.scopes.empty())
		{
			problems.push_back({"scopes", "must name at least one scope"});
		}
		else
		{
			for (const std::string& scope : configuration.scopes)
			{
				if (scope.empty() || detail::contains_whitespace(scope))
				{
					problems.push_back({"scopes", "each scope must be non-empty and contain no whitespace"});
					break;
				}
			}
		}

		return problems;
	}

	inline bool is_usable_configuration(const cognito_configuration& configuration)
	{
		return find_configuration_problems(configuration).empty();
	}

	// The documented configuration keys. Named here so that whatever ends up supplying
	// them — environment variables, an installed settings file, a build-time table — uses
	// the same names, and so a missing key is reported against a name a producer can look
	// up rather than against a field of a struct.
	inline constexpr char configuration_key_authorization_endpoint[] =
		"SESH_AI_COGNITO_AUTHORIZATION_ENDPOINT";
	inline constexpr char configuration_key_token_endpoint[] = "SESH_AI_COGNITO_TOKEN_ENDPOINT";
	inline constexpr char configuration_key_client_id[] = "SESH_AI_COGNITO_CLIENT_ID";
	inline constexpr char configuration_key_redirect_uri[] = "SESH_AI_COGNITO_REDIRECT_URI";
	inline constexpr char configuration_key_scopes[] = "SESH_AI_COGNITO_SCOPES";

	// The scopes used when the configuration does not name any. `openid` is what makes
	// this an OIDC request at all and is what produces the ID token; `email` and `profile`
	// carry the producer identity the session is attributed to.
	inline std::vector<std::string> default_scopes()
	{
		return {"openid", "email", "profile"};
	}

	// Reads a value for a key, or nothing when the key is absent. Deliberately
	// source-agnostic: `std::getenv` satisfies it, so does a parsed settings file, so does
	// a test's map. Which of those the shipped extension uses is settled with the plugin
	// entry point (task 17.1); this component only needs to not care.
	using configuration_lookup = std::function<std::optional<std::string>(std::string_view key)>;

	// Builds a configuration from a lookup. Scopes are space-separated, matching how
	// OAuth itself writes them.
	//
	// No validation here — the result goes through `find_configuration_problems`, so a
	// half-configured deployment produces a list of what is missing rather than an
	// exception from the middle of a read.
	inline cognito_configuration cognito_configuration_from_lookup(const configuration_lookup& lookup)
	{
		cognito_configuration configuration;

		if (!lookup)
		{
			return configuration;
		}

		const auto read = [&lookup](std::string_view key) -> std::string {
			const std::optional<std::string> value = lookup(key);

			return value.has_value() ? *value : std::string{};
		};

		configuration.authorization_endpoint = read(configuration_key_authorization_endpoint);
		configuration.token_endpoint = read(configuration_key_token_endpoint);
		configuration.client_id = read(configuration_key_client_id);
		configuration.redirect_uri = read(configuration_key_redirect_uri);

		const std::string scopes = read(configuration_key_scopes);

		if (scopes.empty())
		{
			configuration.scopes = default_scopes();
		}
		else
		{
			std::size_t position = 0;

			while (position < scopes.size())
			{
				const std::size_t separator = scopes.find(' ', position);
				const std::size_t end = separator == std::string::npos ? scopes.size() : separator;
				const std::string scope = scopes.substr(position, end - position);

				if (!scope.empty())
				{
					configuration.scopes.push_back(scope);
				}

				position = end + 1;
			}
		}

		return configuration;
	}

	// Percent-encodes everything outside RFC 3986's `unreserved` set.
	//
	// Conservative on purpose: `+`, `:`, and `/` are all encoded, which is legal in a
	// query value and avoids having to reason about which sub-delimiter each parameter can
	// tolerate. A redirect URI therefore appears in the query fully encoded, which is what
	// Cognito expects.
	inline std::string percent_encode_component(std::string_view value)
	{
		static constexpr char hexadecimal_digits[] = "0123456789ABCDEF";

		std::string encoded;

		encoded.reserve(value.size());

		for (const char character : value)
		{
			const bool is_alphabetic =
				(character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
			const bool is_digit = character >= '0' && character <= '9';
			const bool is_other_unreserved =
				character == '-' || character == '.' || character == '_' || character == '~';

			if (is_alphabetic || is_digit || is_other_unreserved)
			{
				encoded.push_back(character);
			}
			else
			{
				const unsigned char byte = static_cast<unsigned char>(character);

				encoded.push_back('%');
				encoded.push_back(hexadecimal_digits[(byte >> 4) & 0x0fu]);
				encoded.push_back(hexadecimal_digits[byte & 0x0fu]);
			}
		}

		return encoded;
	}

	// The reverse, for reading the redirect's query string. `+` decodes to a space, which
	// is the query-string convention; an incomplete or non-hexadecimal escape is left
	// alone rather than dropped, so a malformed redirect produces a value that fails
	// comparison instead of a value that silently lost characters.
	inline std::string percent_decode_component(std::string_view value)
	{
		const auto hexadecimal_value = [](char character) -> int {
			if (character >= '0' && character <= '9')
			{
				return character - '0';
			}

			if (character >= 'a' && character <= 'f')
			{
				return (character - 'a') + 10;
			}

			if (character >= 'A' && character <= 'F')
			{
				return (character - 'A') + 10;
			}

			return -1;
		};

		std::string decoded;

		decoded.reserve(value.size());

		for (std::size_t index = 0; index < value.size(); ++index)
		{
			if (value[index] == '+')
			{
				decoded.push_back(' ');

				continue;
			}

			if (value[index] == '%' && index + 2 < value.size())
			{
				const int high = hexadecimal_value(value[index + 1]);
				const int low = hexadecimal_value(value[index + 2]);

				if (high >= 0 && low >= 0)
				{
					decoded.push_back(static_cast<char>((high << 4) | low));
					index += 2;

					continue;
				}
			}

			decoded.push_back(value[index]);
		}

		return decoded;
	}

	// Exactly the parameters the authorization URL carries, in the order it carries them.
	// See the header comment for why the set is closed.
	inline std::array<const char*, 7> authorization_url_parameter_names()
	{
		return {
			"response_type",
			"client_id",
			"redirect_uri",
			"scope",
			"state",
			"code_challenge",
			"code_challenge_method"
		};
	}

	// The URL the CEF browser is pointed at.
	//
	// `code_challenge` is the S256 challenge, never the verifier — the two are different
	// types precisely so that passing the wrong one does not compile.
	inline std::string build_authorization_url(
		const cognito_configuration& configuration,
		std::string_view code_challenge,
		std::string_view state_nonce)
	{
		std::string scope;

		for (const std::string& single_scope : configuration.scopes)
		{
			if (!scope.empty())
			{
				scope.push_back(' ');
			}

			scope += single_scope;
		}

		std::string url = configuration.authorization_endpoint;

		url += "?response_type=code";
		url += "&client_id=" + percent_encode_component(configuration.client_id);
		url += "&redirect_uri=" + percent_encode_component(configuration.redirect_uri);
		url += "&scope=" + percent_encode_component(scope);
		url += "&state=" + percent_encode_component(state_nonce);
		url += "&code_challenge=" + percent_encode_component(code_challenge);
		url += "&code_challenge_method=" + percent_encode_component(pkce_code_challenge_method);

		return url;
	}

	// What came back on the redirect.
	//
	// `matches_redirect_uri` is false for any navigation that is not the configured
	// redirect — which is most of them, since the browser is also loading the login page,
	// its assets, a social provider's pages, and whatever the WebAuthn ceremony touches.
	// The caller uses it to decide whether this navigation is the one that ends the flow.
	struct redirect_parameters
	{
		bool matches_redirect_uri = false;
		std::string authorization_code;
		std::string state;
		std::string error;
		std::string error_description;
	};

	inline redirect_parameters parse_redirect_url(
		std::string_view redirect_url,
		std::string_view expected_redirect_uri)
	{
		redirect_parameters parameters;

		// A fragment is not part of the query and never carries the authorization code in
		// this flow, so it is cut before anything else is read.
		const std::size_t fragment_start = redirect_url.find('#');
		const std::string_view without_fragment = redirect_url.substr(
			0, fragment_start == std::string_view::npos ? redirect_url.size() : fragment_start);

		const std::size_t query_start = without_fragment.find('?');
		const std::string_view base = without_fragment.substr(
			0, query_start == std::string_view::npos ? without_fragment.size() : query_start);

		// A trailing slash is the one difference worth tolerating — browsers add one to a
		// bare origin, and a configured `https://example.com/callback` is the same
		// destination as `https://example.com/callback/`.
		const auto without_trailing_slash = [](std::string_view value) -> std::string_view {
			if (!value.empty() && value.back() == '/')
			{
				return value.substr(0, value.size() - 1);
			}

			return value;
		};

		parameters.matches_redirect_uri =
			!expected_redirect_uri.empty() &&
			without_trailing_slash(base) == without_trailing_slash(expected_redirect_uri);

		if (query_start == std::string_view::npos)
		{
			return parameters;
		}

		std::string_view query = without_fragment.substr(query_start + 1);

		while (!query.empty())
		{
			const std::size_t separator = query.find('&');
			const std::string_view pair = query.substr(
				0, separator == std::string_view::npos ? query.size() : separator);

			query = separator == std::string_view::npos
				? std::string_view{}
				: query.substr(separator + 1);

			if (pair.empty())
			{
				continue;
			}

			const std::size_t equals = pair.find('=');
			const std::string_view raw_name = pair.substr(
				0, equals == std::string_view::npos ? pair.size() : equals);
			const std::string_view raw_value = equals == std::string_view::npos
				? std::string_view{}
				: pair.substr(equals + 1);

			const std::string name = percent_decode_component(raw_name);
			std::string value = percent_decode_component(raw_value);

			// First occurrence wins. A redirect carrying `code` twice is either a
			// malformed server or an attempt at parameter smuggling, and in both cases
			// the later value is the one to ignore.
			if (name == "code" && parameters.authorization_code.empty())
			{
				parameters.authorization_code = std::move(value);
			}
			else if (name == "state" && parameters.state.empty())
			{
				parameters.state = std::move(value);
			}
			else if (name == "error" && parameters.error.empty())
			{
				parameters.error = std::move(value);
			}
			else if (name == "error_description" && parameters.error_description.empty())
			{
				parameters.error_description = std::move(value);
			}
		}

		return parameters;
	}
}

#endif
