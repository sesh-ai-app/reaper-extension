// Substitutes for the three things the Token Manager depends on that the suite cannot
// have: a browser, an authorization server, and the operating system's entropy source.
//
// Same shape as tests/timer_registration_test.cpp's RecordingTimerRegistrar — record what
// was asked for, let the test decide what comes back. Between them these three make the
// whole of the authorization code flow reachable without CEF, without a network, and
// without a clock that moves on its own.
//
// A header rather than a translation unit because the suite's source glob matches *.cpp:
// a shared double in a .cpp would be compiled as its own test file.

#ifndef SESH_AI_TESTS_AUTH_AUTHENTICATION_TEST_DOUBLES_H
#define SESH_AI_TESTS_AUTH_AUTHENTICATION_TEST_DOUBLES_H

#include <auth/secure_random.h>
#include <auth/token_manager.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sesh_ai::tests
{
	// A reproducible byte stream with a long period, so a test can run many sign-ins and still
	// know what it is going to get.
	//
	// Emphatically not a model of the production source — `PlatformSecureRandomSource` goes
	// to the kernel, and tests/auth/platform_secure_random_test.cpp exercises that one
	// directly. This exists so the flow can be driven repeatably, and so that an entropy
	// failure has a test.
	//
	// SplitMix64, which is deterministic from its seed and does not repeat within any number of
	// draws a test will make. A plain incrementing byte would have been simpler to read and
	// would have made "successive sign-ins get different material" pass or fail on the
	// generator's 256-byte cycle rather than on the code under test.
	class DeterministicRandomSource final : public auth::SecureRandomSource
	{
	public:
		explicit DeterministicRandomSource(std::uint64_t seed = 0x5e54a1u)
			: state_{seed}
		{
		}

		bool fill_random_bytes(unsigned char* destination, std::size_t length) override
		{
			++fill_call_count;

			if (fail_next_fill)
			{
				return false;
			}

			for (std::size_t index = 0; index < length; ++index)
			{
				if (index % 8 == 0)
				{
					pending_ = next_value();
				}

				destination[index] = static_cast<unsigned char>(pending_ & 0xffu);
				pending_ >>= 8;
			}

			return true;
		}

		bool fail_next_fill = false;
		int fill_call_count = 0;

	private:
		std::uint64_t next_value()
		{
			state_ += 0x9e3779b97f4a7c15ull;

			std::uint64_t value = state_;

			value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
			value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;

			return value ^ (value >> 31);
		}

		std::uint64_t state_;
		std::uint64_t pending_ = 0;
	};

	// Hands back a byte pattern the test names, cycling if more is asked for than was given.
	//
	// For the one assertion that needs to know the exact bytes the generator consumed: that the
	// code verifier is the encoding of what the entropy source produced, and not of something
	// derived from it.
	class FixedRandomSource final : public auth::SecureRandomSource
	{
	public:
		explicit FixedRandomSource(std::vector<unsigned char> pattern)
			: pattern_{std::move(pattern)}
		{
		}

		bool fill_random_bytes(unsigned char* destination, std::size_t length) override
		{
			if (pattern_.empty())
			{
				return false;
			}

			for (std::size_t index = 0; index < length; ++index)
			{
				destination[index] = pattern_[(offset_ + index) % pattern_.size()];
			}

			offset_ += length;

			return true;
		}

	private:
		std::vector<unsigned char> pattern_;
		std::size_t offset_ = 0;
	};

	// Records every URL the manager asked to present, and every dismissal.
	class RecordingLoginPagePresenter final : public auth::LoginPagePresenter
	{
	public:
		bool present_authorization_page(const std::string& authorization_url) override
		{
			++present_call_count;

			if (refuse_to_present)
			{
				return false;
			}

			presented_urls.push_back(authorization_url);

			return true;
		}

		void dismiss_authorization_page() override
		{
			++dismiss_call_count;
		}

		bool refuse_to_present = false;
		int present_call_count = 0;
		int dismiss_call_count = 0;
		std::vector<std::string> presented_urls;
	};

	// Hands back a scripted sequence of responses and keeps every request it was given, so
	// a test can assert what was sent — including that the verifier sent matches the
	// challenge published.
	class ScriptedTokenEndpoint final : public auth::TokenEndpoint
	{
	public:
		auth::token_endpoint_response exchange(const auth::token_endpoint_request& request) override
		{
			received_requests.push_back(request);

			if (next_response_index_ < scripted_responses.size())
			{
				return scripted_responses[next_response_index_++];
			}

			// Nothing scripted. Reported as an unreachable endpoint rather than as a
			// success, so a test that forgot to script a response fails on the assertion it
			// meant to make instead of passing for the wrong reason.
			auth::token_endpoint_response unreachable;

			unreachable.transport_succeeded = false;

			return unreachable;
		}

		std::vector<auth::token_endpoint_response> scripted_responses;
		std::vector<auth::token_endpoint_request> received_requests;

	private:
		std::size_t next_response_index_ = 0;
	};

	// A clock the test moves by hand. Started well clear of the epoch so that subtracting a
	// lead time never reaches for a time_point before the clock's origin.
	class ManualClock
	{
	public:
		auth::steady_clock::time_point now() const { return now_; }

		void advance(std::chrono::seconds amount) { now_ += amount; }

		auth::clock_reader reader() { return [this] { return now_; }; }

	private:
		auth::steady_clock::time_point now_ =
			auth::steady_clock::time_point{} + std::chrono::hours{24};
	};

	// A configuration that passes validation, with obviously fake values. No real client
	// ID, pool domain, or endpoint appears anywhere in this repository.
	inline auth::cognito_configuration test_configuration()
	{
		auth::cognito_configuration configuration;

		configuration.authorization_endpoint = "https://login.example.invalid/oauth2/authorize";
		configuration.token_endpoint = "https://login.example.invalid/oauth2/token";
		configuration.client_id = "example-app-client-id";
		configuration.redirect_uri = "https://extension.example.invalid/signed-in";
		configuration.scopes = {"openid", "email", "profile"};

		return configuration;
	}

	// Distinctive enough that a leak into a log line or a failure reason is unmistakable.
	inline constexpr char example_access_token[] = "ACCESS.TOKEN.MATERIAL.must.never.be.logged";
	inline constexpr char example_id_token[] = "ID.TOKEN.MATERIAL.must.never.be.logged";
	inline constexpr char example_refresh_token[] = "REFRESH.TOKEN.MATERIAL.must.never.be.logged";
	inline constexpr char example_rotated_access_token[] = "ROTATED.ACCESS.TOKEN.MATERIAL";

	inline auth::token_endpoint_response successful_token_response(
		long long expires_in_seconds = 3600,
		const char* access_token = example_access_token,
		const char* refresh_token = example_refresh_token)
	{
		auth::token_endpoint_response response;

		response.transport_succeeded = true;
		response.http_status_code = 200;
		response.token_type = "Bearer";
		response.expires_in_seconds = expires_in_seconds;
		response.access_token = auth::secret_string{access_token};
		response.id_token = auth::secret_string{example_id_token};

		if (refresh_token != nullptr)
		{
			response.refresh_token = auth::secret_string{refresh_token};
		}

		return response;
	}

	// Drives a manager from `signed_out` to `signed_in`, so the tests about refreshing do
	// not each restate the sign-in.
	inline void sign_in(
		auth::TokenManager& token_manager,
		RecordingLoginPagePresenter& login_page,
		const std::string& redirect_uri = "https://extension.example.invalid/signed-in")
	{
		const auth::operation_outcome started = token_manager.begin_sign_in();

		if (!started.succeeded)
		{
			return;
		}

		// The state nonce is generated inside the manager and is not exposed, which is
		// correct — the only place it is published is the authorization URL. So it is read
		// back out of the URL the presenter was given, which is also what Cognito does.
		const auth::redirect_parameters presented = auth::parse_redirect_url(
			login_page.presented_urls.back(), "https://login.example.invalid/oauth2/authorize");

		token_manager.complete_sign_in(
			redirect_uri + "?code=example-authorization-code&state=" +
			auth::percent_encode_component(presented.state));
	}
}

#endif
