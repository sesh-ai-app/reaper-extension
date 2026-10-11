// The authorization URL and the redirect that comes back.
//
// Two things here are requirements rather than conveniences and are worth reading as such.
//
// The parameter set of the authorization URL is closed (requirement 17.3): nothing in it can
// preselect an identity provider or an authentication method, which is what keeps the managed
// login page offering email/password and social sign-in whether or not the platform
// authenticator is available. The test asserts the set exactly, so adding an eighth parameter
// fails rather than quietly narrowing what the producer is offered.
//
// The redirect parser has to be strict about which navigation ends the flow. The CEF browser
// sees the login page, its assets, whatever a social provider redirects through, and whatever
// the WebAuthn ceremony touches — so treating the wrong navigation as the redirect, or
// accepting a redirect whose state does not match, is the failure mode with security weight.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <auth/authorization_request.h>

#include "authentication_test_doubles.h"

using sesh_ai::auth::authorization_url_parameter_names;
using sesh_ai::auth::build_authorization_url;
using sesh_ai::auth::cognito_configuration;
using sesh_ai::auth::cognito_configuration_from_lookup;
using sesh_ai::auth::configuration_problem;
using sesh_ai::auth::default_scopes;
using sesh_ai::auth::find_configuration_problems;
using sesh_ai::auth::is_usable_configuration;
using sesh_ai::auth::parse_redirect_url;
using sesh_ai::auth::percent_decode_component;
using sesh_ai::auth::percent_encode_component;
using sesh_ai::auth::redirect_parameters;
using sesh_ai::tests::test_configuration;

namespace
{
	// Splits the query string into name/value pairs without using the parser under test, so
	// the assertions about the parameter set are independent of it.
	std::vector<std::pair<std::string, std::string>> query_parameters(const std::string& url)
	{
		std::vector<std::pair<std::string, std::string>> parameters;

		const std::size_t query_start = url.find('?');

		if (query_start == std::string::npos)
		{
			return parameters;
		}

		std::string_view query{url};

		query = query.substr(query_start + 1);

		while (!query.empty())
		{
			const std::size_t separator = query.find('&');
			const std::string_view pair =
				query.substr(0, separator == std::string_view::npos ? query.size() : separator);

			query = separator == std::string_view::npos
				? std::string_view{}
				: query.substr(separator + 1);

			const std::size_t equals = pair.find('=');

			parameters.emplace_back(
				std::string{pair.substr(0, equals)},
				equals == std::string_view::npos ? std::string{} : std::string{pair.substr(equals + 1)});
		}

		return parameters;
	}
}

TEST_CASE("the authorization URL carries exactly the seven closed parameters", "[auth][authorization]")
{
	// Requirement 17.3. `identity_provider`, `idp_identifier`, `prompt`, and `login_hint` are
	// the parameters that would narrow what the managed login page offers; none of them is
	// here, and there is no configuration field that could add one.
	const std::string url = build_authorization_url(test_configuration(), "CHALLENGE", "NONCE");

	const auto parameters = query_parameters(url);
	const auto expected_names = authorization_url_parameter_names();

	REQUIRE(parameters.size() == expected_names.size());

	for (std::size_t index = 0; index < parameters.size(); ++index)
	{
		REQUIRE(parameters[index].first == std::string{expected_names[index]});
	}

	REQUIRE(url.find("identity_provider") == std::string::npos);
	REQUIRE(url.find("idp_identifier") == std::string::npos);
	REQUIRE(url.find("login_hint") == std::string::npos);
	REQUIRE(url.find("prompt") == std::string::npos);
}

TEST_CASE("the authorization URL requests a code with an S256 challenge", "[auth][authorization]")
{
	const cognito_configuration configuration = test_configuration();

	const std::string url = build_authorization_url(configuration, "THE-CHALLENGE", "THE-NONCE");

	std::map<std::string, std::string> parameters;

	for (const auto& parameter : query_parameters(url))
	{
		parameters[parameter.first] = percent_decode_component(parameter.second);
	}

	REQUIRE(url.find(configuration.authorization_endpoint + "?") == 0);
	REQUIRE(parameters["response_type"] == "code");
	REQUIRE(parameters["client_id"] == configuration.client_id);
	REQUIRE(parameters["redirect_uri"] == configuration.redirect_uri);
	REQUIRE(parameters["scope"] == "openid email profile");
	REQUIRE(parameters["state"] == "THE-NONCE");
	REQUIRE(parameters["code_challenge"] == "THE-CHALLENGE");
	REQUIRE(parameters["code_challenge_method"] == "S256");
}

TEST_CASE("the redirect URI and scopes are percent-encoded in the query", "[auth][authorization]")
{
	// A redirect URI carries a colon and slashes, and a scope list carries spaces. Any of them
	// left raw produces a URL Cognito reads differently from what was meant.
	const std::string url = build_authorization_url(test_configuration(), "a+b/c", "d e");

	const auto parameters = query_parameters(url);

	for (const auto& parameter : parameters)
	{
		REQUIRE(parameter.second.find(' ') == std::string::npos);

		if (parameter.first == "redirect_uri")
		{
			REQUIRE(parameter.second.find("://") == std::string::npos);
			REQUIRE(percent_decode_component(parameter.second) ==
				test_configuration().redirect_uri);
		}
	}
}

TEST_CASE("percent encoding round-trips every byte", "[auth][authorization]")
{
	for (int value = 0; value <= 255; ++value)
	{
		// Skipping the null byte: it cannot appear in a URL and is not representable in the
		// std::string round trip the parser performs.
		if (value == 0)
		{
			continue;
		}

		const std::string original(1, static_cast<char>(value));

		REQUIRE(percent_decode_component(percent_encode_component(original)) == original);
	}

	// Everything outside `unreserved` is escaped, including the characters a laxer encoder
	// tends to leave alone.
	REQUIRE(percent_encode_component("a b") == "a%20b");
	REQUIRE(percent_encode_component("+") == "%2B");
	REQUIRE(percent_encode_component("/") == "%2F");
	REQUIRE(percent_encode_component(":") == "%3A");
	REQUIRE(percent_encode_component("Aa0-._~") == "Aa0-._~");
}

TEST_CASE("percent decoding leaves a malformed escape alone", "[auth][authorization]")
{
	// A truncated or non-hexadecimal escape produces a value that fails comparison rather than
	// a value that silently lost characters — which matters for the state nonce, where a
	// mangled value must not accidentally compare equal to anything.
	REQUIRE(percent_decode_component("%2") == "%2");
	REQUIRE(percent_decode_component("%") == "%");
	REQUIRE(percent_decode_component("%zz") == "%zz");
	REQUIRE(percent_decode_component("a+b") == "a b");
	REQUIRE(percent_decode_component("%41%42%43") == "ABC");
}

TEST_CASE("a redirect is recognised only at the configured URI", "[auth][authorization]")
{
	const std::string redirect_uri = "https://extension.example.invalid/signed-in";

	REQUIRE(parse_redirect_url(redirect_uri + "?code=abc", redirect_uri).matches_redirect_uri);
	REQUIRE(parse_redirect_url(redirect_uri, redirect_uri).matches_redirect_uri);

	// A trailing slash is the same destination; everything else is not.
	REQUIRE(parse_redirect_url(redirect_uri + "/?code=abc", redirect_uri).matches_redirect_uri);

	REQUIRE_FALSE(parse_redirect_url(
		"https://login.example.invalid/oauth2/authorize?code=abc", redirect_uri).matches_redirect_uri);
	REQUIRE_FALSE(parse_redirect_url(
		redirect_uri + "-elsewhere?code=abc", redirect_uri).matches_redirect_uri);
	REQUIRE_FALSE(parse_redirect_url(
		"https://extension.example.invalid/signed-in/deeper?code=abc", redirect_uri).matches_redirect_uri);
	REQUIRE_FALSE(parse_redirect_url(
		"http://extension.example.invalid/signed-in?code=abc", redirect_uri).matches_redirect_uri);

	// An unconfigured redirect URI matches nothing, rather than matching everything.
	REQUIRE_FALSE(parse_redirect_url(redirect_uri, "").matches_redirect_uri);
}

TEST_CASE("a redirect's parameters are read and decoded", "[auth][authorization]")
{
	const std::string redirect_uri = "https://extension.example.invalid/signed-in";

	const redirect_parameters parameters = parse_redirect_url(
		redirect_uri + "?code=a%2Fb&state=E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", redirect_uri);

	REQUIRE(parameters.matches_redirect_uri);
	REQUIRE(parameters.authorization_code == "a/b");
	REQUIRE(parameters.state == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
	REQUIRE(parameters.error.empty());
}

TEST_CASE("a redirect carrying an error is read as an error", "[auth][authorization]")
{
	const std::string redirect_uri = "https://extension.example.invalid/signed-in";

	const redirect_parameters parameters = parse_redirect_url(
		redirect_uri + "?error=access_denied&error_description=The%20producer%20cancelled",
		redirect_uri);

	REQUIRE(parameters.matches_redirect_uri);
	REQUIRE(parameters.authorization_code.empty());
	REQUIRE(parameters.error == "access_denied");
	REQUIRE(parameters.error_description == "The producer cancelled");
}

TEST_CASE("a fragment is not read as part of the redirect's query", "[auth][authorization]")
{
	// The authorization code flow returns its parameters in the query. A code in the fragment
	// would be the implicit flow, which this client does not use, and reading one would be
	// accepting a response shape nobody asked for.
	const std::string redirect_uri = "https://extension.example.invalid/signed-in";

	const redirect_parameters parameters =
		parse_redirect_url(redirect_uri + "#code=smuggled&state=smuggled", redirect_uri);

	REQUIRE(parameters.matches_redirect_uri);
	REQUIRE(parameters.authorization_code.empty());
	REQUIRE(parameters.state.empty());
}

TEST_CASE("a duplicated redirect parameter keeps the first occurrence", "[auth][authorization]")
{
	// Two `code` values is either a malformed server or parameter smuggling. Either way the
	// later one is the one to ignore, and the state check then decides the outcome.
	const std::string redirect_uri = "https://extension.example.invalid/signed-in";

	const redirect_parameters parameters = parse_redirect_url(
		redirect_uri + "?code=first&state=alpha&code=second&state=beta", redirect_uri);

	REQUIRE(parameters.authorization_code == "first");
	REQUIRE(parameters.state == "alpha");
}

TEST_CASE("a well-formed configuration is usable and a broken one names every problem",
	"[auth][authorization]")
{
	REQUIRE(is_usable_configuration(test_configuration()));
	REQUIRE(find_configuration_problems(test_configuration()).empty());

	// Everything wrong at once: the caller fixing this should be told about all of it rather
	// than discovering it one sign-in at a time.
	cognito_configuration broken;

	const std::vector<configuration_problem> problems = find_configuration_problems(broken);

	REQUIRE(problems.size() == 5);

	const auto has_problem_with = [&problems](const std::string& field) {
		for (const configuration_problem& problem : problems)
		{
			if (problem.field == field)
			{
				return true;
			}
		}

		return false;
	};

	REQUIRE(has_problem_with("authorization_endpoint"));
	REQUIRE(has_problem_with("token_endpoint"));
	REQUIRE(has_problem_with("client_id"));
	REQUIRE(has_problem_with("redirect_uri"));
	REQUIRE(has_problem_with("scopes"));
}

TEST_CASE("configuration validation insists on TLS", "[auth][authorization]")
{
	// Requirement 26.3 — TLS on every outbound connection. A plaintext authorization or token
	// endpoint would put the code and then the tokens on the wire in the clear.
	cognito_configuration configuration = test_configuration();

	configuration.authorization_endpoint = "http://login.example.invalid/oauth2/authorize";

	REQUIRE_FALSE(is_usable_configuration(configuration));

	configuration = test_configuration();
	configuration.token_endpoint = "http://login.example.invalid/oauth2/token";

	REQUIRE_FALSE(is_usable_configuration(configuration));

	// The redirect URI is the one exception, and only on loopback, where the navigation never
	// leaves the machine. Cognito permits exactly this and nothing broader.
	configuration = test_configuration();
	configuration.redirect_uri = "http://localhost:53682/signed-in";

	REQUIRE(is_usable_configuration(configuration));

	configuration.redirect_uri = "http://127.0.0.1:53682/signed-in";

	REQUIRE(is_usable_configuration(configuration));

	configuration.redirect_uri = "http://producer.example.invalid/signed-in";

	REQUIRE_FALSE(is_usable_configuration(configuration));
}

TEST_CASE("a redirect URI carrying its own query is refused", "[auth][authorization]")
{
	// The authorization response appends `?code=...`, so a redirect URI that already has a
	// query would produce a URL with two of them, and the parser's base comparison would
	// never match.
	cognito_configuration configuration = test_configuration();

	configuration.redirect_uri = "https://extension.example.invalid/signed-in?already=here";

	REQUIRE_FALSE(is_usable_configuration(configuration));

	configuration.redirect_uri = "https://extension.example.invalid/signed-in#fragment";

	REQUIRE_FALSE(is_usable_configuration(configuration));
}

TEST_CASE("configuration is read from a lookup rather than compiled in", "[auth][authorization]")
{
	// No client ID, pool domain, or endpoint appears anywhere in this repository — the values
	// arrive at runtime. The lookup is source-agnostic on purpose: environment, settings file,
	// or a table, decided with the plugin entry point.
	const std::map<std::string, std::string> settings = {
		{"SESH_AI_COGNITO_AUTHORIZATION_ENDPOINT", "https://pool.example.invalid/oauth2/authorize"},
		{"SESH_AI_COGNITO_TOKEN_ENDPOINT", "https://pool.example.invalid/oauth2/token"},
		{"SESH_AI_COGNITO_CLIENT_ID", "client-from-configuration"},
		{"SESH_AI_COGNITO_REDIRECT_URI", "https://extension.example.invalid/signed-in"},
		{"SESH_AI_COGNITO_SCOPES", "openid  email"}
	};

	const cognito_configuration configuration =
		cognito_configuration_from_lookup([&settings](std::string_view key) -> std::optional<std::string> {
			const auto found = settings.find(std::string{key});

			if (found == settings.end())
			{
				return std::nullopt;
			}

			return found->second;
		});

	REQUIRE(configuration.client_id == "client-from-configuration");
	REQUIRE(configuration.authorization_endpoint == "https://pool.example.invalid/oauth2/authorize");
	REQUIRE(configuration.scopes == std::vector<std::string>{"openid", "email"});
	REQUIRE(is_usable_configuration(configuration));
}

TEST_CASE("an absent scope list falls back to the OIDC defaults", "[auth][authorization]")
{
	const cognito_configuration configuration =
		cognito_configuration_from_lookup([](std::string_view) { return std::optional<std::string>{}; });

	REQUIRE(configuration.scopes == default_scopes());
	REQUIRE(configuration.client_id.empty());

	// A lookup that finds nothing produces a configuration that reports what it is missing,
	// rather than a configuration that looks fine and fails at sign-in.
	REQUIRE_FALSE(is_usable_configuration(configuration));

	// And no lookup at all is the same, rather than a crash.
	REQUIRE_FALSE(is_usable_configuration(cognito_configuration_from_lookup({})));
}
