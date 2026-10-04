// Unit conversion between REAPER's stored representations and the producer-facing
// units the protocol schemas report (requirement 19, design "Unit conversion").
//
// REAPER stores a fader as a linear gain factor where 1.0 is unity, and a pan
// position as a number from -1.0 to 1.0. Neither is what a producer says. The
// schemas report decibels and a percentage instead, so "bring it down 3 dB" and
// "30% left" need no translation at the other end of the conversation.
//
// The contract is taken from the vendored schemas, not from REAPER:
//
//   schemas/project-context.schema.json          definitions.volumeDecibels  [-150, 12]
//                                                definitions.panPercent      [-100, 100]
//   schemas/mcp-tools/outputs/tool-output-defs.schema.json   the same two, same bounds
//
// Both sides agree, deliberately — the output definition says as much ("Same range
// the input definitions accept, so a level read back can be written back"), and
// these functions are what makes that true.
//
// ---------------------------------------------------------------------------
// The decibel floor
//
// -150 dB is the schema's stand-in for silence, and the reason is recorded in the
// schema itself: a silent track is expressed through the muted flag, which is
// reversible and visible, rather than through an unrepresentable number. REAPER
// will hand out a stored gain of exactly 0.0 for a fader pulled to the bottom, and
// 20*log10(0.0) is negative infinity — which is not a JSON number, so it would not
// merely be wrong, it would fail serialisation. So the floor is applied before the
// logarithm rather than after it, and silence reads as -150 dB.
//
// ---------------------------------------------------------------------------
// Round-tripping, and why the bounds are exact
//
// Requirement 19.1 and 19.2 ask for the round trip to hold at both bounds, and for
// pan also at centre. Floating-point arithmetic does not give that for free: the
// double nearest to 10^-7.5 does not log back to exactly -150 in general, and a
// clamp written with a strict comparison would let the endpoint fall through to the
// arithmetic path. So each bound is handled by an inclusive comparison that returns
// the paired constant, which makes the two endpoints and pan's centre exact by
// construction rather than by luck. The interior is ordinary arithmetic and carries
// ordinary rounding error, which is what the tolerances below are for.
//
// The round trip is only meaningful over the range the schema can represent.
// Outside it the conversion clamps, so a stored gain of 8.0 reports as 12 dB and
// converts back to roughly 3.98 — information is lost because the schema has
// nowhere to put it. That is the intended behaviour, not a defect in the round
// trip: properties 10 and 11 in the design are quantified over REAPER's stored
// *range*, meaning the representable one bounded by the constants here.
//
// ---------------------------------------------------------------------------
// Tolerance
//
// Neither schema states a tolerance, and nothing else in the protocol does either —
// requirement 19 says "the schema's stated tolerance" for a figure the schema does
// not carry. Rather than leave it to each caller to invent one, the tolerances are
// fixed here so the conversion and its tests agree on a single number.
//
// The values are chosen with a wide margin over measured double round-trip error
// (worst observed: 1.4e-14 dB, 3.2e-15 relative in stored gain, 1.1e-16 in stored
// pan) while staying far below anything audible or representable in REAPER's own
// UI, which shows two decimal places of a decibel. Anything landing outside these
// bounds is a bug in the conversion, not accumulated rounding.
//
// If the schemas later state a tolerance, these constants are the one place to
// reconcile.

#ifndef SESH_AI_DAW_UNIT_CONVERSION_H
#define SESH_AI_DAW_UNIT_CONVERSION_H

#include <cmath>

// Pure arithmetic over doubles — no REAPER C API, no JSON, nothing that has to run
// on the main thread. Defined inline in the header because every definition here is
// a handful of operations that wants to inline at the call site, and because a
// header-only pure-math unit is testable without being linked into the extension's
// shared library.
namespace sesh_ai::daw
{
	// The schema's reported units.
	inline constexpr double minimum_volume_decibels = -150.0;
	inline constexpr double maximum_volume_decibels = 12.0;
	inline constexpr double unity_volume_decibels = 0.0;

	inline constexpr double minimum_pan_percent = -100.0;
	inline constexpr double maximum_pan_percent = 100.0;
	inline constexpr double centre_pan_percent = 0.0;

	// REAPER's stored representations. The volume bounds are the linear gain
	// factors paired with the decibel bounds above: 10^(-150/20) and 10^(12/20),
	// written as literals because std::pow is not usable in a constant expression.
	// They are the nearest double to each value, which is what the paired
	// conversion returns, which is what makes the round trip exact at the bounds.
	inline constexpr double minimum_reaper_volume = 3.1622776601683792e-08;
	inline constexpr double maximum_reaper_volume = 3.9810717055349722;
	inline constexpr double unity_reaper_volume = 1.0;

	inline constexpr double minimum_reaper_pan = -1.0;
	inline constexpr double maximum_reaper_pan = 1.0;
	inline constexpr double centre_reaper_pan = 0.0;

	// Round-trip tolerances. See the note above on why they are stated here.
	inline constexpr double volume_decibels_tolerance = 1e-9;
	inline constexpr double reaper_volume_relative_tolerance = 1e-12;
	inline constexpr double pan_percent_tolerance = 1e-9;
	inline constexpr double reaper_pan_tolerance = 1e-12;

	// The decibel-per-decade factor for an amplitude ratio: 20*log10(gain). Twenty
	// rather than ten because a fader scales amplitude, not power.
	inline constexpr double decibels_per_amplitude_decade = 20.0;

	namespace detail
	{
		// Hand-rolled rather than std::clamp so the header stays free of
		// <algorithm>. The argument order is the same.
		//
		// Note what happens to a NaN: neither comparison is true, so it falls
		// through and is returned unchanged. Every caller below therefore has to
		// deal with a NaN before reaching here, and each one does. An infinity
		// needs no special handling — it is directional, so it compares against
		// the bounds correctly and clamps to the one it is heading for.
		constexpr double clamp_to_range(double value, double lowest, double highest)
		{
			if (value < lowest)
			{
				return lowest;
			}

			if (value > highest)
			{
				return highest;
			}

			return value;
		}
	}

	// REAPER's stored gain factor to the schema's decibels.
	//
	// A stored gain at or below the floor — including exactly 0.0, and including a
	// negative, which REAPER uses for a phase-inverted fader — reports as
	// -150 dB. A NaN reports as the floor too: it cannot be serialised as a JSON
	// number, and reading an unusable value as silence is the failure a producer
	// can see and undo. An infinity is not lumped in with it; it clamps to the
	// bound it points at, because unlike a NaN it says which direction it means.
	inline double volume_decibels_from_reaper_volume(double reaper_volume)
	{
		if (std::isnan(reaper_volume) || reaper_volume <= minimum_reaper_volume)
		{
			return minimum_volume_decibels;
		}

		if (reaper_volume >= maximum_reaper_volume)
		{
			return maximum_volume_decibels;
		}

		const double decibels = decibels_per_amplitude_decade * std::log10(reaper_volume);

		return detail::clamp_to_range(decibels, minimum_volume_decibels, maximum_volume_decibels);
	}

	// The schema's decibels back to REAPER's stored gain factor.
	//
	// The bounds return the paired constants directly, so -150 dB gives back the
	// same stored gain that produced it and +12 dB likewise. A NaN is treated as
	// the floor, matching the direction above.
	inline double reaper_volume_from_volume_decibels(double volume_decibels)
	{
		if (std::isnan(volume_decibels) || volume_decibels <= minimum_volume_decibels)
		{
			return minimum_reaper_volume;
		}

		if (volume_decibels >= maximum_volume_decibels)
		{
			return maximum_reaper_volume;
		}

		const double reaper_volume = std::pow(10.0, volume_decibels / decibels_per_amplitude_decade);

		return detail::clamp_to_range(reaper_volume, minimum_reaper_volume, maximum_reaper_volume);
	}

	// REAPER's stored pan position to the schema's percentage.
	//
	// The mapping is linear and the scale factor is exactly 100, so both bounds are
	// exact without help. Centre is normalised away from negative zero: -0.0 * 100
	// is -0.0, which round-trips correctly but serialises as `-0` and would have an
	// agent reading a pan the schema calls centre as something else.
	// A NaN reads as centre, for the same reason silence is the volume fallback:
	// centre is the neutral position a producer can hear and correct. An infinity
	// is directional and clamps hard left or hard right instead.
	inline double pan_percent_from_reaper_pan(double reaper_pan)
	{
		if (std::isnan(reaper_pan))
		{
			return centre_pan_percent;
		}

		const double clamped_reaper_pan = detail::clamp_to_range(reaper_pan, minimum_reaper_pan, maximum_reaper_pan);

		if (clamped_reaper_pan == centre_reaper_pan)
		{
			return centre_pan_percent;
		}

		return clamped_reaper_pan * maximum_pan_percent;
	}

	// The schema's percentage back to REAPER's stored pan position.
	inline double reaper_pan_from_pan_percent(double pan_percent)
	{
		if (std::isnan(pan_percent))
		{
			return centre_reaper_pan;
		}

		const double clamped_pan_percent = detail::clamp_to_range(pan_percent, minimum_pan_percent, maximum_pan_percent);

		if (clamped_pan_percent == centre_pan_percent)
		{
			return centre_reaper_pan;
		}

		return clamped_pan_percent / maximum_pan_percent;
	}
}

#endif
