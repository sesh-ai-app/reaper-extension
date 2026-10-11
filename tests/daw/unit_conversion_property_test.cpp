// Property tests for the volume and pan conversions — design properties 10 and 11,
// task 6.4, requirements 19.1 and 19.2.
//
// `unit_conversion_test.cpp` holds the worked examples, the named boundary cases,
// the silence floor, the NaN and infinity readings, and the clamps. This file adds
// the formal round trips over a generated corpus, in both directions, and sits on
// top of those rather than replacing them: a round trip that holds everywhere still
// does not say that -150 dB is the floor rather than some other number.
//
// ---------------------------------------------------------------------------
// What the properties are quantified over
//
// "REAPER's stored range" in both property statements is the *representable* range
// — the one bounded by the constants in the header, `minimum_reaper_volume` to
// `maximum_reaper_volume` and `minimum_reaper_pan` to `maximum_reaper_pan`. The
// header says why, and it matters here: outside that range the conversion clamps,
// so a stored gain of 8.0 reports as +12 dB and comes back as roughly 3.98.
// Information is lost because the schema has nowhere to put it. A round trip
// quantified over all doubles would therefore be false, and reading the requirement
// that way would make these tests assert a bug that is not one. Every generated
// value below is inside the representable range by construction, and the coverage
// test checks that it is.
//
// ---------------------------------------------------------------------------
// How the corpus is generated
//
// No new dependency: Catch2 in this build has no generator library, and the repo's
// convention is to enumerate the input space where it is small enough
// (`folder_invariant_keeper_test.cpp`) and to use a deterministic seeded source
// where it is not (`DeterministicBytes` in `alias_store_test.cpp`). A continuous
// double range is not small enough, so this file follows the second convention: a
// fixed-seed xorshift, the same shape as the alias store's, so a failure names a
// value anybody can reproduce from the seed recorded beside it.
//
// Sampling the volume range uniformly would be close to useless. The stored range
// spans eight orders of magnitude and the conversion is logarithmic, so a uniform
// draw over [3.16e-8, 3.98] lands above 0.1 about 97.5% of the time and would
// essentially never visit the quiet end, which is exactly where the logarithm is
// steepest and where a scale-blind tolerance would fail. So each corpus is sampled
// three ways:
//
//   - log-uniform, by drawing a decibel value uniformly and converting, which puts
//     hundreds of samples in every decade of stored gain;
//   - linear-uniform over the stored range, which is what a caller reading faders
//     actually hands over, biased to the loud end as reality is;
//   - clustered against each bound and against unity/centre, including runs of
//     adjacent doubles stepped inward from the endpoints with std::nextafter,
//     because the endpoints are the values the implementation pairs by hand and
//     the doubles either side of them are where that pairing would show a seam.
//
// Requirement 19.1 names both bounds and 19.2 names both bounds and centre, so the
// named values are in the corpus outright and a coverage test asserts they are
// there — a property that skipped them would be quantified over the interior only
// and would miss what the requirement asks about most specifically.
//
// ---------------------------------------------------------------------------
// Tolerances
//
// All four come from the header. No schema states a tolerance — the header records
// that, and records the reasoning behind the numbers it fixes instead. Nothing here
// invents one or relaxes one: if a round trip lands outside these bounds, that is a
// finding about the conversion or about the tolerance, to be reported rather than
// papered over.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <daw/unit_conversion.h>

namespace
{
	using namespace sesh_ai::daw;

	// Seeds, one per corpus so the four sweeps are independent draws rather than
	// the same sequence read twice. Written out here because a failure message
	// quoting an index is only reproducible if the seed that produced it is fixed
	// and visible.
	constexpr std::uint32_t reaper_volume_corpus_seed = 0x5e58c011u;
	constexpr std::uint32_t volume_decibels_corpus_seed = 0x0dbc0de5u;
	constexpr std::uint32_t reaper_pan_corpus_seed = 0x9a71ce17u;
	constexpr std::uint32_t pan_percent_corpus_seed = 0xc0ffee19u;

	// Sample counts. Enough that the log-uniform sweep puts several hundred values
	// in each decade of the eight the stored volume range spans, few enough that
	// the whole file is a few thousand conversions and runs in milliseconds.
	constexpr int log_uniform_sample_count = 4000;
	constexpr int linear_uniform_sample_count = 4000;
	constexpr int cluster_sample_count = 600;

	// How far the adjacent-double runs reach inward from each endpoint. Sixty-four
	// is arbitrary but generous: whatever rounding happens at an endpoint happens
	// within an ulp or two of it, and this covers that with room to spare.
	constexpr int adjacent_double_step_count = 64;

	// Deterministic sample source. Same xorshift as `DeterministicBytes` in
	// alias_store_test.cpp, for the same reason — the corpus has to be identical on
	// every machine, so a counterexample is one anybody can reproduce.
	class DeterministicDoubles
	{
	public:
		explicit DeterministicDoubles(std::uint32_t seed)
			: state_{seed == 0 ? 0x9e3779b9u : seed}
		{
		}

		std::uint32_t next()
		{
			state_ ^= state_ << 13;
			state_ ^= state_ >> 17;
			state_ ^= state_ << 5;

			return state_;
		}

		// A double in [0, 1) carrying a full 53-bit significand, assembled from two
		// draws. One draw would give 32 bits, which quantises the range coarsely
		// enough to keep the sweep off most of the doubles it is meant to visit.
		double unit_interval()
		{
			const std::uint64_t high_bits = next() >> 5;
			const std::uint64_t low_bits = next() >> 6;

			return static_cast<double>((high_bits << 26) + low_bits) / 9007199254740992.0;
		}

		double between(double lowest, double highest)
		{
			return lowest + unit_interval() * (highest - lowest);
		}

		// Signed, in [-1, 1).
		double signed_unit_interval()
		{
			return between(-1.0, 1.0);
		}

	private:
		std::uint32_t state_;
	};

	// Local rather than the header's detail::clamp_to_range: the corpus builders
	// have to stay inside the representable range, and borrowing the
	// implementation's own clamp to establish that would make the corpus agree with
	// the thing under test by construction.
	double clamped_into(double value, double lowest, double highest)
	{
		return std::min(std::max(value, lowest), highest);
	}

	// The `steps` doubles immediately inward from `bound`, walking towards
	// `towards`. These are the values a hand-written endpoint comparison gets wrong
	// if it gets anything wrong.
	void append_adjacent_doubles(std::vector<double>& corpus, double bound, double towards, int steps)
	{
		double value = bound;

		for (int step = 0; step < steps; ++step)
		{
			value = std::nextafter(value, towards);
			corpus.push_back(value);
		}
	}

	std::vector<double> generate_representable_reaper_volumes()
	{
		DeterministicDoubles generator{reaper_volume_corpus_seed};

		// The three values requirement 19.1 and the schema name, outright.
		std::vector<double> corpus{minimum_reaper_volume, unity_reaper_volume, maximum_reaper_volume};

		append_adjacent_doubles(corpus, minimum_reaper_volume, maximum_reaper_volume, adjacent_double_step_count);
		append_adjacent_doubles(corpus, maximum_reaper_volume, minimum_reaper_volume, adjacent_double_step_count);
		append_adjacent_doubles(corpus, unity_reaper_volume, minimum_reaper_volume, adjacent_double_step_count);
		append_adjacent_doubles(corpus, unity_reaper_volume, maximum_reaper_volume, adjacent_double_step_count);

		// Log-uniform over the stored range, by way of a uniform decibel draw.
		for (int sample = 0; sample < log_uniform_sample_count; ++sample)
		{
			const double decibels = generator.between(minimum_volume_decibels, maximum_volume_decibels);
			const double reaper_volume = std::pow(10.0, decibels / decibels_per_amplitude_decade);

			corpus.push_back(clamped_into(reaper_volume, minimum_reaper_volume, maximum_reaper_volume));
		}

		// Linear-uniform over the stored range. Almost all of these land above 0.1,
		// which is the point: it is the distribution a fader read actually has, and
		// the log sweep above is the one that reaches the quiet end.
		for (int sample = 0; sample < linear_uniform_sample_count; ++sample)
		{
			corpus.push_back(generator.between(minimum_reaper_volume, maximum_reaper_volume));
		}

		for (int sample = 0; sample < cluster_sample_count; ++sample)
		{
			// Within a decibel of each bound, and within a thousandth of a decibel
			// of unity.
			const double near_floor = generator.between(minimum_volume_decibels, minimum_volume_decibels + 1.0);
			const double near_ceiling = generator.between(maximum_volume_decibels - 1.0, maximum_volume_decibels);
			const double near_unity_decibels = generator.between(-0.001, 0.001);

			for (const double decibels : {near_floor, near_ceiling, near_unity_decibels})
			{
				const double reaper_volume = std::pow(10.0, decibels / decibels_per_amplitude_decade);

				corpus.push_back(clamped_into(reaper_volume, minimum_reaper_volume, maximum_reaper_volume));
			}

			// Closer in than any decibel offset reaches: a relative hair off each
			// stored bound, and an absolute hair either side of unity.
			const double hair = 1e-9;

			corpus.push_back(clamped_into(minimum_reaper_volume * (1.0 + generator.unit_interval() * hair), minimum_reaper_volume, maximum_reaper_volume));
			corpus.push_back(clamped_into(maximum_reaper_volume * (1.0 - generator.unit_interval() * hair), minimum_reaper_volume, maximum_reaper_volume));
			corpus.push_back(clamped_into(unity_reaper_volume + generator.signed_unit_interval() * hair, minimum_reaper_volume, maximum_reaper_volume));
		}

		return corpus;
	}

	std::vector<double> generate_representable_volume_decibels()
	{
		DeterministicDoubles generator{volume_decibels_corpus_seed};

		std::vector<double> corpus{minimum_volume_decibels, unity_volume_decibels, maximum_volume_decibels};

		append_adjacent_doubles(corpus, minimum_volume_decibels, maximum_volume_decibels, adjacent_double_step_count);
		append_adjacent_doubles(corpus, maximum_volume_decibels, minimum_volume_decibels, adjacent_double_step_count);
		append_adjacent_doubles(corpus, unity_volume_decibels, minimum_volume_decibels, adjacent_double_step_count);
		append_adjacent_doubles(corpus, unity_volume_decibels, maximum_volume_decibels, adjacent_double_step_count);

		// Uniform in decibels, which is log-uniform in stored gain — even coverage
		// of the scale the value is reported on.
		for (int sample = 0; sample < log_uniform_sample_count; ++sample)
		{
			corpus.push_back(generator.between(minimum_volume_decibels, maximum_volume_decibels));
		}

		// Uniform in stored gain, mapped to decibels. Crowds the top two decades of
		// the decibel range, which is where a producer's faders actually sit and
		// where the uniform decibel sweep is thinnest per unit of gain.
		for (int sample = 0; sample < linear_uniform_sample_count; ++sample)
		{
			const double reaper_volume = generator.between(minimum_reaper_volume, maximum_reaper_volume);
			const double decibels = decibels_per_amplitude_decade * std::log10(reaper_volume);

			corpus.push_back(clamped_into(decibels, minimum_volume_decibels, maximum_volume_decibels));
		}

		for (int sample = 0; sample < cluster_sample_count; ++sample)
		{
			const double hair = 1e-9;

			corpus.push_back(generator.between(minimum_volume_decibels, minimum_volume_decibels + 1.0));
			corpus.push_back(generator.between(maximum_volume_decibels - 1.0, maximum_volume_decibels));
			corpus.push_back(minimum_volume_decibels + generator.unit_interval() * hair);
			corpus.push_back(maximum_volume_decibels - generator.unit_interval() * hair);
			corpus.push_back(unity_volume_decibels + generator.signed_unit_interval() * hair);
		}

		return corpus;
	}

	std::vector<double> generate_representable_reaper_pans()
	{
		DeterministicDoubles generator{reaper_pan_corpus_seed};

		// Both bounds and centre outright — the three values requirement 19.2 names.
		std::vector<double> corpus{minimum_reaper_pan, centre_reaper_pan, maximum_reaper_pan};

		append_adjacent_doubles(corpus, minimum_reaper_pan, maximum_reaper_pan, adjacent_double_step_count);
		append_adjacent_doubles(corpus, maximum_reaper_pan, minimum_reaper_pan, adjacent_double_step_count);
		append_adjacent_doubles(corpus, centre_reaper_pan, minimum_reaper_pan, adjacent_double_step_count);
		append_adjacent_doubles(corpus, centre_reaper_pan, maximum_reaper_pan, adjacent_double_step_count);

		// Uniform over the stored range. The mapping is linear, so unlike volume
		// this is even coverage of the scale as reported too.
		for (int sample = 0; sample < linear_uniform_sample_count; ++sample)
		{
			corpus.push_back(generator.between(minimum_reaper_pan, maximum_reaper_pan));
		}

		// Log-uniform in magnitude, both signs. Pan has no logarithm in it, but the
		// interesting neighbourhood is still a multiplicative one: this is what
		// reaches the values a hair off centre, across every decade down to the
		// denormals, rather than stopping wherever a uniform draw happened to stop.
		for (int sample = 0; sample < log_uniform_sample_count; ++sample)
		{
			const double magnitude = std::pow(10.0, -generator.between(0.0, 300.0));
			const double sign = (generator.next() & 1u) == 0u ? -1.0 : 1.0;

			corpus.push_back(clamped_into(sign * magnitude, minimum_reaper_pan, maximum_reaper_pan));
		}

		for (int sample = 0; sample < cluster_sample_count; ++sample)
		{
			const double hair = 1e-9;

			corpus.push_back(generator.between(minimum_reaper_pan, minimum_reaper_pan + 0.01));
			corpus.push_back(generator.between(maximum_reaper_pan - 0.01, maximum_reaper_pan));
			corpus.push_back(clamped_into(minimum_reaper_pan + generator.unit_interval() * hair, minimum_reaper_pan, maximum_reaper_pan));
			corpus.push_back(clamped_into(maximum_reaper_pan - generator.unit_interval() * hair, minimum_reaper_pan, maximum_reaper_pan));
			corpus.push_back(centre_reaper_pan + generator.signed_unit_interval() * hair);
		}

		return corpus;
	}

	std::vector<double> generate_representable_pan_percents()
	{
		DeterministicDoubles generator{pan_percent_corpus_seed};

		std::vector<double> corpus{minimum_pan_percent, centre_pan_percent, maximum_pan_percent};

		append_adjacent_doubles(corpus, minimum_pan_percent, maximum_pan_percent, adjacent_double_step_count);
		append_adjacent_doubles(corpus, maximum_pan_percent, minimum_pan_percent, adjacent_double_step_count);
		append_adjacent_doubles(corpus, centre_pan_percent, minimum_pan_percent, adjacent_double_step_count);
		append_adjacent_doubles(corpus, centre_pan_percent, maximum_pan_percent, adjacent_double_step_count);

		for (int sample = 0; sample < linear_uniform_sample_count; ++sample)
		{
			corpus.push_back(generator.between(minimum_pan_percent, maximum_pan_percent));
		}

		for (int sample = 0; sample < log_uniform_sample_count; ++sample)
		{
			const double magnitude = std::pow(10.0, generator.between(-298.0, 2.0));
			const double sign = (generator.next() & 1u) == 0u ? -1.0 : 1.0;

			corpus.push_back(clamped_into(sign * magnitude, minimum_pan_percent, maximum_pan_percent));
		}

		for (int sample = 0; sample < cluster_sample_count; ++sample)
		{
			const double hair = 1e-9;

			corpus.push_back(generator.between(minimum_pan_percent, minimum_pan_percent + 1.0));
			corpus.push_back(generator.between(maximum_pan_percent - 1.0, maximum_pan_percent));
			corpus.push_back(clamped_into(minimum_pan_percent + generator.unit_interval() * hair, minimum_pan_percent, maximum_pan_percent));
			corpus.push_back(clamped_into(maximum_pan_percent - generator.unit_interval() * hair, minimum_pan_percent, maximum_pan_percent));
			corpus.push_back(centre_pan_percent + generator.signed_unit_interval() * hair);
		}

		return corpus;
	}

	// Built once. The generators are deterministic, so sharing them across test
	// cases costs nothing in reproducibility and keeps each case to the conversions
	// it is actually about.
	const std::vector<double>& representable_reaper_volumes()
	{
		static const std::vector<double> corpus = generate_representable_reaper_volumes();

		return corpus;
	}

	const std::vector<double>& representable_volume_decibels()
	{
		static const std::vector<double> corpus = generate_representable_volume_decibels();

		return corpus;
	}

	const std::vector<double>& representable_reaper_pans()
	{
		static const std::vector<double> corpus = generate_representable_reaper_pans();

		return corpus;
	}

	const std::vector<double>& representable_pan_percents()
	{
		static const std::vector<double> corpus = generate_representable_pan_percents();

		return corpus;
	}

	bool corpus_contains(const std::vector<double>& corpus, double value)
	{
		return std::find(corpus.begin(), corpus.end(), value) != corpus.end();
	}

	bool corpus_lies_within(const std::vector<double>& corpus, double lowest, double highest)
	{
		for (const double value : corpus)
		{
			if (std::isnan(value) || value < lowest || value > highest)
			{
				return false;
			}
		}

		return true;
	}

	// How many corpus values sit below `threshold`. Used to show that the log sweep
	// really did reach the quiet end rather than only claiming to.
	std::size_t corpus_count_below(const std::vector<double>& corpus, double threshold)
	{
		return static_cast<std::size_t>(std::count_if(corpus.begin(), corpus.end(), [threshold](double value) {
			return value < threshold;
		}));
	}

	// The worst round-trip error over a corpus, with the value that produced it, so
	// a failure reports a counterexample rather than only a verdict.
	struct WorstCase
	{
		std::size_t index = 0;
		double input = 0.0;
		double returned = 0.0;
		double error = 0.0;
	};

	template <typename RoundTrip, typename Error>
	WorstCase worst_round_trip(const std::vector<double>& corpus, RoundTrip round_trip, Error error_of)
	{
		WorstCase worst;

		for (std::size_t index = 0; index < corpus.size(); ++index)
		{
			const double input = corpus[index];
			const double returned = round_trip(input);
			const double error = error_of(input, returned);

			if (!(error <= worst.error))
			{
				worst = WorstCase{index, input, returned, error};
			}
		}

		return worst;
	}

	double absolute_error(double input, double returned)
	{
		return std::abs(returned - input);
	}

	// The same quantity Catch2's WithinRel matcher compares, so the relative
	// assertions and the measured worst case are talking about one number.
	double relative_error(double input, double returned)
	{
		const double scale = std::max(std::abs(input), std::abs(returned));

		return scale == 0.0 ? 0.0 : std::abs(returned - input) / scale;
	}

	double reaper_volume_round_trip(double reaper_volume)
	{
		return reaper_volume_from_volume_decibels(volume_decibels_from_reaper_volume(reaper_volume));
	}

	double volume_decibels_round_trip(double volume_decibels)
	{
		return volume_decibels_from_reaper_volume(reaper_volume_from_volume_decibels(volume_decibels));
	}

	double reaper_pan_round_trip(double reaper_pan)
	{
		return reaper_pan_from_pan_percent(pan_percent_from_reaper_pan(reaper_pan));
	}

	double pan_percent_round_trip(double pan_percent)
	{
		return pan_percent_from_reaper_pan(reaper_pan_from_pan_percent(pan_percent));
	}
}

// ---------------------------------------------------------------------------
// The corpus itself, before anything is asserted with it
// ---------------------------------------------------------------------------

TEST_CASE("the generated corpora stay inside the representable range and reach its named values", "[unit_conversion][property][corpus]")
{
	// A property is only as good as what it is quantified over, and both failure
	// modes are silent: a corpus that strayed outside the range would assert a
	// round trip the schema cannot make, and one that missed the endpoints would
	// leave the part of requirement 19 that names them untested while still
	// reporting thousands of passing samples.
	const std::vector<double>& reaper_volumes = representable_reaper_volumes();
	const std::vector<double>& volume_decibels = representable_volume_decibels();
	const std::vector<double>& reaper_pans = representable_reaper_pans();
	const std::vector<double>& pan_percents = representable_pan_percents();

	REQUIRE(corpus_lies_within(reaper_volumes, minimum_reaper_volume, maximum_reaper_volume));
	REQUIRE(corpus_lies_within(volume_decibels, minimum_volume_decibels, maximum_volume_decibels));
	REQUIRE(corpus_lies_within(reaper_pans, minimum_reaper_pan, maximum_reaper_pan));
	REQUIRE(corpus_lies_within(pan_percents, minimum_pan_percent, maximum_pan_percent));

	// Requirement 19.1 names both volume bounds; 19.2 names both pan bounds and
	// centre. Asserted as present rather than assumed.
	REQUIRE(corpus_contains(reaper_volumes, minimum_reaper_volume));
	REQUIRE(corpus_contains(reaper_volumes, unity_reaper_volume));
	REQUIRE(corpus_contains(reaper_volumes, maximum_reaper_volume));

	REQUIRE(corpus_contains(volume_decibels, minimum_volume_decibels));
	REQUIRE(corpus_contains(volume_decibels, unity_volume_decibels));
	REQUIRE(corpus_contains(volume_decibels, maximum_volume_decibels));

	REQUIRE(corpus_contains(reaper_pans, minimum_reaper_pan));
	REQUIRE(corpus_contains(reaper_pans, centre_reaper_pan));
	REQUIRE(corpus_contains(reaper_pans, maximum_reaper_pan));

	REQUIRE(corpus_contains(pan_percents, minimum_pan_percent));
	REQUIRE(corpus_contains(pan_percents, centre_pan_percent));
	REQUIRE(corpus_contains(pan_percents, maximum_pan_percent));

	// And the sweep reached the quiet end. A uniform draw over the stored volume
	// range puts about 2.5% of its samples below 0.1 and essentially none below
	// 1e-3; the log-uniform sweep is what makes these counts what they are, and
	// without it the steepest part of the logarithm would go untested.
	INFO("stored gains below 0.1: " << corpus_count_below(reaper_volumes, 0.1));
	INFO("stored gains below 1e-3: " << corpus_count_below(reaper_volumes, 1e-3));
	INFO("stored gains below 1e-6: " << corpus_count_below(reaper_volumes, 1e-6));

	REQUIRE(corpus_count_below(reaper_volumes, 0.1) > 1000);
	REQUIRE(corpus_count_below(reaper_volumes, 1e-3) > 500);
	REQUIRE(corpus_count_below(reaper_volumes, 1e-6) > 100);

	// Likewise for decibels near the floor, which is the same region seen from the
	// other side.
	REQUIRE(corpus_count_below(volume_decibels, -100.0) > 500);

	// Enough samples to be worth calling a sweep. Not a property, just a guard
	// against a refactor quietly emptying a corpus and leaving every round trip
	// vacuously true.
	REQUIRE(reaper_volumes.size() > 10000);
	REQUIRE(volume_decibels.size() > 10000);
	REQUIRE(reaper_pans.size() > 10000);
	REQUIRE(pan_percents.size() > 10000);
}

// ---------------------------------------------------------------------------
// Property 10: Unit conversion — volume round-trips
//
// For any volume in REAPER's stored range, converting to decibels and back returns
// the original within tolerance, including at both bounds.
//
// **Validates: Requirements 19.1**
// ---------------------------------------------------------------------------

TEST_CASE("Property 10: any representable stored volume survives a trip through decibels", "[unit_conversion][volume][property][round_trip]")
{
	const std::vector<double>& corpus = representable_reaper_volumes();

	for (std::size_t index = 0; index < corpus.size(); ++index)
	{
		const double reaper_volume = corpus[index];
		const double returned = reaper_volume_round_trip(reaper_volume);

		INFO("seed " << reaper_volume_corpus_seed << ", corpus index " << index << ", stored gain " << reaper_volume << ", returned " << returned);

		// Relative, as in the example tests: the range spans eight orders of
		// magnitude and an absolute tolerance would be vacuous at the quiet end
		// and unmeetable at the loud one.
		REQUIRE_THAT(returned, Catch::Matchers::WithinRel(reaper_volume, reaper_volume_relative_tolerance));
	}
}

TEST_CASE("Property 10: any representable decibel value survives a trip through stored volume", "[unit_conversion][volume][property][round_trip]")
{
	// The other direction. Both are needed: a conversion pair that collapsed a
	// neighbourhood of stored gains onto one decibel value would still round-trip
	// that decibel value, so only the stored-first direction catches it, and a pair
	// that lost resolution in decibels shows up only here.
	const std::vector<double>& corpus = representable_volume_decibels();

	for (std::size_t index = 0; index < corpus.size(); ++index)
	{
		const double volume_decibels = corpus[index];
		const double returned = volume_decibels_round_trip(volume_decibels);

		INFO("seed " << volume_decibels_corpus_seed << ", corpus index " << index << ", decibels " << volume_decibels << ", returned " << returned);

		REQUIRE_THAT(returned, Catch::Matchers::WithinAbs(volume_decibels, volume_decibels_tolerance));
	}
}

// ---------------------------------------------------------------------------
// Property 11: Unit conversion — pan round-trips
//
// For any pan in REAPER's stored range, converting to a percentage and back returns
// the original within tolerance, including at both bounds and at centre.
//
// **Validates: Requirements 19.2**
// ---------------------------------------------------------------------------

TEST_CASE("Property 11: any representable stored pan survives a trip through a percentage", "[unit_conversion][pan][property][round_trip]")
{
	const std::vector<double>& corpus = representable_reaper_pans();

	for (std::size_t index = 0; index < corpus.size(); ++index)
	{
		const double reaper_pan = corpus[index];
		const double returned = reaper_pan_round_trip(reaper_pan);

		INFO("seed " << reaper_pan_corpus_seed << ", corpus index " << index << ", stored pan " << reaper_pan << ", returned " << returned);

		// Absolute, because centre is in the range and a relative tolerance has
		// nothing to divide by there.
		REQUIRE_THAT(returned, Catch::Matchers::WithinAbs(reaper_pan, reaper_pan_tolerance));
	}
}

TEST_CASE("Property 11: any representable pan percentage survives a trip through stored pan", "[unit_conversion][pan][property][round_trip]")
{
	const std::vector<double>& corpus = representable_pan_percents();

	for (std::size_t index = 0; index < corpus.size(); ++index)
	{
		const double pan_percent = corpus[index];
		const double returned = pan_percent_round_trip(pan_percent);

		INFO("seed " << pan_percent_corpus_seed << ", corpus index " << index << ", percent " << pan_percent << ", returned " << returned);

		REQUIRE_THAT(returned, Catch::Matchers::WithinAbs(pan_percent, pan_percent_tolerance));
	}
}

// ---------------------------------------------------------------------------
// What the properties measured
// ---------------------------------------------------------------------------

TEST_CASE("the round trips stay well inside the header's tolerances across the whole sweep", "[unit_conversion][property][bounds]")
{
	// The four properties above pass or fail against the header's tolerances. This
	// records how much room they passed by, which is the part that would change
	// silently: a conversion rewritten to be a hundred times less accurate would
	// still satisfy every assertion above, and the tolerances would have stopped
	// meaning what the header says they mean.
	//
	// Measured worst case over this corpus, macOS arm64, Catch2 3.16.0:
	//
	//   stored gain -> dB -> stored gain     3.1e-15 relative  (tolerance 1e-12)
	//     a hair above the floor, 3.16e-08, where the logarithm is steepest
	//   dB -> stored gain -> dB              1.4e-14 absolute  (tolerance 1e-9)
	//     at -82.3 dB, which is one ulp of that value
	//   stored pan -> percent -> stored pan  1.1e-16 absolute  (tolerance 1e-12)
	//     one ulp of 1.0, at the double adjacent to hard left
	//   percent -> stored pan -> percent     7.1e-15 absolute  (tolerance 1e-9)
	//
	// The first three reproduce the figures the header records — 1.4e-14 dB,
	// 3.2e-15 relative in stored gain, 1.1e-16 in stored pan — from a corpus two
	// orders of magnitude larger and sampled differently, so the tolerances were set
	// against the right numbers and there is nothing here to reconcile.
	//
	// The headroom factors asserted below sit an order of magnitude or more clear of
	// those measurements, so another libm's rounding will not trip them, but they
	// are tight enough that losing a decimal digit of accuracy would.
	const WorstCase worst_reaper_volume = worst_round_trip(representable_reaper_volumes(), reaper_volume_round_trip, relative_error);
	const WorstCase worst_volume_decibels = worst_round_trip(representable_volume_decibels(), volume_decibels_round_trip, absolute_error);
	const WorstCase worst_reaper_pan = worst_round_trip(representable_reaper_pans(), reaper_pan_round_trip, absolute_error);
	const WorstCase worst_pan_percent = worst_round_trip(representable_pan_percents(), pan_percent_round_trip, absolute_error);

	INFO("worst stored gain round trip: index " << worst_reaper_volume.index << ", input " << worst_reaper_volume.input << ", returned " << worst_reaper_volume.returned << ", relative error " << worst_reaper_volume.error);
	INFO("worst decibel round trip: index " << worst_volume_decibels.index << ", input " << worst_volume_decibels.input << ", returned " << worst_volume_decibels.returned << ", absolute error " << worst_volume_decibels.error);
	INFO("worst stored pan round trip: index " << worst_reaper_pan.index << ", input " << worst_reaper_pan.input << ", returned " << worst_reaper_pan.returned << ", absolute error " << worst_reaper_pan.error);
	INFO("worst percent round trip: index " << worst_pan_percent.index << ", input " << worst_pan_percent.input << ", returned " << worst_pan_percent.returned << ", absolute error " << worst_pan_percent.error);

	// The properties themselves, restated over the measured worst case.
	REQUIRE(worst_reaper_volume.error <= reaper_volume_relative_tolerance);
	REQUIRE(worst_volume_decibels.error <= volume_decibels_tolerance);
	REQUIRE(worst_reaper_pan.error <= reaper_pan_tolerance);
	REQUIRE(worst_pan_percent.error <= pan_percent_tolerance);

	// And the headroom.
	REQUIRE(worst_reaper_volume.error < reaper_volume_relative_tolerance / 10.0);
	REQUIRE(worst_volume_decibels.error < volume_decibels_tolerance / 1000.0);
	// A hundred rather than a thousand: the pan worst case is one ulp of 1.0, which
	// is the floor for any conversion that touches the value at all, so there is
	// nothing below it to leave room for.
	REQUIRE(worst_reaper_pan.error < reaper_pan_tolerance / 100.0);
	REQUIRE(worst_pan_percent.error < pan_percent_tolerance / 1000.0);
}
