// Structural role derivation.
//
// REAPER has no bus object. A producer's "drum bus" is a folder parent, or an aux
// return fed by sends, or just a track with children routed around it — and the API
// cannot be asked which. So the extension derives a role per track from routing
// topology signals, and the agent reasons with the role rather than with raw folder
// deltas it would have to interpret itself (requirement 18.1).
//
// Three things about this file are deliberate.
//
// The role enumeration matches `structuralRole` in project-context.schema.json and
// tool-output-defs.schema.json exactly — the same five strings, spelled the same
// way. The Project Context Builder serialises what `to_schema_string` returns and
// validates the result against the vendored bundle, so a spelling drift here is a
// snapshot the server refuses. The round-trip test is what holds that.
//
// The derivation reports the signals it used alongside the role it chose
// (requirement 18.2). A role is a summary, and a summary that turns out to be wrong
// is nearly undiagnosable on its own: "why did it call the drum bus silent" has no
// answer without the evidence. Returning both means one log line answers it.
//
// And the derivation is a pure function of those signals (requirement 18.3), which
// is enforced structurally rather than by discipline. `StructuralRoleSignals` holds
// no index, GUID, name, pointer, or iterator — nothing that could make the answer
// depend on where the track sits in the list or when it was visited. The function
// is a free `constexpr` function over that aggregate, so it cannot read global
// state, call into REAPER, or allocate. Purity is not something this file has to
// remember to preserve; there is nothing available to it that would break it.
//
// Nothing here includes the REAPER API. The Project Context Builder collects the
// signals; this file only classifies them, which is what keeps it unit-testable
// outside REAPER.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace sesh_ai::context
{
	// The five roles, spelled as the schemas spell them.
	enum class StructuralRole
	{
		// A folder parent that actually sums: at least one source of signal reaches
		// it, so mixing or rendering it means something.
		summing_folder_parent,

		// A folder parent whose children route around it and which has no signal of
		// its own. Looks like a bus, sums nothing. Reporting this is how the agent can
		// tell the producer their bus carries nothing, so the line between it and
		// summing_folder_parent is the one that matters most here — see
		// derive_structural_role.
		silent_folder_parent,

		// A track fed by sends rather than by material of its own — a reverb return,
		// a parallel compression bus.
		aux_return,

		// A track carrying its own material. The common case, and the fallback: a
		// track is normal when nothing else describes it better.
		normal,

		// The master track. Not in the indexed track list, holds no media items, has
		// no parent to send to.
		master
	};

	// Every role, for exhaustive iteration in tests and for anything that needs to
	// enumerate the enum without repeating it.
	inline constexpr std::array<StructuralRole, 5> all_structural_roles{
		StructuralRole::summing_folder_parent,
		StructuralRole::silent_folder_parent,
		StructuralRole::aux_return,
		StructuralRole::normal,
		StructuralRole::master
	};

	// The raw topology signals a role is derived from, and the whole of what the
	// derivation is allowed to look at.
	//
	// Requirement 18.1 names five: accumulated depth, parent send state, item count,
	// FX presence, and receive count. Two of those arrive as a pair of fields rather
	// than one, and both splits are the derivation being explicit about what it
	// actually needs.
	//
	// Accumulating I_FOLDERDEPTH across track order tells you two different things
	// about a track: the level it sits at, and whether anything sits inside it. Only
	// the second changes the role — a return is a return at any nesting level — but
	// both are reported, because a role that looks wrong is usually a depth
	// accumulation bug and the level is what makes that visible.
	//
	// Parent send state splits the same way. A track's own B_MAINSEND says where its
	// output goes. Its children's B_MAINSEND says whether anything arrives. For a
	// folder parent it is the children's flags that decide whether it sums at all,
	// which is invisible to send enumeration and is exactly what the
	// silent-versus-summing distinction turns on.
	struct StructuralRoleSignals
	{
		// Whether these signals came from the master track rather than from a track
		// in the indexed list. Not derivable from the other signals — the master is a
		// different object in REAPER's model, with no depth and no items — so it is
		// carried rather than inferred.
		bool is_master_track = false;

		// Accumulated depth: the folder nesting level of this track. 0 is top level,
		// 1 is inside one folder, and so on. Non-negative by requirement 18.1;
		// normalization clamps a negative, which would mean the accumulation upstream
		// went wrong.
		int accumulated_folder_depth = 0;

		// Accumulated depth, read from the other side: how many tracks sit directly
		// inside this track's folder. 0 means the track opens no folder and so is not
		// a folder parent. Non-negative.
		int child_track_count = 0;

		// Parent send state of those children: how many of them have B_MAINSEND on
		// and therefore feed this track. Non-negative, and never more than
		// child_track_count — normalization enforces the ceiling.
		int children_with_parent_send_enabled = 0;

		// Parent send state of this track itself: its B_MAINSEND flag. The target is
		// the enclosing folder parent inside a folder, the master track at top level.
		bool parent_send_enabled = true;

		// Number of media items on the track. Non-negative.
		int item_count = 0;

		// Whether the track's FX chain holds anything at all.
		bool has_fx = false;

		// Number of explicit receives into the track. Non-negative. Folder summing
		// and the master send do not appear here, which is why the parent send
		// signals are carried separately.
		int receive_count = 0;
	};

	constexpr bool operator==(const StructuralRoleSignals& left, const StructuralRoleSignals& right)
	{
		return left.is_master_track == right.is_master_track
			&& left.accumulated_folder_depth == right.accumulated_folder_depth
			&& left.child_track_count == right.child_track_count
			&& left.children_with_parent_send_enabled == right.children_with_parent_send_enabled
			&& left.parent_send_enabled == right.parent_send_enabled
			&& left.item_count == right.item_count
			&& left.has_fx == right.has_fx
			&& left.receive_count == right.receive_count;
	}

	constexpr bool operator!=(const StructuralRoleSignals& left, const StructuralRoleSignals& right)
	{
		return !(left == right);
	}

	// A role plus the evidence for it (requirement 18.2). `signals` is the normalized
	// form the derivation actually reasoned about, not the caller's input, so a log
	// line shows the values that produced the role rather than values that were
	// adjusted on the way in.
	struct StructuralRoleDerivation
	{
		StructuralRole role = StructuralRole::normal;
		StructuralRoleSignals signals{};
	};

	// Brings the counted signals into the non-negative range requirement 18.1 states,
	// and caps the feeding-children count at the number of children.
	//
	// Clamping rather than rejecting keeps the derivation total: every combination of
	// signals, including nonsense ones, yields exactly one role. A partial function
	// would have the Project Context Builder deciding what to do about an
	// out-of-range count in the middle of assembling a snapshot, and there is no good
	// answer available to it there. The normalized values are reported, so a clamp
	// that fired is visible in the same log line as the role.
	constexpr StructuralRoleSignals normalize_structural_role_signals(const StructuralRoleSignals& signals)
	{
		StructuralRoleSignals normalized = signals;

		if (normalized.accumulated_folder_depth < 0)
		{
			normalized.accumulated_folder_depth = 0;
		}

		if (normalized.child_track_count < 0)
		{
			normalized.child_track_count = 0;
		}

		if (normalized.item_count < 0)
		{
			normalized.item_count = 0;
		}

		if (normalized.receive_count < 0)
		{
			normalized.receive_count = 0;
		}

		if (normalized.children_with_parent_send_enabled < 0)
		{
			normalized.children_with_parent_send_enabled = 0;
		}

		if (normalized.children_with_parent_send_enabled > normalized.child_track_count)
		{
			normalized.children_with_parent_send_enabled = normalized.child_track_count;
		}

		return normalized;
	}

	// Whether these signals describe a folder parent: something sits inside its
	// folder. Says nothing about whether it sums.
	constexpr bool signals_describe_folder_parent(const StructuralRoleSignals& signals)
	{
		return !signals.is_master_track && signals.child_track_count > 0;
	}

	// Derives exactly one role from the signals, and returns them alongside it.
	//
	// The order of the tests is the classification: master first because it is a
	// different object, then folder parents because being one overrides what the
	// track otherwise looks like, then returns, then everything else.
	//
	// **summing versus silent is the distinction worth reading carefully**, because
	// the role is reported to the agent — in the project context snapshot and in
	// `list_tracks` — and the agent relays it to the producer as a statement about
	// their routing. Getting it wrong is how the producer hears something untrue about
	// their own session.
	//
	// A folder parent sums a child only when that child's B_MAINSEND is on. Switch it
	// off on every child — a common routing choice, since it is how you send children
	// straight to the master while keeping them visually grouped — and the parent
	// still looks like a bus in the track list while carrying no signal from its
	// children at all. Send enumeration cannot see this: B_MAINSEND is a flag, not a
	// send.
	//
	// But "no child feeds me" is not the same as "I am silent", and conflating them
	// is the failure mode to avoid. A folder parent has three other ways to carry
	// audio: its own media items, explicit receives from elsewhere, and an FX chain
	// that generates rather than processes — an instrument, a synth, a noise source.
	// So silence is claimed only when every possible source of signal is absent: no
	// child feeding it, no items, no receives, no FX.
	//
	// FX presence is the conservative half of that. Whether a plugin generates or
	// merely processes cannot be told from the chain without instantiating it, so a
	// chain of any kind is treated as a possible source and the parent is reported as
	// summing. That direction is chosen on purpose. A false "silent" tells the producer
	// their bus is dead when it is not, which sends them looking for a routing problem
	// that does not exist; a false "summing" merely withholds a remark they had not
	// asked for. Silence is claimed only where it is unambiguous.
	constexpr StructuralRoleDerivation derive_structural_role(const StructuralRoleSignals& signals)
	{
		const StructuralRoleSignals normalized = normalize_structural_role_signals(signals);

		if (normalized.is_master_track)
		{
			return StructuralRoleDerivation{StructuralRole::master, normalized};
		}

		if (normalized.child_track_count > 0)
		{
			const bool no_child_feeds_it = normalized.children_with_parent_send_enabled == 0;
			const bool carries_nothing_of_its_own = normalized.item_count == 0
				&& normalized.receive_count == 0
				&& !normalized.has_fx;

			if (no_child_feeds_it && carries_nothing_of_its_own)
			{
				return StructuralRoleDerivation{StructuralRole::silent_folder_parent, normalized};
			}

			return StructuralRoleDerivation{StructuralRole::summing_folder_parent, normalized};
		}

		// Things feed it and it holds no material of its own — a return. Its own
		// parent send state does not change that: a return feeding only a sidechain
		// rather than the master is still a return.
		if (normalized.receive_count > 0 && normalized.item_count == 0)
		{
			return StructuralRoleDerivation{StructuralRole::aux_return, normalized};
		}

		return StructuralRoleDerivation{StructuralRole::normal, normalized};
	}

	// The schema spelling of a role. Total over the enum, so there is no fallback
	// string that could reach a snapshot and fail validation with a value nobody
	// wrote.
	constexpr std::string_view to_schema_string(StructuralRole role)
	{
		switch (role)
		{
			case StructuralRole::summing_folder_parent:
				return "summing_folder_parent";
			case StructuralRole::silent_folder_parent:
				return "silent_folder_parent";
			case StructuralRole::aux_return:
				return "aux_return";
			case StructuralRole::normal:
				return "normal";
			case StructuralRole::master:
				return "master";
		}

		// Unreachable for any enumerator. Present because a switch over an enum class
		// with no default is not a guarantee to the compiler that the value is one of
		// them.
		return "normal";
	}

	// The inverse, for reading a role back out of a payload. Empty for anything that
	// is not one of the five schema strings, so an unknown role is a refusal at the
	// call site rather than a silent fallback to `normal`.
	constexpr std::optional<StructuralRole> structural_role_from_schema_string(std::string_view role)
	{
		for (StructuralRole candidate : all_structural_roles)
		{
			if (to_schema_string(candidate) == role)
			{
				return candidate;
			}
		}

		return std::nullopt;
	}

	// One log line carrying the role and every signal behind it — requirement 18.2's
	// "diagnosable from a log" made concrete. Reads as `role=… depth=… children=…`
	// so the evidence for a wrong role is in the same line as the role.
	inline std::string describe_structural_role_derivation(const StructuralRoleDerivation& derivation)
	{
		const StructuralRoleSignals& signals = derivation.signals;

		std::string description{"role="};
		description += to_schema_string(derivation.role);
		description += " master=";
		description += signals.is_master_track ? "true" : "false";
		description += " accumulatedFolderDepth=";
		description += std::to_string(signals.accumulated_folder_depth);
		description += " childTrackCount=";
		description += std::to_string(signals.child_track_count);
		description += " childrenWithParentSendEnabled=";
		description += std::to_string(signals.children_with_parent_send_enabled);
		description += " parentSendEnabled=";
		description += signals.parent_send_enabled ? "true" : "false";
		description += " itemCount=";
		description += std::to_string(signals.item_count);
		description += " hasFx=";
		description += signals.has_fx ? "true" : "false";
		description += " receiveCount=";
		description += std::to_string(signals.receive_count);

		return description;
	}
}
