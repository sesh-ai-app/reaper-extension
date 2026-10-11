// Unit tests for the volume and pan conversions (requirements 19.1 and 19.2).
//
// These are the worked examples and the named boundary cases. The formal
// round-trip properties over the whole stored range are task 6.4 — design
// properties 10 and 11 — and sit on top of these rather than replacing them: a
// property that holds everywhere still does not tell you that -150 dB is silence
// rather than some other floor someone picked.
//
// Every tolerance here comes from the header rather than being written inline, so
// the conversion and its tests cannot disagree about what "within tolerance" means.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <vector>

#include <daw/unit_conversion.h>

namespace
{
	using namespace sesh_ai::daw;

	// A spread of stored gains inside the representable range, including both
	// bounds and unity. Deliberately includes values a decade apart at the quiet
	// end, where the logarithm is steepest and a scale-blind tolerance would be
	// the easiest mistake to make.
	const std::vector<double> representable_reaper_volumes{
		minimum_reaper_volume,
		1e-7,
		1e-6,
		1e-5,
		1e-4,
		1e-3,
		0.01,
		0.125,
		0.5,
		0.7071067811865476,
		unity_reaper_volume,
		1.5,
		2.0,
		3.0,
		maximum_reaper_volume,
	};

	const std::vector<double> representable_volume_decibels{
		minimum_volume_decibels,
		-120.0,
		-96.0,
		-60.0,
		-24.0,
		-6.0,
		-3.0,
		-0.5,
		unity_volume_decibels,
		0.5,
		3.0,
		6.0,
		maximum_volume_decibels,
	};

	const std::vector<double> representable_reaper_pans{
		minimum_reaper_pan,
		-0.9,
		-0.5,
		-0.3,
		-0.001,
		centre_reaper_pan,
		0.001,
		0.3,
		0.5,
		0.9,
		maximum_reaper_pan,
	};

	const std::vector<double> representable_pan_percents{
		minimum_pan_percent,
		-75.0,
		-30.0,
		-0.5,
		centre_pan_percent,
		0.5,
		30.0,
		75.0,
		maximum_pan_percent,
	};
}

// ---------------------------------------------------------------------------
// Volume — the values a producer would recognise
// ---------------------------------------------------------------------------

TEST_CASE("unity gain is zero decibels in both directions", "[unit_conversion][volume]")
{
	REQUIRE(volume_decibels_from_reaper_volume(unity_reaper_volume) == unity_volume_decibels);
	REQUIRE(reaper_volume_from_volume_decibels(unity_volume_decibels) == unity_reaper_volume);
}

TEST_CASE("known decibel values convert to the expected gain", "[unit_conversion][volume]")
{
	// Halving amplitude is -6.0206 dB; -6 dB is the value producers quote and 0.5012
	// is what it actually is. Both directions are checked against the arithmetic
	// definition rather than against each other, so a sign error or a factor of ten
	// in place of twenty cannot pass.
	REQUIRE_THAT(reaper_volume_from_volume_decibels(-6.0), Catch::Matchers::WithinRel(0.5011872336272722, 1e-12));
	REQUIRE_THAT(reaper_volume_from_volume_decibels(6.0), Catch::Matchers::WithinRel(1.9952623149688795, 1e-12));
	REQUIRE_THAT(volume_decibels_from_reaper_volume(0.5), Catch::Matchers::WithinAbs(-6.020599913279624, volume_decibels_tolerance));
	REQUIRE_THAT(volume_decibels_from_reaper_volume(2.0), Catch::Matchers::WithinAbs(6.020599913279624, volume_decibels_tolerance));
}

TEST_CASE("a quieter fader is a lower decibel value", "[unit_conversion][volume]")
{
	// Monotonicity, spot-checked. A conversion that is right at the bounds and
	// wrong in between would still pass the round-trip tests if it were wrong
	// symmetrically, and this is what catches that.
	for (std::size_t index = 1; index < representable_reaper_volumes.size(); ++index)
	{
		const double quieter = volume_decibels_from_reaper_volume(representable_reaper_volumes[index - 1]);
		const double louder = volume_decibels_from_reaper_volume(representable_reaper_volumes[index]);

		REQUIRE(quieter < louder);
	}
}

// ---------------------------------------------------------------------------
// Volume — the floor, which is the case the schema calls out
// ---------------------------------------------------------------------------

TEST_CASE("a silent fader reports the decibel floor rather than negative infinity", "[unit_conversion][volume][bounds]")
{
	// The whole reason the floor exists. std::log10(0.0) is -HUGE_VAL, which is not
	// a JSON number, so this has to be intercepted before the logarithm.
	const double silent = volume_decibels_from_reaper_volume(0.0);

	REQUIRE(std::isfinite(silent));
	REQUIRE(silent == minimum_volume_decibels);
}

TEST_CASE("a gain below the floor reports the floor", "[unit_conversion][volume][bounds]")
{
	REQUIRE(volume_decibels_from_reaper_volume(1e-12) == minimum_volume_decibels);
	REQUIRE(volume_decibels_from_reaper_volume(std::numeric_limits<double>::denorm_min()) == minimum_volume_decibels);

	// REAPER stores a phase-inverted fader as a negative gain. The magnitude is the
	// level and the sign is polarity, which the schema has no field for, so a
	// negative reads as the floor rather than as a NaN out of the logarithm.
	REQUIRE(volume_decibels_from_reaper_volume(-0.5) == minimum_volume_decibels);
}

TEST_CASE("volume conversion clamps to the schema bounds", "[unit_conversion][volume][bounds]")
{
	// Nothing may leave these functions outside the range the schemas declare —
	// project-context.schema.json and tool-output-defs.schema.json both say
	// [-150, 12] and [-1, 1] equivalents, and a payload outside them is refused.
	REQUIRE(volume_decibels_from_reaper_volume(1000.0) == maximum_volume_decibels);
	REQUIRE(reaper_volume_from_volume_decibels(96.0) == maximum_reaper_volume);
	REQUIRE(reaper_volume_from_volume_decibels(-400.0) == minimum_reaper_volume);

	for (const double reaper_volume : {0.0, 1e-30, minimum_reaper_volume, 0.5, unity_reaper_volume, maximum_reaper_volume, 12.0, 1e6})
	{
		const double decibels = volume_decibels_from_reaper_volume(reaper_volume);

		REQUIRE(decibels >= minimum_volume_decibels);
		REQUIRE(decibels <= maximum_volume_decibels);
	}

	for (const double decibels : {-1e6, -300.0, minimum_volume_decibels, -12.0, unity_volume_decibels, maximum_volume_decibels, 48.0, 1e6})
	{
		const double reaper_volume = reaper_volume_from_volume_decibels(decibels);

		REQUIRE(reaper_volume >= minimum_reaper_volume);
		REQUIRE(reaper_volume <= maximum_reaper_volume);
	}
}

TEST_CASE("a NaN volume reads as silence and an infinity clamps", "[unit_conversion][volume][bounds]")
{
	// Nothing non-finite may reach the JSON layer, but the two kinds are not the
	// same input. A NaN says nothing, so it takes the safe reading; an infinity
	// says which direction it means, so it clamps to that bound.
	const double quiet_nan = std::numeric_limits<double>::quiet_NaN();
	const double infinity = std::numeric_limits<double>::infinity();

	REQUIRE(volume_decibels_from_reaper_volume(quiet_nan) == minimum_volume_decibels);
	REQUIRE(volume_decibels_from_reaper_volume(-infinity) == minimum_volume_decibels);
	REQUIRE(volume_decibels_from_reaper_volume(infinity) == maximum_volume_decibels);

	REQUIRE(reaper_volume_from_volume_decibels(quiet_nan) == minimum_reaper_volume);
	REQUIRE(reaper_volume_from_volume_decibels(-infinity) == minimum_reaper_volume);
	REQUIRE(reaper_volume_from_volume_decibels(infinity) == maximum_reaper_volume);
}

// ---------------------------------------------------------------------------
// Volume — round trips, requirement 19.1
// ---------------------------------------------------------------------------

TEST_CASE("volume round-trips exactly at both bounds", "[unit_conversion][volume][round_trip][bounds]")
{
	// Exactly, not within tolerance. Requirement 19.1 singles out the bounds, and
	// they are the two values the implementation pairs by construction, so anything
	// less than equality here means the pairing broke.
	REQUIRE(volume_decibels_from_reaper_volume(minimum_reaper_volume) == minimum_volume_decibels);
	REQUIRE(reaper_volume_from_volume_decibels(minimum_volume_decibels) == minimum_reaper_volume);

	REQUIRE(volume_decibels_from_reaper_volume(maximum_reaper_volume) == maximum_volume_decibels);
	REQUIRE(reaper_volume_from_volume_decibels(maximum_volume_decibels) == maximum_reaper_volume);

	REQUIRE(reaper_volume_from_volume_decibels(volume_decibels_from_reaper_volume(minimum_reaper_volume)) == minimum_reaper_volume);
	REQUIRE(reaper_volume_from_volume_decibels(volume_decibels_from_reaper_volume(maximum_reaper_volume)) == maximum_reaper_volume);
	REQUIRE(volume_decibels_from_reaper_volume(reaper_volume_from_volume_decibels(minimum_volume_decibels)) == minimum_volume_decibels);
	REQUIRE(volume_decibels_from_reaper_volume(reaper_volume_from_volume_decibels(maximum_volume_decibels)) == maximum_volume_decibels);
}

TEST_CASE("stored volume survives a trip through decibels", "[unit_conversion][volume][round_trip]")
{
	for (const double reaper_volume : representable_reaper_volumes)
	{
		const double returned = reaper_volume_from_volume_decibels(volume_decibels_from_reaper_volume(reaper_volume));

		// Relative, because the range spans eight orders of magnitude and an
		// absolute tolerance would be meaningless at the quiet end.
		REQUIRE_THAT(returned, Catch::Matchers::WithinRel(reaper_volume, reaper_volume_relative_tolerance));
	}
}

TEST_CASE("decibels survive a trip through stored volume", "[unit_conversion][volume][round_trip]")
{
	for (const double decibels : representable_volume_decibels)
	{
		const double returned = volume_decibels_from_reaper_volume(reaper_volume_from_volume_decibels(decibels));

		REQUIRE_THAT(returned, Catch::Matchers::WithinAbs(decibels, volume_decibels_tolerance));
	}
}

// ---------------------------------------------------------------------------
// Pan — the values a producer would recognise
// ---------------------------------------------------------------------------

TEST_CASE("pan converts on the scale the schema describes", "[unit_conversion][pan]")
{
	// "-100 is hard left, 0 is centre, and 100 is hard right", from
	// project-context.schema.json's panPercent description.
	REQUIRE(pan_percent_from_reaper_pan(-1.0) == -100.0);
	REQUIRE(pan_percent_from_reaper_pan(1.0) == 100.0);
	REQUIRE_THAT(pan_percent_from_reaper_pan(-0.3), Catch::Matchers::WithinAbs(-30.0, pan_percent_tolerance));
	REQUIRE_THAT(pan_percent_from_reaper_pan(0.75), Catch::Matchers::WithinAbs(75.0, pan_percent_tolerance));

	REQUIRE(reaper_pan_from_pan_percent(-100.0) == -1.0);
	REQUIRE(reaper_pan_from_pan_percent(100.0) == 1.0);
	REQUIRE_THAT(reaper_pan_from_pan_percent(-30.0), Catch::Matchers::WithinAbs(-0.3, reaper_pan_tolerance));
	REQUIRE_THAT(reaper_pan_from_pan_percent(75.0), Catch::Matchers::WithinAbs(0.75, reaper_pan_tolerance));
}

TEST_CASE("panning further right is a higher percentage", "[unit_conversion][pan]")
{
	for (std::size_t index = 1; index < representable_reaper_pans.size(); ++index)
	{
		const double further_left = pan_percent_from_reaper_pan(representable_reaper_pans[index - 1]);
		const double further_right = pan_percent_from_reaper_pan(representable_reaper_pans[index]);

		REQUIRE(further_left < further_right);
	}
}

// ---------------------------------------------------------------------------
// Pan — bounds and centre, the cases requirement 19.2 names
// ---------------------------------------------------------------------------

TEST_CASE("pan centre is exactly centre and carries no sign", "[unit_conversion][pan][bounds]")
{
	REQUIRE(pan_percent_from_reaper_pan(centre_reaper_pan) == centre_pan_percent);
	REQUIRE(reaper_pan_from_pan_percent(centre_pan_percent) == centre_reaper_pan);

	// Negative zero would round-trip correctly but serialise as `-0`, so centre is
	// normalised. std::signbit is the only way to observe this: -0.0 == 0.0 is true.
	REQUIRE_FALSE(std::signbit(pan_percent_from_reaper_pan(-0.0)));
	REQUIRE_FALSE(std::signbit(reaper_pan_from_pan_percent(-0.0)));
	REQUIRE(pan_percent_from_reaper_pan(-0.0) == centre_pan_percent);
	REQUIRE(reaper_pan_from_pan_percent(-0.0) == centre_reaper_pan);
}

TEST_CASE("pan conversion clamps to the schema bounds", "[unit_conversion][pan][bounds]")
{
	REQUIRE(pan_percent_from_reaper_pan(-4.0) == minimum_pan_percent);
	REQUIRE(pan_percent_from_reaper_pan(4.0) == maximum_pan_percent);
	REQUIRE(reaper_pan_from_pan_percent(-400.0) == minimum_reaper_pan);
	REQUIRE(reaper_pan_from_pan_percent(400.0) == maximum_reaper_pan);

	for (const double reaper_pan : {-1e6, -1.5, minimum_reaper_pan, -0.25, centre_reaper_pan, 0.25, maximum_reaper_pan, 1.5, 1e6})
	{
		const double percent = pan_percent_from_reaper_pan(reaper_pan);

		REQUIRE(percent >= minimum_pan_percent);
		REQUIRE(percent <= maximum_pan_percent);
	}

	for (const double percent : {-1e6, -150.0, minimum_pan_percent, -25.0, centre_pan_percent, 25.0, maximum_pan_percent, 150.0, 1e6})
	{
		const double reaper_pan = reaper_pan_from_pan_percent(percent);

		REQUIRE(reaper_pan >= minimum_reaper_pan);
		REQUIRE(reaper_pan <= maximum_reaper_pan);
	}
}

TEST_CASE("a NaN pan reads as centre and an infinity clamps", "[unit_conversion][pan][bounds]")
{
	const double quiet_nan = std::numeric_limits<double>::quiet_NaN();
	const double infinity = std::numeric_limits<double>::infinity();

	REQUIRE(pan_percent_from_reaper_pan(quiet_nan) == centre_pan_percent);
	REQUIRE(reaper_pan_from_pan_percent(quiet_nan) == centre_reaper_pan);

	// An infinity is directional, so it clamps to the corresponding bound rather
	// than collapsing to centre — unlike a NaN, which says nothing.
	REQUIRE(pan_percent_from_reaper_pan(-infinity) == minimum_pan_percent);
	REQUIRE(pan_percent_from_reaper_pan(infinity) == maximum_pan_percent);
	REQUIRE(reaper_pan_from_pan_percent(-infinity) == minimum_reaper_pan);
	REQUIRE(reaper_pan_from_pan_percent(infinity) == maximum_reaper_pan);
}

// ---------------------------------------------------------------------------
// Pan — round trips, requirement 19.2
// ---------------------------------------------------------------------------

TEST_CASE("pan round-trips exactly at both bounds and at centre", "[unit_conversion][pan][round_trip][bounds]")
{
	REQUIRE(reaper_pan_from_pan_percent(pan_percent_from_reaper_pan(minimum_reaper_pan)) == minimum_reaper_pan);
	REQUIRE(reaper_pan_from_pan_percent(pan_percent_from_reaper_pan(centre_reaper_pan)) == centre_reaper_pan);
	REQUIRE(reaper_pan_from_pan_percent(pan_percent_from_reaper_pan(maximum_reaper_pan)) == maximum_reaper_pan);

	REQUIRE(pan_percent_from_reaper_pan(reaper_pan_from_pan_percent(minimum_pan_percent)) == minimum_pan_percent);
	REQUIRE(pan_percent_from_reaper_pan(reaper_pan_from_pan_percent(centre_pan_percent)) == centre_pan_percent);
	REQUIRE(pan_percent_from_reaper_pan(reaper_pan_from_pan_percent(maximum_pan_percent)) == maximum_pan_percent);
}

TEST_CASE("stored pan survives a trip through a percentage", "[unit_conversion][pan][round_trip]")
{
	for (const double reaper_pan : representable_reaper_pans)
	{
		const double returned = reaper_pan_from_pan_percent(pan_percent_from_reaper_pan(reaper_pan));

		// Absolute, because the stored range is [-1, 1] and centre is in it, where a
		// relative tolerance divides by zero.
		REQUIRE_THAT(returned, Catch::Matchers::WithinAbs(reaper_pan, reaper_pan_tolerance));
	}
}

TEST_CASE("a pan percentage survives a trip through stored pan", "[unit_conversion][pan][round_trip]")
{
	for (const double percent : representable_pan_percents)
	{
		const double returned = pan_percent_from_reaper_pan(reaper_pan_from_pan_percent(percent));

		REQUIRE_THAT(returned, Catch::Matchers::WithinAbs(percent, pan_percent_tolerance));
	}
}

// ---------------------------------------------------------------------------
// The contract the tolerances rest on
// ---------------------------------------------------------------------------

TEST_CASE("the stored bounds are the gains the decibel bounds name", "[unit_conversion][bounds]")
{
	// The constants are written as literals because std::pow is not a constant
	// expression, which means nothing in the header checks them against the formula
	// they came from. This does. A mistyped digit in either literal would leave
	// every round-trip test passing — they would all agree on the wrong number —
	// and would report the wrong decibel value to a producer.
	REQUIRE(minimum_reaper_volume == std::pow(10.0, minimum_volume_decibels / decibels_per_amplitude_decade));
	REQUIRE(maximum_reaper_volume == std::pow(10.0, maximum_volume_decibels / decibels_per_amplitude_decade));
}

TEST_CASE("the tolerances leave room over measured round-trip error", "[unit_conversion][bounds]")
{
	// Guards the tolerances themselves against being tightened to the point where
	// ordinary double rounding fails the suite, or loosened to where they would
	// stop catching anything. The measured worst case across the representable
	// range is around 1e-14 dB and 3e-15 relative in stored gain.
	REQUIRE(volume_decibels_tolerance > 1e-13);
	REQUIRE(volume_decibels_tolerance < 1e-6);
	REQUIRE(reaper_volume_relative_tolerance > 1e-14);
	REQUIRE(reaper_volume_relative_tolerance < 1e-9);
	REQUIRE(pan_percent_tolerance > 1e-13);
	REQUIRE(reaper_pan_tolerance > 1e-15);
}
