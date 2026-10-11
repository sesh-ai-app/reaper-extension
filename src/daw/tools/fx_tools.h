// The eight FX tools: add_fx, remove_fx, set_fx_bypass, set_fx_parameter,
// get_fx_parameters, list_track_fx, list_installed_fx, and apply_fx_destructively
// (task 10.6, requirements 9.1 and 9.2).
//
// Header-only and inline, for the reason `tool_executor.h` gives for itself: the
// Catch2 target compiles what it finds under `tests/` and does not compile `src/`, so
// logic the suite exercises has to be visible through the header. `reaper_fx_host.h`
// and its one translation unit are the other side of the seam, and they contain no
// decisions at all.
//
// ---------------------------------------------------------------------------
// No FX tool can refuse
//
// `messages/tool-result-refusal.schema.json` closes `reason` at five values —
// `foreign_undo_entries`, `output_file_collision`, `ambiguous_track_selector`,
// `unresolved_track_selector`, `signal_cycle` — and not one of them describes anything
// an FX tool decides. A chain position past the end of the chain, a parameter name the
// plugin does not expose, a value outside the range the plugin reported, an identifier
// for an FX the producer has not installed: every one of those is an invalid call, and
// there is no acknowledgement the producer could set on a retry that would make it
// valid. Requirement 9.6 settles the same question for an inverted time range in the
// same direction, and the reasoning transfers.
//
// So each of these is a **failed action carrying a reason**, and the mutating tools
// below register with no precondition check. The only refusal an FX tool can produce is
// target resolution's, which `tool_executor_of::execute` has already dealt with before
// a handler is reached.
//
// ---------------------------------------------------------------------------
// A parameter outside its range fails; a parameter the plugin moved is reported
//
// `set-fx-parameter.schema.json` says `value` is "within the range get_fx_parameters
// reported for this parameter". A value outside that range is therefore a call that was
// built wrong, and clamping it would set something other than what was asked while
// reporting success — the producer is told the threshold is at -30 dB when it is at
// -24. So `apply_set_fx_parameter` checks the requested value against the range this
// FX reported and fails the action without writing anything when it falls outside.
//
// That does not make the output schema's `clamped` field dead, and it is worth being
// precise about why, because the two look contradictory at a glance. The range REAPER
// reports is what the plugin *claims*; what the plugin *honours* can be narrower. A
// stepped parameter quantises, a plugin with a stale range reports bounds it no longer
// accepts, and either can hold a value that passed the range check. So the write is
// followed by a read-back, and `clamped` is true when what landed is not what was
// asked for. The two rules divide cleanly: out of the reported range is the caller's
// error and fails, inside it and moved anyway is the plugin's business and is reported.
//
// ---------------------------------------------------------------------------
// FX indices are positional, so identity is read before the chain is touched
//
// Removing the FX at index 1 renumbers everything above it, and `remove-fx.schema.json`
// says so in its own `fxIndex` description. Two consequences are handled here.
//
// `remove_fx` reads the FX's name *before* deleting it, because after the delete there
// is nothing at that index to read a name from — the output schema requires `fxName`
// and its description says exactly this.
//
// The second is the multi-target case, and it is worth recording what the contract
// actually admits: **no FX tool takes an array of indices**. `remove-fx.schema.json`
// requires one integer `fxIndex` and sets `additionalProperties: false`, so the trap
// where the second removal in an array addresses a different FX than the caller meant
// is not reachable through a validated input. `plan_fx_removals` still exists and is
// still what `apply_remove_fx` goes through, because the shift-safe ordering is the
// part that is easy to get wrong later and cheap to settle now: targets are resolved to
// (index, name) pairs against one read of the chain, and the removals then run in
// **descending** index order, so no removal can renumber a target that has not been
// removed yet. Ascending order corrupts a chain on the second element while passing
// every single-element test, which is why the descending order is asserted directly
// rather than left implied.
//
// ---------------------------------------------------------------------------
// Requirement 23.9, three times, and it is not one answer
//
// A read whose result set is too large must paginate or summarise rather than truncate
// silently. What "rather than silently" can mean is decided by the output schema, and
// the three FX reads do not agree:
//
//   - `list_installed_fx` caps at the call's `maximumResults` (default 20, hard ceiling
//     100 from both the input and the output schema) and reports `totalInRange` and
//     `truncated`. A capped read is visible.
//   - `get_fx_parameters` has the same two fields, so a plugin exposing more than 1024
//     parameters is capped at 1024 with `truncated` true.
//   - `list_track_fx` has **neither field**, and caps `fx` at 256. There is no way to
//     say "there were more", so a chain longer than 256 cannot be reported honestly and
//     the read fails with a reason instead. Capping silently is the one thing
//     requirement 23.9 forbids. 256 FX on one chain is not a session anybody has, which
//     is why this costs nothing; the alternative would be a field the schema does not
//     have.
//
// All three failures take the same shape, because a read tool has only one: a
// `handler_action_outcomes` carrying one failed action with the reason. That payload holds
// `actions` and nothing else, and it validates against
// `messages/tool-result-partial.schema.json` rather than against the read's own output
// schema — `tool_executor.h`'s `result_schema_path_for` is where that is decided and its
// header explains why. Worth knowing here because these three schemas are closed with
// `additionalProperties: false` around fields the failure cannot supply:
// `list-track-fx.schema.json` requires `track` and `fx`, `get-fx-parameters.schema.json`
// requires six fields including `truncated`. Routed to the tool's own schema, a failed read
// would be refused by the extension's own outbound validation and the producer would see
// the tool do nothing at all.
//
// ---------------------------------------------------------------------------
// apply_fx_destructively
//
// It rewrites take audio, so it is the one tool here where getting the undo report
// wrong has a cost the producer cannot walk back. The three places that classify it
// agree, and this file follows them rather than deciding anything:
// `apply-fx-destructively.schema.json` marks it `classification: destructive` with
// `undoEffect: "mutates"`; `infra/lib/tool-classification.js` puts it at `high` risk,
// the band reserved for operations that discard data REAPER's undo cannot fully
// restore, so the Bedrock agent must obtain `user_confirmation` before it is dispatched
// at all; and the output schema requires `undoPositionBefore` and explains that with
// `replacedExistingTake` true "REAPER's undo is then the only way back".
//
// So it registers as a mutating tool, one undo block, and the honesty lives in
// `replacedExistingTake` — reported from what was actually done rather than echoed from
// the input, because the producer's route back differs entirely between the two and
// that field is the only thing that tells them which they got.
//
// `appliedFxCount` counts the FX that were actually rendered in, which is not the chain
// length: a bypassed FX is not applied, and neither is an offline one, which REAPER has
// not loaded. A producer who bypassed a plugin to audition without it would not expect
// it baked in. Zero is a real answer and not an error — the output schema's `minimum: 0`
// contemplates it, and applying an empty chain is how a producer consolidates items —
// so it is reported rather than rejected.
//
// Per-item outcomes are the action array requirement 9.4 describes, and each outcome's
// target is the item's GUID. That is deliberate: a partial outcome carries no
// tool-specific fields, so the succeeded targets are the only thing left that says
// which items were processed, and naming them by GUID keeps the partial as informative
// as `processedItemGuids` would have been.

#ifndef SESH_AI_DAW_TOOLS_FX_TOOLS_H
#define SESH_AI_DAW_TOOLS_FX_TOOLS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <daw/tool_executor.h>

namespace sesh_ai::daw::tools
{
	// ---------------------------------------------------------------------------
	// Contract values
	//
	// Spelled once each, because every one is a string the server or the model reads,
	// where a typo is a protocol bug rather than a compile error.
	// ---------------------------------------------------------------------------

	inline constexpr std::string_view add_fx_tool_name{"add_fx"};
	inline constexpr std::string_view remove_fx_tool_name{"remove_fx"};
	inline constexpr std::string_view set_fx_bypass_tool_name{"set_fx_bypass"};
	inline constexpr std::string_view set_fx_parameter_tool_name{"set_fx_parameter"};
	inline constexpr std::string_view get_fx_parameters_tool_name{"get_fx_parameters"};
	inline constexpr std::string_view list_track_fx_tool_name{"list_track_fx"};
	inline constexpr std::string_view list_installed_fx_tool_name{"list_installed_fx"};
	inline constexpr std::string_view apply_fx_destructively_tool_name{"apply_fx_destructively"};

	// `code` values on a failed action. Every FX-specific problem lands on one of these,
	// because none of them is a refusal — see the file header.
	inline constexpr std::string_view fx_host_unavailable_code{"fx_host_unavailable"};
	inline constexpr std::string_view fx_input_unreadable_code{"fx_input_unreadable"};
	inline constexpr std::string_view fx_index_out_of_range_code{"fx_index_out_of_range"};
	inline constexpr std::string_view fx_chain_position_out_of_range_code{"fx_chain_position_out_of_range"};
	inline constexpr std::string_view fx_identifier_not_installed_code{"fx_identifier_not_installed"};
	inline constexpr std::string_view fx_removal_failed_code{"fx_removal_failed"};
	inline constexpr std::string_view fx_bypass_write_failed_code{"fx_bypass_write_failed"};
	inline constexpr std::string_view fx_parameter_not_found_code{"fx_parameter_not_found"};
	inline constexpr std::string_view fx_parameter_out_of_range_code{"fx_parameter_out_of_range"};
	inline constexpr std::string_view fx_parameter_write_failed_code{"fx_parameter_write_failed"};
	inline constexpr std::string_view fx_chain_too_long_to_report_code{"fx_chain_too_long_to_report"};
	inline constexpr std::string_view fx_item_not_on_track_code{"fx_item_not_on_track"};
	inline constexpr std::string_view fx_apply_failed_code{"fx_apply_failed"};
	inline constexpr std::string_view fx_track_not_resolved_code{"fx_track_not_resolved"};
	inline constexpr std::string_view fx_too_many_items_code{"fx_too_many_items"};

	// Bounds from the output schemas, so a payload these handlers fill in cannot fail
	// the outbound validation requirement 4.1 asks for.
	//
	// `list-track-fx.schema.json` caps `fx` at 256 and gives no way to say the list was
	// cut, which is why exceeding it fails rather than truncates.
	inline constexpr std::size_t maximum_reported_chain_length = 256;

	// `get-fx-parameters.schema.json` caps `parameters` at 1024, and carries
	// `totalInRange` and `truncated` to say when the cap bit.
	inline constexpr std::size_t maximum_reported_fx_parameters = 1024;

	// `list-installed-fx.schema.json` caps `matches` at 100, and its input caps
	// `maximumResults` at the same number with a default of 20.
	inline constexpr std::size_t maximum_installed_fx_matches = 100;
	inline constexpr std::size_t default_installed_fx_matches = 20;

	// `apply-fx-destructively.schema.json` caps `itemGuids` on the way in and
	// `processedItemGuids` on the way out at 4096.
	inline constexpr std::size_t maximum_processed_items = 4096;

	// Field lengths the output schemas impose, used when a name REAPER gave us is
	// longer than the contract allows.
	inline constexpr std::size_t maximum_fx_name_length = 512;
	inline constexpr std::size_t maximum_fx_parameter_name_length = 512;
	inline constexpr std::size_t maximum_fx_identifier_length = 512;
	inline constexpr std::size_t maximum_fx_vendor_length = 256;
	inline constexpr std::size_t maximum_formatted_value_length = 256;

	// `fxEntry.name` and `fxParameter.name` both carry `minLength: 1`, so an FX REAPER
	// would not name still needs a name. Substituted rather than dropped, for the reason
	// `undo_manager.h` substitutes for an unnamed undo entry: an FX nobody can name is
	// still an FX on the producer's chain, and omitting it would remove the only
	// evidence it is there.
	inline constexpr std::string_view unnamed_fx{"unnamed FX"};
	inline constexpr std::string_view unnamed_fx_parameter{"unnamed parameter"};

	// ---------------------------------------------------------------------------
	// What the seam moves across it
	// ---------------------------------------------------------------------------

	// `fxEntry` from `mcp-tools/outputs/tool-output-defs.schema.json`.
	struct fx_chain_entry
	{
		int index = 0;
		std::string name;
		bool bypassed = false;

		// The schema makes this optional, because REAPER does not always answer. An FX
		// that is offline is neither processing nor loaded, which is why a parameter
		// read against one returns nothing useful and why it is not applied by
		// `apply_fx_destructively`.
		bool offline = false;
	};

	// `fxParameter` from the same file. The range travels with the value because it
	// differs between plugins, and it is the range `set_fx_parameter` is checked
	// against.
	struct fx_parameter
	{
		int index = 0;
		std::string name;
		double value = 0.0;
		double minimum_value = 0.0;
		double maximum_value = 1.0;

		// Absent when the plugin supplies none. The string to quote to the producer,
		// since a normalised number means nothing to them.
		std::optional<std::string> formatted_value;
	};

	// `installedFx` from `list-installed-fx.schema.json`.
	struct installed_fx
	{
		// The identifier `add_fx` and `create_bus` accept, exactly as REAPER gave it.
		// Never reconstructed or tidied: the prefix and spacing are part of how REAPER
		// resolves it.
		std::string identifier;

		std::string name;
		std::string vendor;

		// One of the output schema's `format` enum values. Passed through rather than
		// derived here — the host reads REAPER's own answer, and an unrecognised one
		// becomes `unknown` on that side of the seam.
		std::string format;

		bool is_instrument = false;
	};

	// ---------------------------------------------------------------------------
	// The seam
	// ---------------------------------------------------------------------------

	// Everything the FX tools need from REAPER, and nothing else.
	//
	// Note what is absent as much as what is present, following `RenderHost`. There is
	// no undo operation, because the undo block belongs to `undo_manager` and is opened
	// by the executor before a handler runs. There is no "run a REAPER action", because
	// ADR 0005 chose constrained tools over generic scripting and a hole that size would
	// let any decision leak to this side of the seam. There is no chain reorder, because
	// no tool in the contract reorders a chain. Each absence is a requirement that
	// cannot be violated through this interface.
	//
	// Every method addresses a track by its REAPER GUID rather than by `MediaTrack*`, so
	// this header stays free of the SDK and the suite can implement the interface with
	// no REAPER present.
	class fx_host
	{
	public:
		virtual ~fx_host() = default;

		// False when any REAPER function the FX path needs could not be resolved. A host
		// that is not usable reports empty chains and silently drops writes, so every
		// handler checks this first and fails the action with a reason rather than
		// telling the producer a change landed.
		virtual bool is_usable() const = 0;

		// The functions REAPER did not supply, for the reason that accompanies an
		// unusable host.
		virtual std::vector<std::string> unresolved_function_names() const = 0;

		// --- reads ---

		// The track's chain in chain order. Empty for a track with no FX, which is a
		// normal answer and not a failure.
		virtual std::vector<fx_chain_entry> read_track_fx(const std::string& track_guid) = 0;

		// The FX's parameters in the plugin's own order. Empty for a plugin exposing
		// none, which some analysers do.
		virtual std::vector<fx_parameter> read_fx_parameters(
			const std::string& track_guid,
			int fx_index) = 0;

		// Every FX installed on this machine. Filtering and ranking happen on this side
		// of the seam; the host does the enumeration and nothing else.
		virtual std::vector<installed_fx> read_installed_fx() = 0;

		// The GUIDs of the media items on the track, in timeline order.
		virtual std::vector<std::string> read_track_item_guids(const std::string& track_guid) = 0;

		// --- mutations ---

		// The chain position the FX landed at, or nothing when REAPER would not load it
		// — which for an identifier that is not installed is the answer, not a failure
		// to report as one. `chain_position` empty appends.
		virtual std::optional<int> insert_fx(
			const std::string& track_guid,
			const std::string& fx_identifier,
			std::optional<int> chain_position) = 0;

		virtual bool delete_fx(const std::string& track_guid, int fx_index) = 0;

		virtual bool set_fx_bypassed(const std::string& track_guid, int fx_index, bool bypassed) = 0;

		// Addressed by parameter *index*, because that is what REAPER's setter takes.
		// The name the call supplied is matched to an index on this side of the seam,
		// against the same read `get_fx_parameters` returns.
		virtual bool set_fx_parameter_value(
			const std::string& track_guid,
			int fx_index,
			int parameter_index,
			double value) = 0;

		// Renders the track's chain into one item's audio. Per item rather than per
		// track, so a failure names the item it belongs to — which is what makes
		// requirement 9.4's per-action reporting possible for this tool.
		virtual bool apply_fx_to_item(
			const std::string& item_guid,
			bool replace_existing_take) = 0;
	};

	// ---------------------------------------------------------------------------
	// Requests
	//
	// One per tool, mirroring the input schemas. The acknowledgement fields are absent
	// throughout: no FX tool can refuse, so there is nothing for the producer to
	// acknowledge. `confirmedTrackSelection` is read by target resolution, which has
	// already run.
	// ---------------------------------------------------------------------------

	struct add_fx_request
	{
		std::string fx_identifier;

		// Absent appends. A position past the end of the chain is an invalid call rather
		// than an append, because appending instead would put the FX somewhere other
		// than asked and chain order changes the sound.
		std::optional<int> chain_position;
	};

	struct remove_fx_request
	{
		int fx_index = 0;
	};

	struct set_fx_bypass_request
	{
		int fx_index = 0;
		bool bypassed = false;
	};

	struct set_fx_parameter_request
	{
		int fx_index = 0;
		std::string parameter_name;
		double value = 0.0;
	};

	struct get_fx_parameters_request
	{
		int fx_index = 0;
	};

	// `list_track_fx` takes nothing but the track, which target resolution already
	// carries. Declared anyway so the eight tools read alike and so a later field has
	// somewhere to go.
	struct list_track_fx_request
	{
	};

	struct list_installed_fx_request
	{
		std::string search_term;

		// Absent means the input schema's default of 20.
		std::optional<int> maximum_results;
	};

	struct apply_fx_destructively_request
	{
		// False preserves the original take by adding the processed audio alongside it,
		// which is the input schema's default and the recoverable direction.
		bool replace_existing_take = false;

		// Empty means every item on the track.
		std::vector<std::string> item_guids;
	};

	// ---------------------------------------------------------------------------
	// Results
	//
	// One per tool, field for field with the output schemas. `track` is the resolved
	// reference the executor already produced, and the undo report is attached by the
	// framework — a handler never sees one, which is what keeps a handler from
	// reporting a marker for a tool that should not have one.
	// ---------------------------------------------------------------------------

	struct add_fx_result
	{
		TrackReference track;
		int fx_index = 0;
		std::string fx_name;
		int chain_length = 1;
	};

	struct remove_fx_result
	{
		TrackReference track;
		int fx_index = 0;

		// Read before the removal, because it cannot be read after.
		std::string fx_name;

		int chain_length = 0;
	};

	struct set_fx_bypass_result
	{
		TrackReference track;
		int fx_index = 0;
		std::string fx_name;
		bool bypassed = false;

		// Equal to `bypassed` when the FX was already in the requested state. Carried so
		// the agent does not describe a no-op as a change.
		bool previous_bypassed = false;
	};

	struct set_fx_parameter_result
	{
		TrackReference track;
		int fx_index = 0;
		std::string fx_name;
		std::string parameter_name;

		// Read back from the plugin rather than assumed from the input.
		double value = 0.0;

		double previous_value = 0.0;

		// True when the plugin held the parameter somewhere other than where it was
		// asked to. Only reachable for a value that passed the range check — see the
		// file header.
		bool clamped = false;

		std::optional<std::string> formatted_value;
	};

	struct get_fx_parameters_result
	{
		TrackReference track;
		int fx_index = 0;
		std::string fx_name;
		std::vector<fx_parameter> parameters;

		// How many the FX exposes, before the cap. Equal to `parameters.size()` when
		// nothing was capped.
		std::size_t total_in_range = 0;

		bool truncated = false;
	};

	struct list_track_fx_result
	{
		TrackReference track;
		std::vector<fx_chain_entry> fx;
	};

	struct list_installed_fx_result
	{
		std::vector<installed_fx> matches;
		std::size_t total_in_range = 0;
		bool truncated = false;
	};

	struct apply_fx_destructively_result
	{
		TrackReference track;

		// The items whose audio now contains the chain, in timeline order.
		std::vector<std::string> processed_item_guids;

		bool replaced_existing_take = false;

		// How many FX from the chain were rendered in: neither bypassed nor offline.
		int applied_fx_count = 0;
	};

	// Either the tool's result, or the reason the action failed. A variant, so a caller
	// cannot read a populated result next to a populated failure — the same reasoning
	// `object_resolver.h` gives for its own outcomes.
	template <typename Result>
	using fx_outcome = std::variant<Result, action_error>;

	// ---------------------------------------------------------------------------
	// Shared checks
	// ---------------------------------------------------------------------------

	namespace detail
	{
		inline std::string bounded_fx_string(
			std::string_view value,
			std::string_view substitute_when_empty,
			std::size_t maximum_length)
		{
			std::string bounded{value.empty() ? substitute_when_empty : value};

			if (bounded.size() > maximum_length)
			{
				bounded.resize(maximum_length);
			}

			return bounded;
		}

		// A `maxLength` with no `minLength`: empty stays empty rather than gaining a
		// substitute, because an absent vendor is absent and inventing one would have
		// the agent telling the producer who made their FX.
		inline std::string bounded_optional_fx_string(std::string_view value, std::size_t maximum_length)
		{
			std::string bounded{value};

			if (bounded.size() > maximum_length)
			{
				bounded.resize(maximum_length);
			}

			return bounded;
		}

		inline std::string describe_index(int index)
		{
			return std::to_string(index);
		}

		// Lowercased for the loose match `list-installed-fx.schema.json` describes.
		// ASCII only and deliberately so: `std::tolower` under a non-C locale is
		// locale-dependent, and an FX name matching in one producer's locale and not
		// another's is worse than a match that ignores accents.
		inline std::string ascii_lowercased(std::string_view value)
		{
			std::string lowered;
			lowered.reserve(value.size());

			for (const char character : value)
			{
				if (character >= 'A' && character <= 'Z')
				{
					lowered.push_back(static_cast<char>(character - 'A' + 'a'));
					continue;
				}

				lowered.push_back(character);
			}

			return lowered;
		}
	}

	// An unusable host, as a reason rather than as a crash.
	//
	// Every handler's first check. A host missing a function reports an empty chain and
	// drops a write on the floor, so proceeding would tell the producer a change landed
	// when nothing did — and `is_usable` exists precisely so that is reportable.
	inline std::optional<action_error> check_fx_host_usable(const fx_host& host)
	{
		if (host.is_usable())
		{
			return std::nullopt;
		}

		std::string reason{
			"REAPER did not supply the functions the FX tools need, so no FX operation can run."};

		const std::vector<std::string> missing = host.unresolved_function_names();

		if (!missing.empty())
		{
			reason += " Missing:";

			for (const std::string& function_name : missing)
			{
				reason += ' ';
				reason += function_name;
			}

			reason += '.';
		}

		return action_error{fx_host_unavailable_code, reason};
	}

	// An FX index against the chain that was actually read.
	//
	// Checked against one read rather than against a count read separately, so the
	// chain cannot change between the check and the use.
	inline std::optional<action_error> check_fx_index_in_range(
		const std::vector<fx_chain_entry>& chain,
		int fx_index)
	{
		if (fx_index >= 0 && static_cast<std::size_t>(fx_index) < chain.size())
		{
			return std::nullopt;
		}

		if (chain.empty())
		{
			return action_error{
				fx_index_out_of_range_code,
				"this track has no FX on its chain, so there is nothing at index "
					+ detail::describe_index(fx_index)};
		}

		return action_error{
			fx_index_out_of_range_code,
			"this track's chain holds " + std::to_string(chain.size())
				+ " FX, so there is nothing at index " + detail::describe_index(fx_index)
				+ " — read the chain again, since an earlier insert or removal shifts every index above it"};
	}

	// A requested value against the range this parameter reported.
	//
	// Strictly outside the bounds fails; a value on a bound is inside it. A NaN fails
	// every comparison, so it is rejected by falling through, which is the refusing
	// direction — see the file header for why this is a failure and not a clamp.
	inline std::optional<action_error> check_parameter_value_in_range(
		const fx_parameter& parameter,
		double requested_value)
	{
		if (requested_value >= parameter.minimum_value && requested_value <= parameter.maximum_value)
		{
			return std::nullopt;
		}

		return action_error{
			fx_parameter_out_of_range_code,
			"\"" + parameter.name + "\" accepts values from "
				+ std::to_string(parameter.minimum_value) + " to "
				+ std::to_string(parameter.maximum_value)
				+ ", and the value asked for is outside that range — the parameter was not changed"};
	}

	// Whether the plugin held the parameter where it was asked to.
	//
	// A tolerance rather than equality: the value travels as a double through REAPER and
	// back, and reporting `clamped` on the last bit of a round trip would have the agent
	// warning the producer about a change they cannot hear. Scaled by the parameter's own
	// range, because a range of 0 to 1 and a range of -60 to 0 do not share a meaningful
	// absolute epsilon.
	inline bool parameter_value_was_held_elsewhere(
		const fx_parameter& parameter,
		double requested_value,
		double actual_value)
	{
		const double span = parameter.maximum_value - parameter.minimum_value;
		const double magnitude = std::abs(span) > 0.0 ? std::abs(span) : 1.0;
		const double tolerance = magnitude * 1e-9;

		// A NaN read-back is not a value that matches anything, and saying the plugin
		// moved the parameter is the honest report.
		if (std::isnan(actual_value) || std::isnan(requested_value))
		{
			return true;
		}

		return std::abs(actual_value - requested_value) > tolerance;
	}

	// ---------------------------------------------------------------------------
	// Shift-safe removal ordering
	// ---------------------------------------------------------------------------

	// One FX a removal addresses, resolved against a single read of the chain.
	//
	// The name is captured here and not looked up later, because after any removal the
	// index no longer names the same FX.
	struct fx_removal_target
	{
		int fx_index = 0;
		std::string fx_name;
	};

	// The order a set of indices must be removed in.
	//
	// Descending, and deduplicated. Descending because removing index 1 renumbers
	// everything above it, so ascending order has the second removal delete an FX the
	// caller never named — a bug that passes every single-element test and corrupts a
	// chain on the second. Deduplicated because the same index twice means the second
	// pass would delete whatever shifted down into that slot.
	//
	// Indices outside the chain are dropped here rather than reported, because
	// `plan_fx_removals`' callers check the range first and a plan is not the place to
	// carry a reason.
	inline std::vector<fx_removal_target> plan_fx_removals(
		const std::vector<fx_chain_entry>& chain,
		std::vector<int> fx_indices)
	{
		std::sort(fx_indices.begin(), fx_indices.end(), std::greater<int>{});
		fx_indices.erase(std::unique(fx_indices.begin(), fx_indices.end()), fx_indices.end());

		std::vector<fx_removal_target> plan;
		plan.reserve(fx_indices.size());

		for (const int fx_index : fx_indices)
		{
			if (fx_index < 0 || static_cast<std::size_t>(fx_index) >= chain.size())
			{
				continue;
			}

			plan.push_back(fx_removal_target{
				fx_index,
				detail::bounded_fx_string(
					chain[static_cast<std::size_t>(fx_index)].name,
					unnamed_fx,
					maximum_fx_name_length)});
		}

		return plan;
	}

	// ---------------------------------------------------------------------------
	// list_installed_fx — matching, ranking, and the cap
	// ---------------------------------------------------------------------------

	// How well one installed FX answers a search term. Lower is better; absent means it
	// does not match at all.
	//
	// The bands are the ones a producer's request distinguishes: they say "Pro-Q" and
	// mean the plugin whose name is that, not a plugin whose vendor string happens to
	// contain it. The identifier is matched last because its format prefix is the part
	// they never say, so a hit there is the weakest evidence.
	inline std::optional<int> rank_installed_fx_match(
		const installed_fx& candidate,
		std::string_view lowercased_search_term)
	{
		if (lowercased_search_term.empty())
		{
			return std::nullopt;
		}

		const std::string name = detail::ascii_lowercased(candidate.name);
		const std::string vendor = detail::ascii_lowercased(candidate.vendor);
		const std::string identifier = detail::ascii_lowercased(candidate.identifier);

		if (name == lowercased_search_term)
		{
			return 0;
		}

		if (name.rfind(lowercased_search_term, 0) == 0)
		{
			return 1;
		}

		if (name.find(lowercased_search_term) != std::string::npos)
		{
			return 2;
		}

		if (vendor.find(lowercased_search_term) != std::string::npos)
		{
			return 3;
		}

		if (identifier.find(lowercased_search_term) != std::string::npos)
		{
			return 4;
		}

		return std::nullopt;
	}

	// The input schema's `maximumResults`: default 20, floor 1, ceiling 100. A value
	// outside the bounds is brought inside rather than failed, because the server
	// validated the input against the authoritative schema before it arrived — this is
	// the belt to that braces, and refusing a read over a cap the producer never sees
	// would be a worse answer than reading fewer.
	inline std::size_t effective_installed_fx_cap(const std::optional<int>& maximum_results)
	{
		if (!maximum_results.has_value())
		{
			return default_installed_fx_matches;
		}

		if (*maximum_results < 1)
		{
			return 1;
		}

		if (static_cast<std::size_t>(*maximum_results) > maximum_installed_fx_matches)
		{
			return maximum_installed_fx_matches;
		}

		return static_cast<std::size_t>(*maximum_results);
	}

	// Requirement 23.9 for this tool: the matches, best first, capped — with the full
	// count carried so the cap is visible rather than a silent truncation the agent
	// reasons over as if it were the whole set.
	//
	// `std::stable_sort`, so equally good matches keep REAPER's own enumeration order.
	// A ranking that reordered them arbitrarily would make the same call return
	// different "best matches" on two machines with the same plugins installed.
	inline list_installed_fx_result match_installed_fx(
		const std::vector<installed_fx>& installed,
		const list_installed_fx_request& request)
	{
		const std::string search_term = detail::ascii_lowercased(request.search_term);

		std::vector<std::pair<int, installed_fx>> ranked;

		for (const installed_fx& candidate : installed)
		{
			const std::optional<int> rank = rank_installed_fx_match(candidate, search_term);

			if (!rank.has_value())
			{
				continue;
			}

			installed_fx bounded = candidate;
			bounded.identifier = detail::bounded_optional_fx_string(
				candidate.identifier,
				maximum_fx_identifier_length);
			bounded.name = detail::bounded_fx_string(candidate.name, unnamed_fx, maximum_fx_name_length);
			bounded.vendor = detail::bounded_optional_fx_string(candidate.vendor, maximum_fx_vendor_length);

			ranked.emplace_back(*rank, std::move(bounded));
		}

		std::stable_sort(
			ranked.begin(),
			ranked.end(),
			[](const std::pair<int, installed_fx>& left, const std::pair<int, installed_fx>& right) {
				return left.first < right.first;
			});

		list_installed_fx_result result;

		// The count before the cap, which is what `totalInRange` means.
		result.total_in_range = ranked.size();

		const std::size_t cap = effective_installed_fx_cap(request.maximum_results);
		const std::size_t returned = std::min(cap, ranked.size());

		result.matches.reserve(returned);

		for (std::size_t position = 0; position < returned; ++position)
		{
			result.matches.push_back(std::move(ranked[position].second));
		}

		result.truncated = result.total_in_range > result.matches.size();

		return result;
	}

	// ---------------------------------------------------------------------------
	// The eight handlers
	//
	// Each takes the resolved track and the request, and returns the tool's result or
	// the reason the action failed. No handler opens an undo block, captures a marker,
	// or builds a refusal: the first two belong to `undo_manager` through the executor,
	// and the third is not something an FX tool can produce.
	// ---------------------------------------------------------------------------

	inline fx_outcome<add_fx_result> apply_add_fx(
		fx_host& host,
		const TrackReference& track,
		const add_fx_request& request)
	{
		if (const std::optional<action_error> unusable = check_fx_host_usable(host))
		{
			return *unusable;
		}

		if (request.fx_identifier.empty())
		{
			return action_error{
				fx_identifier_not_installed_code,
				"no FX identifier was given — call list_installed_fx and pass an identifier it returned"};
		}

		const std::vector<fx_chain_entry> chain_before = host.read_track_fx(track.guid);

		// A position equal to the chain length appends, which is legitimate. Past that
		// is an invalid call: REAPER would append, and the FX would sit somewhere other
		// than where it was asked to go, which changes the sound.
		if (request.chain_position.has_value())
		{
			const int position = *request.chain_position;

			if (position < 0 || static_cast<std::size_t>(position) > chain_before.size())
			{
				return action_error{
					fx_chain_position_out_of_range_code,
					"this track's chain holds " + std::to_string(chain_before.size())
						+ " FX, so the furthest position an FX can be inserted at is "
						+ std::to_string(chain_before.size()) + ", and "
						+ detail::describe_index(position) + " is past the end of the chain"};
			}
		}

		const std::optional<int> landed_at =
			host.insert_fx(track.guid, request.fx_identifier, request.chain_position);

		if (!landed_at.has_value())
		{
			return action_error{
				fx_identifier_not_installed_code,
				"REAPER would not load \"" + request.fx_identifier
					+ "\" — the identifier must come from list_installed_fx on this machine, since the "
					  "producer's FX set is specific to it"};
		}

		const std::vector<fx_chain_entry> chain_after = host.read_track_fx(track.guid);

		if (chain_after.empty() || *landed_at < 0
			|| static_cast<std::size_t>(*landed_at) >= chain_after.size())
		{
			// REAPER reported an insert and then a chain that does not contain it. The
			// FX may well be there, so this does not claim the insert failed — it says
			// the result cannot be described, which is the honest report and leaves the
			// undo block to put it back.
			return action_error{
				fx_identifier_not_installed_code,
				"REAPER reported inserting \"" + request.fx_identifier
					+ "\" at chain position " + detail::describe_index(*landed_at)
					+ ", but the chain read back afterwards has no FX there"};
		}

		add_fx_result result;
		result.track = track;
		result.fx_index = *landed_at;
		result.fx_name = detail::bounded_fx_string(
			chain_after[static_cast<std::size_t>(*landed_at)].name,
			unnamed_fx,
			maximum_fx_name_length);
		result.chain_length = static_cast<int>(chain_after.size());

		return result;
	}

	inline fx_outcome<remove_fx_result> apply_remove_fx(
		fx_host& host,
		const TrackReference& track,
		const remove_fx_request& request)
	{
		if (const std::optional<action_error> unusable = check_fx_host_usable(host))
		{
			return *unusable;
		}

		const std::vector<fx_chain_entry> chain_before = host.read_track_fx(track.guid);

		if (const std::optional<action_error> out_of_range =
				check_fx_index_in_range(chain_before, request.fx_index))
		{
			return *out_of_range;
		}

		// Through the planner even for one index, so the one path is the shift-safe one.
		// The plan resolves every target's identity against `chain_before` and orders the
		// removals descending, which is what makes a longer plan correct rather than
		// accidentally correct at length one.
		const std::vector<fx_removal_target> plan = plan_fx_removals(chain_before, {request.fx_index});

		if (plan.empty())
		{
			return action_error{
				fx_index_out_of_range_code,
				"there is no FX at index " + detail::describe_index(request.fx_index)
					+ " on this track's chain"};
		}

		const fx_removal_target& target = plan.front();

		if (!host.delete_fx(track.guid, target.fx_index))
		{
			return action_error{
				fx_removal_failed_code,
				"REAPER would not remove \"" + target.fx_name + "\" from chain position "
					+ detail::describe_index(target.fx_index) + "; the chain is unchanged"};
		}

		remove_fx_result result;
		result.track = track;
		result.fx_index = target.fx_index;

		// The name read before the removal. There is nothing at that index to read now,
		// and the producer still needs told what went.
		result.fx_name = target.fx_name;
		result.chain_length = static_cast<int>(host.read_track_fx(track.guid).size());

		return result;
	}

	inline fx_outcome<set_fx_bypass_result> apply_set_fx_bypass(
		fx_host& host,
		const TrackReference& track,
		const set_fx_bypass_request& request)
	{
		if (const std::optional<action_error> unusable = check_fx_host_usable(host))
		{
			return *unusable;
		}

		const std::vector<fx_chain_entry> chain = host.read_track_fx(track.guid);

		if (const std::optional<action_error> out_of_range =
				check_fx_index_in_range(chain, request.fx_index))
		{
			return *out_of_range;
		}

		const fx_chain_entry& entry = chain[static_cast<std::size_t>(request.fx_index)];
		const bool previous_bypassed = entry.bypassed;

		// Written even when the FX is already in the requested state. REAPER records the
		// no-op as an undo entry either way, and a handler that skipped the write would
		// have to decide whether the block it is already inside should record nothing —
		// which is the executor's business, not this handler's. The result says it was a
		// no-op through `previousBypassed`, which is what that field is for.
		if (!host.set_fx_bypassed(track.guid, request.fx_index, request.bypassed))
		{
			return action_error{
				fx_bypass_write_failed_code,
				"REAPER would not change the bypass state of \"" + entry.name
					+ "\" at chain position " + detail::describe_index(request.fx_index)};
		}

		set_fx_bypass_result result;
		result.track = track;
		result.fx_index = request.fx_index;
		result.fx_name = detail::bounded_fx_string(entry.name, unnamed_fx, maximum_fx_name_length);
		result.bypassed = request.bypassed;
		result.previous_bypassed = previous_bypassed;

		return result;
	}

	inline fx_outcome<set_fx_parameter_result> apply_set_fx_parameter(
		fx_host& host,
		const TrackReference& track,
		const set_fx_parameter_request& request)
	{
		if (const std::optional<action_error> unusable = check_fx_host_usable(host))
		{
			return *unusable;
		}

		const std::vector<fx_chain_entry> chain = host.read_track_fx(track.guid);

		if (const std::optional<action_error> out_of_range =
				check_fx_index_in_range(chain, request.fx_index))
		{
			return *out_of_range;
		}

		const std::vector<fx_parameter> parameters =
			host.read_fx_parameters(track.guid, request.fx_index);

		const auto matched = std::find_if(
			parameters.begin(),
			parameters.end(),
			[&request](const fx_parameter& parameter) { return parameter.name == request.parameter_name; });

		if (matched == parameters.end())
		{
			return action_error{
				fx_parameter_not_found_code,
				"\"" + chain[static_cast<std::size_t>(request.fx_index)].name
					+ "\" exposes no parameter named \"" + request.parameter_name
					+ "\" — call get_fx_parameters and use a name exactly as it reported it"};
		}

		// The range check, before any write. Out of the range this FX reported is a call
		// built wrong, and clamping it would report success for a value the plugin never
		// held.
		if (const std::optional<action_error> out_of_range =
				check_parameter_value_in_range(*matched, request.value))
		{
			return *out_of_range;
		}

		const double previous_value = matched->value;

		if (!host.set_fx_parameter_value(track.guid, request.fx_index, matched->index, request.value))
		{
			return action_error{
				fx_parameter_write_failed_code,
				"REAPER would not set \"" + matched->name + "\" on \""
					+ chain[static_cast<std::size_t>(request.fx_index)].name
					+ "\"; the parameter still holds its previous value"};
		}

		// Read back rather than assumed. A stepped parameter quantises and a plugin can
		// hold a value inside its own reported range, so what landed is a question only
		// the plugin can answer.
		const std::vector<fx_parameter> parameters_after =
			host.read_fx_parameters(track.guid, request.fx_index);

		const auto matched_after = std::find_if(
			parameters_after.begin(),
			parameters_after.end(),
			[&request](const fx_parameter& parameter) { return parameter.name == request.parameter_name; });

		set_fx_parameter_result result;
		result.track = track;
		result.fx_index = request.fx_index;
		result.fx_name = detail::bounded_fx_string(
			chain[static_cast<std::size_t>(request.fx_index)].name,
			unnamed_fx,
			maximum_fx_name_length);
		result.parameter_name = detail::bounded_fx_string(
			matched->name,
			unnamed_fx_parameter,
			maximum_fx_parameter_name_length);
		result.previous_value = previous_value;

		if (matched_after == parameters_after.end())
		{
			// The write succeeded and the parameter has gone. Nothing useful can be said
			// about what it holds, and the output schema requires a value — so this is a
			// failed action rather than a success reporting the number that was asked
			// for, which is the one thing the schema says not to do.
			return action_error{
				fx_parameter_write_failed_code,
				"\"" + request.parameter_name
					+ "\" could not be read back after being set, so the value it now holds is unknown"};
		}

		result.value = matched_after->value;
		result.clamped = parameter_value_was_held_elsewhere(*matched, request.value, matched_after->value);

		if (matched_after->formatted_value.has_value())
		{
			result.formatted_value = detail::bounded_optional_fx_string(
				*matched_after->formatted_value,
				maximum_formatted_value_length);
		}

		return result;
	}

	inline fx_outcome<get_fx_parameters_result> apply_get_fx_parameters(
		fx_host& host,
		const TrackReference& track,
		const get_fx_parameters_request& request)
	{
		if (const std::optional<action_error> unusable = check_fx_host_usable(host))
		{
			return *unusable;
		}

		const std::vector<fx_chain_entry> chain = host.read_track_fx(track.guid);

		if (const std::optional<action_error> out_of_range =
				check_fx_index_in_range(chain, request.fx_index))
		{
			return *out_of_range;
		}

		std::vector<fx_parameter> parameters = host.read_fx_parameters(track.guid, request.fx_index);

		get_fx_parameters_result result;
		result.track = track;
		result.fx_index = request.fx_index;
		result.fx_name = detail::bounded_fx_string(
			chain[static_cast<std::size_t>(request.fx_index)].name,
			unnamed_fx,
			maximum_fx_name_length);
		result.total_in_range = parameters.size();

		// Requirement 23.9. The cap is the schema's `maxItems`, and `truncated` is what
		// keeps it from being silent.
		if (parameters.size() > maximum_reported_fx_parameters)
		{
			parameters.resize(maximum_reported_fx_parameters);
		}

		for (fx_parameter& parameter : parameters)
		{
			parameter.name = detail::bounded_fx_string(
				parameter.name,
				unnamed_fx_parameter,
				maximum_fx_parameter_name_length);

			if (parameter.formatted_value.has_value())
			{
				parameter.formatted_value = detail::bounded_optional_fx_string(
					*parameter.formatted_value,
					maximum_formatted_value_length);
			}
		}

		result.parameters = std::move(parameters);
		result.truncated = result.total_in_range > result.parameters.size();

		return result;
	}

	inline fx_outcome<list_track_fx_result> apply_list_track_fx(
		fx_host& host,
		const TrackReference& track,
		const list_track_fx_request&)
	{
		if (const std::optional<action_error> unusable = check_fx_host_usable(host))
		{
			return *unusable;
		}

		std::vector<fx_chain_entry> chain = host.read_track_fx(track.guid);

		// Requirement 23.9, and the one of the three reads where the output schema gives
		// nothing to say the list was cut. Capping silently is what 23.9 forbids, so a
		// chain longer than the contract can carry is a failed action naming the length.
		if (chain.size() > maximum_reported_chain_length)
		{
			return action_error{
				fx_chain_too_long_to_report_code,
				"this track's chain holds " + std::to_string(chain.size())
					+ " FX, and a result can carry at most "
					+ std::to_string(maximum_reported_chain_length)
					+ " — the chain cannot be reported without leaving some out, which would be "
					  "indistinguishable from a shorter chain"};
		}

		for (fx_chain_entry& entry : chain)
		{
			entry.name = detail::bounded_fx_string(entry.name, unnamed_fx, maximum_fx_name_length);
		}

		list_track_fx_result result;
		result.track = track;
		result.fx = std::move(chain);

		return result;
	}

	inline fx_outcome<list_installed_fx_result> apply_list_installed_fx(
		fx_host& host,
		const list_installed_fx_request& request)
	{
		if (const std::optional<action_error> unusable = check_fx_host_usable(host))
		{
			return *unusable;
		}

		// An empty match list is a real answer meaning the FX is not installed, and the
		// output schema says so. Not a failure, so nothing is checked about the term
		// beyond what `rank_installed_fx_match` does with an empty one.
		return match_installed_fx(host.read_installed_fx(), request);
	}

	// The per-item outcomes `apply_fx_destructively` produces, plus the result it would
	// report if every item landed.
	//
	// Both, because the caller has to choose between the two framework shapes: a success
	// carrying the tool's fields when nothing failed, and a partial carrying the per-item
	// outcomes when something did. The result's `processed_item_guids` and the succeeded
	// outcomes' targets name the same items either way, which is what keeps a partial as
	// informative as the success would have been.
	struct apply_fx_destructively_outcomes
	{
		std::vector<action_outcome> actions;
		apply_fx_destructively_result result;

		bool every_item_landed() const { return count_failed_actions(actions) == 0; }
	};

	// How many FX on the chain would actually be rendered in.
	//
	// Neither bypassed nor offline. A bypassed FX is not processing, and a producer who
	// bypassed a plugin to audition without it would not expect it baked in; an offline
	// FX REAPER has not loaded at all. Counting either would have the result claim FX
	// were applied that were not.
	inline int count_applicable_fx(const std::vector<fx_chain_entry>& chain)
	{
		int applicable = 0;

		for (const fx_chain_entry& entry : chain)
		{
			if (entry.bypassed || entry.offline)
			{
				continue;
			}

			++applicable;
		}

		return applicable;
	}

	inline std::variant<apply_fx_destructively_outcomes, action_error> apply_fx_destructively(
		fx_host& host,
		const TrackReference& track,
		const apply_fx_destructively_request& request)
	{
		if (const std::optional<action_error> unusable = check_fx_host_usable(host))
		{
			return *unusable;
		}

		const std::vector<fx_chain_entry> chain = host.read_track_fx(track.guid);
		const std::vector<std::string> items_on_track = host.read_track_item_guids(track.guid);

		// Every item on the track when the call named none, which is what the input
		// schema says omitting `itemGuids` means.
		std::vector<std::string> requested_items =
			request.item_guids.empty() ? items_on_track : request.item_guids;

		if (requested_items.empty())
		{
			return action_error{
				fx_item_not_on_track_code,
				"this track has no media items, so there is no audio for the FX chain to be rendered into"};
		}

		if (requested_items.size() > maximum_processed_items)
		{
			return action_error{
				fx_too_many_items_code,
				"this call names " + std::to_string(requested_items.size())
					+ " items and a result can carry at most " + std::to_string(maximum_processed_items)
					+ " — nothing was applied, since a destructive operation that cannot report what it "
					  "touched is worse than one that did not run"};
		}

		apply_fx_destructively_outcomes outcomes;
		outcomes.result.track = track;
		outcomes.result.applied_fx_count = count_applicable_fx(chain);

		// The same value every item below is applied with, which is what makes it a report
		// of what was done rather than a restatement of the input — the output schema is
		// explicit that it is not a restatement, and the thing that makes that true is
		// that this variable and the one handed to `apply_fx_to_item` are the same
		// variable. If a future change gave different items different routes, this would
		// have to move onto the per-item outcome, because one flag could no longer
		// describe the call.
		outcomes.result.replaced_existing_take = request.replace_existing_take;

		// Timeline order, which is the order `read_track_item_guids` returns and the
		// order the output schema asks for. An item the call named that is not on the
		// track fails rather than being applied to, because a GUID from another track
		// names audio the producer did not ask to have rewritten — and this one is not
		// reversible in the way a parameter change is.
		for (const std::string& item_guid : requested_items)
		{
			const bool item_is_on_track =
				std::find(items_on_track.begin(), items_on_track.end(), item_guid) != items_on_track.end();

			if (!item_is_on_track)
			{
				outcomes.actions.push_back(failed_action(
					item_guid,
					fx_item_not_on_track_code,
					"this item is not on the track the call named, so its audio was left alone"));

				continue;
			}

			if (!host.apply_fx_to_item(item_guid, request.replace_existing_take))
			{
				outcomes.actions.push_back(failed_action(
					item_guid,
					fx_apply_failed_code,
					"REAPER would not render the FX chain into this item's audio; the item is unchanged"));

				continue;
			}

			outcomes.actions.push_back(succeeded_action(item_guid));
			outcomes.result.processed_item_guids.push_back(item_guid);
		}

		return outcomes;
	}

	// ---------------------------------------------------------------------------
	// Registration
	//
	// Through `tool_executor.h`'s seam: `register_read_tool` for the three reads,
	// `register_mutating_tool` for the five that change something. No mutating tool
	// supplies a precondition check, because none of them can refuse — see the file
	// header.
	//
	// Templated on the payload type, on how a request is read out of the already
	// validated input, and on how a result becomes a payload. The framework holds the
	// input as an opaque pointer it never dereferences (requirement 4.4), so reading a
	// field is the codec's job and not this file's; injecting both directions is what
	// lets the suite drive all eight handlers with no JSON library present.
	//
	// `RequestReader` is called as `read(input, request) -> bool`, overloaded per
	// request type. False means the input could not be read, which becomes a failed
	// action rather than a crash — the server validated the input, so this is the belt
	// to that braces.
	//
	// `ResultWriter` is called as `write(result) -> JsonValue`, overloaded per result
	// type.
	// ---------------------------------------------------------------------------

	// What one registration did, so a caller can report a gap at startup rather than
	// discovering it one unknown-tool error at a time while a producer waits.
	struct fx_tool_registration
	{
		std::string tool_name;
		tool_registration_outcome outcome = tool_registration_outcome::registered;
	};

	namespace detail
	{
		// The framework's two shapes for "this went wrong", from one reason.
		//
		// A single failed action, which is the only shape that can carry a reason:
		// `handler_success` has nowhere to put one, and requirement 9.5 says the reason
		// is not optional.
		inline handler_action_outcomes failed_fx_action(
			std::string_view target,
			const action_error& reason)
		{
			handler_action_outcomes outcomes;
			outcomes.actions.push_back(failed_action(target, reason.code(), reason.message()));

			return outcomes;
		}

		// The track a single-target FX tool addresses.
		//
		// Target resolution runs before a handler does and refuses a selector it cannot
		// resolve, so an empty list here means the call named no track at all — which
		// the input schemas make impossible, and which is therefore reported rather than
		// worked around.
		template <typename JsonValue>
		std::optional<TrackReference> first_resolved_track(
			const tool_execution_context<JsonValue>& context)
		{
			if (context.resolved_targets.empty())
			{
				return std::nullopt;
			}

			return context.resolved_targets.front().reference;
		}

		inline action_error no_resolved_track_error()
		{
			return action_error{
				fx_track_not_resolved_code,
				"this call named no track, and every FX tool addresses one"};
		}

		inline action_error unreadable_input_error(std::string_view tool_name)
		{
			return action_error{
				fx_input_unreadable_code,
				"the input for " + std::string{tool_name} + " could not be read into the shape the "
					"handler expects, so nothing was attempted"};
		}

		// Reads one tool's request out of the call's validated input.
		//
		// A null input is the same answer as an unreadable one: the framework holds the
		// payload as a pointer it never dereferences, and a handler that dereferenced a
		// null one would crash REAPER rather than report anything. The server validated
		// the input against the authoritative schema before it arrived, so reaching
		// either branch means something upstream is wrong — which is exactly the case
		// worth reporting rather than assuming away.
		template <typename JsonValue, typename RequestReader, typename Request>
		bool read_fx_request(
			const tool_execution_context<JsonValue>& context,
			const RequestReader& read_request,
			Request& request)
		{
			if (context.call.validated_input == nullptr)
			{
				return false;
			}

			return read_request(*context.call.validated_input, request);
		}
	}


	// Registers all eight. The returned vector holds one entry per tool in registration
	// order, so a caller can log the ones that did not take.
	//
	// Written as eight explicit lambdas rather than one generic adapter over all of
	// them. A single parameterised registrar was tried and needed `decltype` on the
	// handler's own return type to name the shape it was building, which made a change
	// to any one tool a puzzle about template deduction across the other seven. The
	// repetition here is the readable trade: each block says which seam the tool
	// registers through, which handler runs, and which of the framework's shapes comes
	// back.
	template <typename JsonValue, typename RequestReader, typename ResultWriter>
	std::vector<fx_tool_registration> register_fx_tools(
		tool_handler_registry<JsonValue>& registry,
		fx_host& host,
		RequestReader read_request,
		ResultWriter write_result)
	{
		std::vector<fx_tool_registration> registrations;

		// --- the five mutating tools ---
		//
		// None supplies a precondition check. An FX tool has no refusal to produce, so a
		// check would have nothing to return but empty — see the file header.

		registrations.push_back(fx_tool_registration{
			std::string{add_fx_tool_name},
			registry.register_mutating_tool(
				add_fx_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					add_fx_request request;

					if (!detail::read_fx_request(context, read_request, request))
					{
						return detail::failed_fx_action(
							add_fx_tool_name,
							detail::unreadable_input_error(add_fx_tool_name));
					}

					const std::optional<TrackReference> track =
						detail::first_resolved_track<JsonValue>(context);

					if (!track.has_value())
					{
						return detail::failed_fx_action(
							add_fx_tool_name,
							detail::no_resolved_track_error());
					}

					fx_outcome<add_fx_result> outcome = apply_add_fx(host, *track, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_fx_action(add_fx_tool_name, *reason);
					}

					return handler_success<JsonValue>{write_result(std::get<add_fx_result>(outcome))};
				})});

		registrations.push_back(fx_tool_registration{
			std::string{remove_fx_tool_name},
			registry.register_mutating_tool(
				remove_fx_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					remove_fx_request request;

					if (!detail::read_fx_request(context, read_request, request))
					{
						return detail::failed_fx_action(
							remove_fx_tool_name,
							detail::unreadable_input_error(remove_fx_tool_name));
					}

					const std::optional<TrackReference> track =
						detail::first_resolved_track<JsonValue>(context);

					if (!track.has_value())
					{
						return detail::failed_fx_action(
							remove_fx_tool_name,
							detail::no_resolved_track_error());
					}

					fx_outcome<remove_fx_result> outcome = apply_remove_fx(host, *track, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_fx_action(remove_fx_tool_name, *reason);
					}

					return handler_success<JsonValue>{write_result(std::get<remove_fx_result>(outcome))};
				})});

		registrations.push_back(fx_tool_registration{
			std::string{set_fx_bypass_tool_name},
			registry.register_mutating_tool(
				set_fx_bypass_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					set_fx_bypass_request request;

					if (!detail::read_fx_request(context, read_request, request))
					{
						return detail::failed_fx_action(
							set_fx_bypass_tool_name,
							detail::unreadable_input_error(set_fx_bypass_tool_name));
					}

					const std::optional<TrackReference> track =
						detail::first_resolved_track<JsonValue>(context);

					if (!track.has_value())
					{
						return detail::failed_fx_action(
							set_fx_bypass_tool_name,
							detail::no_resolved_track_error());
					}

					fx_outcome<set_fx_bypass_result> outcome =
						apply_set_fx_bypass(host, *track, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_fx_action(set_fx_bypass_tool_name, *reason);
					}

					return handler_success<JsonValue>{
						write_result(std::get<set_fx_bypass_result>(outcome))};
				})});

		registrations.push_back(fx_tool_registration{
			std::string{set_fx_parameter_tool_name},
			registry.register_mutating_tool(
				set_fx_parameter_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					set_fx_parameter_request request;

					if (!detail::read_fx_request(context, read_request, request))
					{
						return detail::failed_fx_action(
							set_fx_parameter_tool_name,
							detail::unreadable_input_error(set_fx_parameter_tool_name));
					}

					const std::optional<TrackReference> track =
						detail::first_resolved_track<JsonValue>(context);

					if (!track.has_value())
					{
						return detail::failed_fx_action(
							set_fx_parameter_tool_name,
							detail::no_resolved_track_error());
					}

					fx_outcome<set_fx_parameter_result> outcome =
						apply_set_fx_parameter(host, *track, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_fx_action(set_fx_parameter_tool_name, *reason);
					}

					return handler_success<JsonValue>{
						write_result(std::get<set_fx_parameter_result>(outcome))};
				})});

		// The dangerous one, and the only FX tool with per-action outcomes: a partial
		// when any item failed, a success carrying the tool's fields when none did.
		registrations.push_back(fx_tool_registration{
			std::string{apply_fx_destructively_tool_name},
			registry.register_mutating_tool(
				apply_fx_destructively_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					apply_fx_destructively_request request;

					if (!detail::read_fx_request(context, read_request, request))
					{
						return detail::failed_fx_action(
							apply_fx_destructively_tool_name,
							detail::unreadable_input_error(apply_fx_destructively_tool_name));
					}

					const std::optional<TrackReference> track =
						detail::first_resolved_track<JsonValue>(context);

					if (!track.has_value())
					{
						return detail::failed_fx_action(
							apply_fx_destructively_tool_name,
							detail::no_resolved_track_error());
					}

					std::variant<apply_fx_destructively_outcomes, action_error> outcome =
						apply_fx_destructively(host, *track, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_fx_action(apply_fx_destructively_tool_name, *reason);
					}

					apply_fx_destructively_outcomes& outcomes =
						std::get<apply_fx_destructively_outcomes>(outcome);

					if (!outcomes.every_item_landed())
					{
						// Requirement 9.4: what landed stays landed, and each failure
						// names the item it belongs to. There is no rollback on this
						// path, and for this tool there could not be one — the take
						// audio has already been rewritten.
						return handler_action_outcomes{std::move(outcomes.actions)};
					}

					return handler_success<JsonValue>{write_result(outcomes.result)};
				})});

		// --- the three reads ---
		//
		// `register_read_tool`, so no undo block is opened and no marker is captured
		// (requirement 10.3). A read tool's body *may* refuse, because nothing was
		// opened for it — but none of these does, for the reason in the file header.

		registrations.push_back(fx_tool_registration{
			std::string{get_fx_parameters_tool_name},
			registry.register_read_tool(
				get_fx_parameters_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> non_mutating_handler_result<JsonValue> {
					get_fx_parameters_request request;

					if (!detail::read_fx_request(context, read_request, request))
					{
						return detail::failed_fx_action(
							get_fx_parameters_tool_name,
							detail::unreadable_input_error(get_fx_parameters_tool_name));
					}

					const std::optional<TrackReference> track =
						detail::first_resolved_track<JsonValue>(context);

					if (!track.has_value())
					{
						return detail::failed_fx_action(
							get_fx_parameters_tool_name,
							detail::no_resolved_track_error());
					}

					fx_outcome<get_fx_parameters_result> outcome =
						apply_get_fx_parameters(host, *track, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_fx_action(get_fx_parameters_tool_name, *reason);
					}

					return handler_success<JsonValue>{
						write_result(std::get<get_fx_parameters_result>(outcome))};
				})});

		registrations.push_back(fx_tool_registration{
			std::string{list_track_fx_tool_name},
			registry.register_read_tool(
				list_track_fx_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> non_mutating_handler_result<JsonValue> {
					list_track_fx_request request;

					if (!detail::read_fx_request(context, read_request, request))
					{
						return detail::failed_fx_action(
							list_track_fx_tool_name,
							detail::unreadable_input_error(list_track_fx_tool_name));
					}

					const std::optional<TrackReference> track =
						detail::first_resolved_track<JsonValue>(context);

					if (!track.has_value())
					{
						return detail::failed_fx_action(
							list_track_fx_tool_name,
							detail::no_resolved_track_error());
					}

					fx_outcome<list_track_fx_result> outcome =
						apply_list_track_fx(host, *track, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_fx_action(list_track_fx_tool_name, *reason);
					}

					return handler_success<JsonValue>{
						write_result(std::get<list_track_fx_result>(outcome))};
				})});

		// The one FX tool that addresses no track: the producer's installed set is a
		// property of the machine, not of a track.
		registrations.push_back(fx_tool_registration{
			std::string{list_installed_fx_tool_name},
			registry.register_read_tool(
				list_installed_fx_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> non_mutating_handler_result<JsonValue> {
					list_installed_fx_request request;

					if (!detail::read_fx_request(context, read_request, request))
					{
						return detail::failed_fx_action(
							list_installed_fx_tool_name,
							detail::unreadable_input_error(list_installed_fx_tool_name));
					}

					fx_outcome<list_installed_fx_result> outcome =
						apply_list_installed_fx(host, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_fx_action(list_installed_fx_tool_name, *reason);
					}

					return handler_success<JsonValue>{
						write_result(std::get<list_installed_fx_result>(outcome))};
				})});

		return registrations;
	}

	// How many tools `register_fx_tools` registers, so a caller checking completeness
	// does not have to count the blocks above.
	inline constexpr std::size_t fx_tool_count = 8;

	// Whether every registration took. False names a gap the extension can report at
	// startup, rather than discovering it one unknown-tool error at a time while a
	// producer waits.
	inline bool every_fx_tool_registered(const std::vector<fx_tool_registration>& registrations)
	{
		if (registrations.size() != fx_tool_count)
		{
			return false;
		}

		for (const fx_tool_registration& registration : registrations)
		{
			if (registration.outcome != tool_registration_outcome::registered)
			{
				return false;
			}
		}

		return true;
	}
}

#endif
