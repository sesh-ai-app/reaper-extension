// The eight FX tools (task 10.6, requirements 9.1 and 9.2).
//
// The suite is built around the four places these handlers can lie to a producer, since
// the rest is arithmetic:
//
//   - Reporting a parameter value the plugin never held. `set_fx_parameter` checks the
//     requested value against the range this FX reported and fails without writing when
//     it falls outside, so the tests assert both the failure *and* that the host was not
//     written to — a clamp that reported success would pass an assertion about the
//     result alone.
//   - Addressing the wrong FX after an index shifted. The multi-removal case is checked
//     against the chain that survives rather than against the result payload, and the
//     ascending-order bug is reproduced deliberately in one test so the descending order
//     is shown to be load-bearing rather than incidental.
//   - Truncating a read silently. Each of the three reads has a different answer
//     available to it, and each is asserted separately, including the one where the
//     output schema gives no field to say the list was cut and the read therefore fails.
//   - Claiming an FX was rendered into audio that was not. `appliedFxCount` excludes
//     bypassed and offline FX, and `replacedExistingTake` is asserted against what the
//     host was actually asked to do.
//
// The scripted host is what makes all of that reachable: it holds a mutable chain the
// handlers write to, counts the writes, and can be told to refuse any one operation —
// none of which can be staged inside REAPER.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/tools/fx_tools.h>

using sesh_ai::daw::action_error;
using sesh_ai::daw::action_failed;
using sesh_ai::daw::action_outcome;
using sesh_ai::daw::action_succeeded;
using sesh_ai::daw::action_target;
using sesh_ai::daw::action_was_applied;
using sesh_ai::daw::count_applied_actions;
using sesh_ai::daw::count_failed_actions;
using sesh_ai::daw::dispatch_outcome;
using sesh_ai::daw::handler_action_outcomes;
using sesh_ai::daw::handler_success;
using sesh_ai::daw::LearnedAliasLookup;
using sesh_ai::daw::NoLearnedAliases;
using sesh_ai::daw::ResolvableTrack;
using sesh_ai::daw::tool_call;
using sesh_ai::daw::tool_execution_context;
using sesh_ai::daw::tool_executor_of;
using sesh_ai::daw::tool_handler_registry;
using sesh_ai::daw::tool_partial_outcome;
using sesh_ai::daw::tool_refusal;
using sesh_ai::daw::tool_registration_outcome;
using sesh_ai::daw::tool_result;
using sesh_ai::daw::tool_success;
using sesh_ai::daw::tool_undo_effect;
using sesh_ai::daw::TrackGuidSelector;
using sesh_ai::daw::TrackListSource;
using sesh_ai::daw::TrackReference;
using sesh_ai::daw::TrackSelector;
using sesh_ai::daw::undo_manager;
using sesh_ai::daw::undo_stack;

using sesh_ai::daw::tools::add_fx_request;
using sesh_ai::daw::tools::add_fx_result;
using sesh_ai::daw::tools::add_fx_tool_name;
using sesh_ai::daw::tools::apply_add_fx;
using sesh_ai::daw::tools::apply_fx_destructively;
using sesh_ai::daw::tools::apply_fx_destructively_outcomes;
using sesh_ai::daw::tools::apply_fx_destructively_request;
using sesh_ai::daw::tools::apply_fx_destructively_result;
using sesh_ai::daw::tools::apply_fx_destructively_tool_name;
using sesh_ai::daw::tools::apply_get_fx_parameters;
using sesh_ai::daw::tools::apply_list_installed_fx;
using sesh_ai::daw::tools::apply_list_track_fx;
using sesh_ai::daw::tools::apply_remove_fx;
using sesh_ai::daw::tools::apply_set_fx_bypass;
using sesh_ai::daw::tools::apply_set_fx_parameter;
using sesh_ai::daw::tools::check_fx_host_usable;
using sesh_ai::daw::tools::check_fx_index_in_range;
using sesh_ai::daw::tools::check_parameter_value_in_range;
using sesh_ai::daw::tools::count_applicable_fx;
using sesh_ai::daw::tools::default_installed_fx_matches;
using sesh_ai::daw::tools::effective_installed_fx_cap;
using sesh_ai::daw::tools::every_fx_tool_registered;
using sesh_ai::daw::tools::fx_apply_failed_code;
using sesh_ai::daw::tools::fx_chain_entry;
using sesh_ai::daw::tools::fx_chain_position_out_of_range_code;
using sesh_ai::daw::tools::fx_chain_too_long_to_report_code;
using sesh_ai::daw::tools::fx_host;
using sesh_ai::daw::tools::fx_host_unavailable_code;
using sesh_ai::daw::tools::fx_identifier_not_installed_code;
using sesh_ai::daw::tools::fx_index_out_of_range_code;
using sesh_ai::daw::tools::fx_item_not_on_track_code;
using sesh_ai::daw::tools::fx_outcome;
using sesh_ai::daw::tools::fx_parameter;
using sesh_ai::daw::tools::fx_parameter_not_found_code;
using sesh_ai::daw::tools::fx_parameter_out_of_range_code;
using sesh_ai::daw::tools::fx_removal_target;
using sesh_ai::daw::tools::fx_tool_count;
using sesh_ai::daw::tools::fx_tool_registration;
using sesh_ai::daw::tools::get_fx_parameters_request;
using sesh_ai::daw::tools::get_fx_parameters_result;
using sesh_ai::daw::tools::get_fx_parameters_tool_name;
using sesh_ai::daw::tools::installed_fx;
using sesh_ai::daw::tools::list_installed_fx_request;
using sesh_ai::daw::tools::list_installed_fx_result;
using sesh_ai::daw::tools::list_installed_fx_tool_name;
using sesh_ai::daw::tools::list_track_fx_request;
using sesh_ai::daw::tools::list_track_fx_result;
using sesh_ai::daw::tools::list_track_fx_tool_name;
using sesh_ai::daw::tools::match_installed_fx;
using sesh_ai::daw::tools::maximum_installed_fx_matches;
using sesh_ai::daw::tools::maximum_reported_chain_length;
using sesh_ai::daw::tools::maximum_reported_fx_parameters;
using sesh_ai::daw::tools::parameter_value_was_held_elsewhere;
using sesh_ai::daw::tools::plan_fx_removals;
using sesh_ai::daw::tools::rank_installed_fx_match;
using sesh_ai::daw::tools::register_fx_tools;
using sesh_ai::daw::tools::remove_fx_request;
using sesh_ai::daw::tools::remove_fx_result;
using sesh_ai::daw::tools::remove_fx_tool_name;
using sesh_ai::daw::tools::set_fx_bypass_request;
using sesh_ai::daw::tools::set_fx_bypass_result;
using sesh_ai::daw::tools::set_fx_bypass_tool_name;
using sesh_ai::daw::tools::set_fx_parameter_request;
using sesh_ai::daw::tools::set_fx_parameter_result;
using sesh_ai::daw::tools::set_fx_parameter_tool_name;

namespace
{
	// ---------------------------------------------------------------------------
	// The scripted session
	// ---------------------------------------------------------------------------

	const std::string mix_bus_guid{"{00000000-0000-0000-0000-0000000000aa}"};
	const std::string mix_bus_name{"Drum Bus"};

	TrackReference scripted_track()
	{
		TrackReference track;
		track.guid = mix_bus_guid;
		track.name = mix_bus_name;

		return track;
	}

	// One FX on the scripted chain. Holds its own parameters, because a parameter read
	// has to follow the FX when an index shifts — which is the whole point of the
	// removal-ordering tests.
	struct scripted_fx
	{
		std::string name;
		bool bypassed = false;
		bool offline = false;
		std::vector<fx_parameter> parameters;
	};

	fx_parameter scripted_parameter(
		int index,
		std::string name,
		double value,
		double minimum_value,
		double maximum_value)
	{
		fx_parameter parameter;
		parameter.index = index;
		parameter.name = std::move(name);
		parameter.value = value;
		parameter.minimum_value = minimum_value;
		parameter.maximum_value = maximum_value;

		return parameter;
	}

	installed_fx scripted_installed(
		std::string identifier,
		std::string name,
		std::string vendor = {},
		std::string format = "vst3")
	{
		installed_fx candidate;
		candidate.identifier = std::move(identifier);
		candidate.name = std::move(name);
		candidate.vendor = std::move(vendor);
		candidate.format = std::move(format);

		return candidate;
	}

	// A scripted REAPER, counting everything the handlers do to it.
	//
	// The track GUID is accepted and ignored: there is one track in this session, and
	// making the host multi-track would add a lookup the handlers do not perform and
	// cannot get wrong. What the handlers *can* get wrong is which index on that one
	// chain they address, which is what this host is built to catch.
	class scripted_fx_host final : public fx_host
	{
	public:
		bool is_usable() const override { return usable; }

		std::vector<std::string> unresolved_function_names() const override
		{
			return missing_function_names;
		}

		std::vector<fx_chain_entry> read_track_fx(const std::string&) override
		{
			++chain_read_count;

			std::vector<fx_chain_entry> entries;
			entries.reserve(chain.size());

			for (std::size_t position = 0; position < chain.size(); ++position)
			{
				fx_chain_entry entry;
				entry.index = static_cast<int>(position);
				entry.name = chain[position].name;
				entry.bypassed = chain[position].bypassed;
				entry.offline = chain[position].offline;

				entries.push_back(std::move(entry));
			}

			return entries;
		}

		std::vector<fx_parameter> read_fx_parameters(const std::string&, int fx_index) override
		{
			if (fx_index < 0 || static_cast<std::size_t>(fx_index) >= chain.size())
			{
				return {};
			}

			if (parameter_vanishes_after_write && parameter_write_count > 0)
			{
				return {};
			}

			return chain[static_cast<std::size_t>(fx_index)].parameters;
		}

		std::vector<installed_fx> read_installed_fx() override { return installed; }

		std::vector<std::string> read_track_item_guids(const std::string&) override
		{
			return item_guids;
		}

		std::optional<int> insert_fx(
			const std::string&,
			const std::string& fx_identifier,
			std::optional<int> chain_position) override
		{
			++insert_call_count;

			if (refuse_insert)
			{
				return std::nullopt;
			}

			const auto known = std::find_if(
				installed.begin(),
				installed.end(),
				[&fx_identifier](const installed_fx& candidate) {
					return candidate.identifier == fx_identifier;
				});

			// An identifier the producer does not have is REAPER declining to load it,
			// which is the answer rather than an error to report as one.
			if (known == installed.end())
			{
				return std::nullopt;
			}

			scripted_fx inserted;
			inserted.name = known->name;

			const std::size_t position = chain_position.has_value()
				? std::min(static_cast<std::size_t>(*chain_position), chain.size())
				: chain.size();

			chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(position), std::move(inserted));

			return static_cast<int>(position);
		}

		bool delete_fx(const std::string&, int fx_index) override
		{
			++delete_call_count;

			if (refuse_delete || fx_index < 0
				|| static_cast<std::size_t>(fx_index) >= chain.size())
			{
				return false;
			}

			chain.erase(chain.begin() + static_cast<std::ptrdiff_t>(fx_index));

			return true;
		}

		bool set_fx_bypassed(const std::string&, int fx_index, bool bypassed) override
		{
			++bypass_write_count;

			if (refuse_bypass_write || fx_index < 0
				|| static_cast<std::size_t>(fx_index) >= chain.size())
			{
				return false;
			}

			chain[static_cast<std::size_t>(fx_index)].bypassed = bypassed;

			return true;
		}

		bool set_fx_parameter_value(
			const std::string&,
			int fx_index,
			int parameter_index,
			double value) override
		{
			++parameter_write_count;

			if (refuse_parameter_write || fx_index < 0
				|| static_cast<std::size_t>(fx_index) >= chain.size())
			{
				return false;
			}

			for (fx_parameter& parameter : chain[static_cast<std::size_t>(fx_index)].parameters)
			{
				if (parameter.index != parameter_index)
				{
					continue;
				}

				// A plugin that holds the parameter somewhere other than where it was
				// asked to: a stepped control, or a range it reports but no longer
				// honours. This is the case `clamped` exists for.
				parameter.value = parameter_lands_at.value_or(value);

				return true;
			}

			return false;
		}

		bool apply_fx_to_item(const std::string& item_guid, bool replace_existing_take) override
		{
			applied_item_guids.push_back(item_guid);
			applied_replace_flags.push_back(replace_existing_take);

			return std::find(items_that_refuse_apply.begin(), items_that_refuse_apply.end(), item_guid)
				== items_that_refuse_apply.end();
		}

		// --- the session ---

		bool usable = true;
		std::vector<std::string> missing_function_names;
		std::vector<scripted_fx> chain;
		std::vector<installed_fx> installed;
		std::vector<std::string> item_guids;

		// --- scripted refusals ---

		bool refuse_insert = false;
		bool refuse_delete = false;
		bool refuse_bypass_write = false;
		bool refuse_parameter_write = false;
		bool parameter_vanishes_after_write = false;
		std::optional<double> parameter_lands_at;
		std::vector<std::string> items_that_refuse_apply;

		// --- what was done to it ---

		int chain_read_count = 0;
		int insert_call_count = 0;
		int delete_call_count = 0;
		int bypass_write_count = 0;
		int parameter_write_count = 0;
		std::vector<std::string> applied_item_guids;
		std::vector<bool> applied_replace_flags;
	};

	// A chain of named FX with no parameters, for the tests that only care about order.
	std::vector<scripted_fx> chain_named(std::vector<std::string> names)
	{
		std::vector<scripted_fx> chain;
		chain.reserve(names.size());

		for (std::string& name : names)
		{
			scripted_fx entry;
			entry.name = std::move(name);
			chain.push_back(std::move(entry));
		}

		return chain;
	}

	std::vector<std::string> chain_names(const std::vector<scripted_fx>& chain)
	{
		std::vector<std::string> names;
		names.reserve(chain.size());

		for (const scripted_fx& entry : chain)
		{
			names.push_back(entry.name);
		}

		return names;
	}

	// --- reading an outcome ---

	// By value, deliberately. Every call site passes the handler's return straight in, so
	// the outcome is a temporary that dies at the end of the full expression — a
	// reference into it would dangle, and the first draft of this file did exactly that
	// and read garbage.
	template <typename Result>
	Result result_of(const fx_outcome<Result>& outcome)
	{
		REQUIRE(std::holds_alternative<Result>(outcome));

		return std::get<Result>(outcome);
	}

	template <typename Result>
	action_error failure_of(const fx_outcome<Result>& outcome)
	{
		REQUIRE(std::holds_alternative<action_error>(outcome));

		return std::get<action_error>(outcome);
	}

	std::string item_guid_for(char distinguishing_character)
	{
		std::string guid{"{00000000-0000-0000-0000-0000000000"};
		guid.push_back(distinguishing_character);
		guid.push_back(distinguishing_character);
		guid.push_back('}');

		return guid;
	}
}

// ---------------------------------------------------------------------------
// An unusable host fails every operation with a reason that names what is missing
// ---------------------------------------------------------------------------

TEST_CASE("an unusable FX host fails the action and names the functions REAPER did not supply", "[fx_tools]")
{
	scripted_fx_host host;
	host.usable = false;
	host.missing_function_names = {"TrackFX_AddByName", "TrackFX_GetParam"};

	const std::optional<action_error> reason = check_fx_host_usable(host);

	REQUIRE(reason.has_value());
	CHECK(reason->code() == std::string{fx_host_unavailable_code});
	CHECK(reason->message().find("TrackFX_AddByName") != std::string::npos);
	CHECK(reason->message().find("TrackFX_GetParam") != std::string::npos);
}

TEST_CASE("an unusable FX host does not let a mutation report success", "[fx_tools]")
{
	scripted_fx_host host;
	host.usable = false;
	host.chain = chain_named({"ReaComp"});
	host.installed = {scripted_installed("VST3: Pro-Q 3", "Pro-Q 3", "FabFilter")};

	add_fx_request add;
	add.fx_identifier = "VST3: Pro-Q 3";

	CHECK(failure_of(apply_add_fx(host, scripted_track(), add)).code()
		== std::string{fx_host_unavailable_code});

	remove_fx_request remove;
	remove.fx_index = 0;

	CHECK(failure_of(apply_remove_fx(host, scripted_track(), remove)).code()
		== std::string{fx_host_unavailable_code});

	set_fx_bypass_request bypass;
	bypass.fx_index = 0;
	bypass.bypassed = true;

	CHECK(failure_of(apply_set_fx_bypass(host, scripted_track(), bypass)).code()
		== std::string{fx_host_unavailable_code});

	// And nothing was written. A host that cannot be used has not been used.
	CHECK(host.insert_call_count == 0);
	CHECK(host.delete_call_count == 0);
	CHECK(host.bypass_write_count == 0);
	CHECK(host.chain.size() == 1);
}

// ---------------------------------------------------------------------------
// add_fx
// ---------------------------------------------------------------------------

TEST_CASE("add_fx appends to the end of the chain and reports where it landed", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ", "ReaComp"});
	host.installed = {scripted_installed("VST3: Pro-Q 3", "Pro-Q 3", "FabFilter")};

	add_fx_request request;
	request.fx_identifier = "VST3: Pro-Q 3";

	const fx_outcome<add_fx_result> outcome = apply_add_fx(host, scripted_track(), request);
	const add_fx_result& result = result_of(outcome);

	// The previous chain length, which the agent cannot know without being told.
	CHECK(result.fx_index == 2);
	CHECK(result.chain_length == 3);

	// The name REAPER reports on the chain, not the identifier that was passed in.
	CHECK(result.fx_name == "Pro-Q 3");
	CHECK(result.track.guid == mix_bus_guid);
	CHECK(chain_names(host.chain) == std::vector<std::string>{"ReaEQ", "ReaComp", "Pro-Q 3"});
}

TEST_CASE("add_fx inserts at the position asked for rather than at the end", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ", "ReaComp"});
	host.installed = {scripted_installed("VST3: Pro-Q 3", "Pro-Q 3", "FabFilter")};

	add_fx_request request;
	request.fx_identifier = "VST3: Pro-Q 3";
	request.chain_position = 0;

	const add_fx_result& result = result_of(apply_add_fx(host, scripted_track(), request));

	CHECK(result.fx_index == 0);
	CHECK(result.chain_length == 3);

	// Chain order changes the sound, so landing at the end when position 0 was asked for
	// is a different result and not a cosmetic one.
	CHECK(chain_names(host.chain) == std::vector<std::string>{"Pro-Q 3", "ReaEQ", "ReaComp"});
}

TEST_CASE("add_fx accepts the position one past the last FX as an append", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ", "ReaComp"});
	host.installed = {scripted_installed("VST3: Pro-Q 3", "Pro-Q 3")};

	add_fx_request request;
	request.fx_identifier = "VST3: Pro-Q 3";
	request.chain_position = 2;

	const add_fx_result& result = result_of(apply_add_fx(host, scripted_track(), request));

	CHECK(result.fx_index == 2);
	CHECK(chain_names(host.chain) == std::vector<std::string>{"ReaEQ", "ReaComp", "Pro-Q 3"});
}

TEST_CASE("add_fx fails a chain position past the end rather than appending", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ", "ReaComp"});
	host.installed = {scripted_installed("VST3: Pro-Q 3", "Pro-Q 3")};

	add_fx_request request;
	request.fx_identifier = "VST3: Pro-Q 3";
	request.chain_position = 7;

	const action_error& reason = failure_of(apply_add_fx(host, scripted_track(), request));

	CHECK(reason.code() == std::string{fx_chain_position_out_of_range_code});
	CHECK(reason.message().find('7') != std::string::npos);

	// Appending would have put the FX somewhere other than where it was asked to go, and
	// reported success for it.
	CHECK(host.insert_call_count == 0);
	CHECK(chain_names(host.chain) == std::vector<std::string>{"ReaEQ", "ReaComp"});
}

TEST_CASE("add_fx fails an identifier the producer does not have installed", "[fx_tools]")
{
	scripted_fx_host host;
	host.installed = {scripted_installed("VST3: Pro-Q 3", "Pro-Q 3")};

	add_fx_request request;
	request.fx_identifier = "VST3: Some Compressor The Agent Invented";

	const action_error& reason = failure_of(apply_add_fx(host, scripted_track(), request));

	CHECK(reason.code() == std::string{fx_identifier_not_installed_code});
	CHECK(reason.message().find("list_installed_fx") != std::string::npos);
	CHECK(host.chain.empty());
}

TEST_CASE("add_fx fails an empty identifier without asking REAPER", "[fx_tools]")
{
	scripted_fx_host host;

	add_fx_request request;

	CHECK(failure_of(apply_add_fx(host, scripted_track(), request)).code()
		== std::string{fx_identifier_not_installed_code});
	CHECK(host.insert_call_count == 0);
}

// ---------------------------------------------------------------------------
// remove_fx, and the index that shifts
// ---------------------------------------------------------------------------

TEST_CASE("remove_fx names the FX it removed and reports what is left", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ", "ReaComp", "ReaVerb"});

	remove_fx_request request;
	request.fx_index = 1;

	const remove_fx_result& result = result_of(apply_remove_fx(host, scripted_track(), request));

	CHECK(result.fx_index == 1);

	// Read before the removal, because after it there is nothing at index 1 to read a
	// name from — index 1 now holds ReaVerb.
	CHECK(result.fx_name == "ReaComp");
	CHECK(result.chain_length == 2);
	CHECK(chain_names(host.chain) == std::vector<std::string>{"ReaEQ", "ReaVerb"});
}

TEST_CASE("remove_fx fails an index past the end of the chain", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ", "ReaComp"});

	remove_fx_request request;
	request.fx_index = 4;

	const action_error& reason = failure_of(apply_remove_fx(host, scripted_track(), request));

	CHECK(reason.code() == std::string{fx_index_out_of_range_code});

	// The reason says how long the chain is and warns that an earlier edit shifts indices,
	// which is what the agent needs to recover rather than retry the same number.
	CHECK(reason.message().find('2') != std::string::npos);
	CHECK(host.delete_call_count == 0);
	CHECK(host.chain.size() == 2);
}

TEST_CASE("remove_fx on an empty chain says the chain is empty", "[fx_tools]")
{
	scripted_fx_host host;

	remove_fx_request request;
	request.fx_index = 0;

	const action_error& reason = failure_of(apply_remove_fx(host, scripted_track(), request));

	CHECK(reason.code() == std::string{fx_index_out_of_range_code});
	CHECK(reason.message().find("no FX") != std::string::npos);
}

TEST_CASE("remove_fx reports a removal REAPER declined as a failure, not a success", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ"});
	host.refuse_delete = true;

	remove_fx_request request;
	request.fx_index = 0;

	const action_error& reason = failure_of(apply_remove_fx(host, scripted_track(), request));

	CHECK(reason.message().find("ReaEQ") != std::string::npos);
	CHECK(host.chain.size() == 1);
}

TEST_CASE("a removal plan is ordered descending and deduplicated", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ", "ReaComp", "ReaVerb", "ReaDelay"});

	const std::vector<fx_removal_target> plan =
		plan_fx_removals(host.read_track_fx(mix_bus_guid), {1, 3, 1, 0});

	REQUIRE(plan.size() == 3);
	CHECK(plan[0].fx_index == 3);
	CHECK(plan[1].fx_index == 1);
	CHECK(plan[2].fx_index == 0);

	// Each target carries the name it had before anything moved, resolved against one
	// read of the chain.
	CHECK(plan[0].fx_name == "ReaDelay");
	CHECK(plan[1].fx_name == "ReaComp");
	CHECK(plan[2].fx_name == "ReaEQ");
}

TEST_CASE("a removal plan drops indices that are not on the chain", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ"});

	const std::vector<fx_removal_target> plan =
		plan_fx_removals(host.read_track_fx(mix_bus_guid), {0, 9, -1});

	REQUIRE(plan.size() == 1);
	CHECK(plan[0].fx_index == 0);
}

TEST_CASE("removing several FX in the planned order removes exactly the ones named", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ", "ReaComp", "ReaVerb", "ReaDelay"});

	// The producer asked for the compressor and the delay to go.
	const std::vector<fx_removal_target> plan =
		plan_fx_removals(host.read_track_fx(mix_bus_guid), {1, 3});

	for (const fx_removal_target& target : plan)
	{
		CHECK(host.delete_fx(mix_bus_guid, target.fx_index));
	}

	CHECK(chain_names(host.chain) == std::vector<std::string>{"ReaEQ", "ReaVerb"});
}

TEST_CASE("removing several FX in ascending order deletes an FX nobody named", "[fx_tools]")
{
	// This is the bug the descending order exists to prevent, reproduced so that the
	// ordering is shown to be load-bearing rather than incidental. The producer asked for
	// the compressor and the reverb — indices 1 and 2.
	//
	// Removing index 1 renumbers everything above it, so the second removal at index 2
	// addresses a chain one shorter than the one the caller read, and takes the delay
	// instead. Both removals "succeed", the result payload reports two FX gone, and one of
	// them is the wrong FX.
	const std::vector<std::string> before{"ReaEQ", "ReaComp", "ReaVerb", "ReaDelay"};

	scripted_fx_host ascending_host;
	ascending_host.chain = chain_named(before);

	for (const int fx_index : std::vector<int>{1, 2})
	{
		CHECK(ascending_host.delete_fx(mix_bus_guid, fx_index));
	}

	CHECK(chain_names(ascending_host.chain) == std::vector<std::string>{"ReaEQ", "ReaVerb"});

	// The same request through the planner, which orders the removals descending.
	scripted_fx_host planned_host;
	planned_host.chain = chain_named(before);

	for (const fx_removal_target& target :
		 plan_fx_removals(planned_host.read_track_fx(mix_bus_guid), {1, 2}))
	{
		CHECK(planned_host.delete_fx(mix_bus_guid, target.fx_index));
	}

	// The delay survives and the reverb is gone, which is what was asked for.
	CHECK(chain_names(planned_host.chain) == std::vector<std::string>{"ReaEQ", "ReaDelay"});

	// Stated directly: the two orders do not agree, so the descending one is the reason
	// the planned result is right.
	CHECK_FALSE(chain_names(ascending_host.chain) == chain_names(planned_host.chain));
}

// ---------------------------------------------------------------------------
// set_fx_bypass
// ---------------------------------------------------------------------------

TEST_CASE("set_fx_bypass reports the state before as well as after", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ", "ReaComp"});

	set_fx_bypass_request request;
	request.fx_index = 1;
	request.bypassed = true;

	const set_fx_bypass_result& result = result_of(apply_set_fx_bypass(host, scripted_track(), request));

	CHECK(result.fx_index == 1);
	CHECK(result.fx_name == "ReaComp");
	CHECK(result.bypassed == true);
	CHECK(result.previous_bypassed == false);
	CHECK(host.chain[1].bypassed == true);
}

TEST_CASE("set_fx_bypass on an FX already in that state reports equal states", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ"});
	host.chain[0].bypassed = true;

	set_fx_bypass_request request;
	request.fx_index = 0;
	request.bypassed = true;

	const set_fx_bypass_result& result = result_of(apply_set_fx_bypass(host, scripted_track(), request));

	// Equal, so the agent can tell the producer nothing changed rather than describing a
	// no-op as a change — which is usually how they find out they misread the chain.
	CHECK(result.previous_bypassed == result.bypassed);
}

TEST_CASE("set_fx_bypass fails an index past the end of the chain", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ"});

	set_fx_bypass_request request;
	request.fx_index = 3;
	request.bypassed = true;

	CHECK(failure_of(apply_set_fx_bypass(host, scripted_track(), request)).code()
		== std::string{fx_index_out_of_range_code});
	CHECK(host.bypass_write_count == 0);
}

// ---------------------------------------------------------------------------
// set_fx_parameter — the range is the plugin's, and outside it is a failure
// ---------------------------------------------------------------------------

namespace
{
	// A compressor whose threshold runs -60 to 0, which is the case the schema's own
	// description uses to explain why ranges must be read rather than assumed.
	scripted_fx_host host_with_threshold(double current_value = -12.0)
	{
		scripted_fx_host host;
		host.chain = chain_named({"ReaComp"});
		host.chain[0].parameters = {
			scripted_parameter(0, "Threshold", current_value, -60.0, 0.0),
			scripted_parameter(1, "Ratio", 4.0, 1.0, 20.0)};

		return host;
	}

	set_fx_parameter_request threshold_set_to(double value)
	{
		set_fx_parameter_request request;
		request.fx_index = 0;
		request.parameter_name = "Threshold";
		request.value = value;

		return request;
	}
}

TEST_CASE("set_fx_parameter reports the move rather than just the destination", "[fx_tools]")
{
	scripted_fx_host host = host_with_threshold(-12.0);

	const set_fx_parameter_result& result =
		result_of(apply_set_fx_parameter(host, scripted_track(), threshold_set_to(-24.0)));

	CHECK(result.parameter_name == "Threshold");
	CHECK(result.fx_name == "ReaComp");
	CHECK(result.previous_value == -12.0);
	CHECK(result.value == -24.0);
	CHECK(result.clamped == false);
	CHECK(host.chain[0].parameters[0].value == -24.0);
}

TEST_CASE("set_fx_parameter accepts a value on either bound of the reported range", "[fx_tools]")
{
	SECTION("the minimum")
	{
		scripted_fx_host host = host_with_threshold();

		const set_fx_parameter_result& result =
			result_of(apply_set_fx_parameter(host, scripted_track(), threshold_set_to(-60.0)));

		CHECK(result.value == -60.0);
		CHECK(result.clamped == false);
	}

	SECTION("the maximum")
	{
		scripted_fx_host host = host_with_threshold();

		const set_fx_parameter_result& result =
			result_of(apply_set_fx_parameter(host, scripted_track(), threshold_set_to(0.0)));

		CHECK(result.value == 0.0);
		CHECK(result.clamped == false);
	}
}

TEST_CASE("set_fx_parameter fails a value just outside the reported range without writing", "[fx_tools]")
{
	// Just outside, not wildly outside. A handler that clamped would look correct on a
	// value of -1000 and wrong here, because -60.001 is the value an agent reaches by
	// rounding rather than by inventing.
	SECTION("below the minimum")
	{
		scripted_fx_host host = host_with_threshold(-12.0);

		const action_error& reason =
			failure_of(apply_set_fx_parameter(host, scripted_track(), threshold_set_to(-60.001)));

		CHECK(reason.code() == std::string{fx_parameter_out_of_range_code});
		CHECK(reason.message().find("Threshold") != std::string::npos);

		// The assertion that separates failing from clamping: clamping would have written
		// -60 and reported success, and a test reading only the result could not tell.
		CHECK(host.parameter_write_count == 0);
		CHECK(host.chain[0].parameters[0].value == -12.0);
	}

	SECTION("above the maximum")
	{
		scripted_fx_host host = host_with_threshold(-12.0);

		const action_error& reason =
			failure_of(apply_set_fx_parameter(host, scripted_track(), threshold_set_to(0.001)));

		CHECK(reason.code() == std::string{fx_parameter_out_of_range_code});
		CHECK(host.parameter_write_count == 0);
		CHECK(host.chain[0].parameters[0].value == -12.0);
	}
}

TEST_CASE("set_fx_parameter fails a value that is not a number", "[fx_tools]")
{
	scripted_fx_host host = host_with_threshold();

	const double not_a_number = std::numeric_limits<double>::quiet_NaN();

	CHECK(failure_of(apply_set_fx_parameter(host, scripted_track(), threshold_set_to(not_a_number)))
			  .code()
		== std::string{fx_parameter_out_of_range_code});
	CHECK(host.parameter_write_count == 0);
}

TEST_CASE("set_fx_parameter reports clamped when the plugin held the value elsewhere", "[fx_tools]")
{
	// Inside the range the plugin reported, and the plugin still put it somewhere else —
	// a stepped control, or a range it no longer honours. This is what `clamped` is for,
	// and it is a success carrying the truth rather than a failure.
	scripted_fx_host host = host_with_threshold(-12.0);
	host.parameter_lands_at = -20.0;

	const set_fx_parameter_result& result =
		result_of(apply_set_fx_parameter(host, scripted_track(), threshold_set_to(-24.0)));

	CHECK(result.clamped == true);

	// Read back from the plugin, not echoed from the input. Reporting -24 here is the
	// failure the output schema's description is about.
	CHECK(result.value == -20.0);
	CHECK(result.previous_value == -12.0);
}

TEST_CASE("set_fx_parameter fails a parameter name the FX does not expose", "[fx_tools]")
{
	scripted_fx_host host = host_with_threshold();

	set_fx_parameter_request request;
	request.fx_index = 0;
	request.parameter_name = "Thresh";
	request.value = -20.0;

	const action_error& reason = failure_of(apply_set_fx_parameter(host, scripted_track(), request));

	CHECK(reason.code() == std::string{fx_parameter_not_found_code});
	CHECK(reason.message().find("get_fx_parameters") != std::string::npos);
	CHECK(host.parameter_write_count == 0);
}

TEST_CASE("set_fx_parameter fails when the value cannot be read back after the write", "[fx_tools]")
{
	scripted_fx_host host = host_with_threshold();
	host.parameter_vanishes_after_write = true;

	// The write landed and the parameter has gone, so what it holds is unknown. Reporting
	// the number that was asked for would be the one thing the output schema says not to
	// do.
	const action_error& reason =
		failure_of(apply_set_fx_parameter(host, scripted_track(), threshold_set_to(-24.0)));

	CHECK(reason.message().find("unknown") != std::string::npos);
}

TEST_CASE("a value on a bound is inside the range and a value beyond it is not", "[fx_tools]")
{
	const fx_parameter parameter = scripted_parameter(0, "Threshold", -12.0, -60.0, 0.0);

	CHECK(check_parameter_value_in_range(parameter, -60.0).has_value() == false);
	CHECK(check_parameter_value_in_range(parameter, 0.0).has_value() == false);
	CHECK(check_parameter_value_in_range(parameter, -30.0).has_value() == false);

	CHECK(check_parameter_value_in_range(parameter, -60.000001).has_value());
	CHECK(check_parameter_value_in_range(parameter, 0.000001).has_value());
	CHECK(check_parameter_value_in_range(parameter, std::numeric_limits<double>::infinity())
			  .has_value());
}

TEST_CASE("a round trip through the same value is not reported as clamped", "[fx_tools]")
{
	const fx_parameter parameter = scripted_parameter(0, "Threshold", -12.0, -60.0, 0.0);

	// The tolerance is what stops the agent warning a producer about a change they cannot
	// hear.
	CHECK(parameter_value_was_held_elsewhere(parameter, -24.0, -24.0) == false);
	CHECK(parameter_value_was_held_elsewhere(parameter, -24.0, -24.0 + 1e-15) == false);
	CHECK(parameter_value_was_held_elsewhere(parameter, -24.0, -23.9) == true);
}

// ---------------------------------------------------------------------------
// get_fx_parameters
// ---------------------------------------------------------------------------

TEST_CASE("get_fx_parameters reports each parameter with the range it accepts", "[fx_tools]")
{
	scripted_fx_host host = host_with_threshold(-18.0);

	get_fx_parameters_request request;
	request.fx_index = 0;

	const get_fx_parameters_result& result =
		result_of(apply_get_fx_parameters(host, scripted_track(), request));

	CHECK(result.fx_name == "ReaComp");
	REQUIRE(result.parameters.size() == 2);
	CHECK(result.parameters[0].name == "Threshold");
	CHECK(result.parameters[0].value == -18.0);
	CHECK(result.parameters[0].minimum_value == -60.0);
	CHECK(result.parameters[0].maximum_value == 0.0);
	CHECK(result.total_in_range == 2);
	CHECK(result.truncated == false);
}

TEST_CASE("get_fx_parameters reports a plugin that exposes none as an empty list", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"Spectrum Analyser"});

	get_fx_parameters_request request;

	const get_fx_parameters_result& result =
		result_of(apply_get_fx_parameters(host, scripted_track(), request));

	// Empty is a normal answer for an analyser, not a failure.
	CHECK(result.parameters.empty());
	CHECK(result.total_in_range == 0);
	CHECK(result.truncated == false);
}

TEST_CASE("get_fx_parameters caps a very large parameter set and says it did", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"Modular Everything"});

	const std::size_t exposed = maximum_reported_fx_parameters + 25;

	for (std::size_t index = 0; index < exposed; ++index)
	{
		host.chain[0].parameters.push_back(scripted_parameter(
			static_cast<int>(index),
			"Parameter " + std::to_string(index),
			0.5,
			0.0,
			1.0));
	}

	get_fx_parameters_request request;

	const get_fx_parameters_result& result =
		result_of(apply_get_fx_parameters(host, scripted_track(), request));

	// Requirement 23.9: capped, and the cap is visible rather than a silent truncation the
	// agent reasons over as if it were the whole set.
	CHECK(result.parameters.size() == maximum_reported_fx_parameters);
	CHECK(result.total_in_range == exposed);
	CHECK(result.truncated == true);
}

TEST_CASE("get_fx_parameters fails an index past the end of the chain", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ"});

	get_fx_parameters_request request;
	request.fx_index = 2;

	CHECK(failure_of(apply_get_fx_parameters(host, scripted_track(), request)).code()
		== std::string{fx_index_out_of_range_code});
}

// ---------------------------------------------------------------------------
// list_track_fx
// ---------------------------------------------------------------------------

TEST_CASE("list_track_fx reports the chain in chain order with its bypass states", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ", "ReaComp", "ReaVerb"});
	host.chain[1].bypassed = true;
	host.chain[2].offline = true;

	const list_track_fx_result& result =
		result_of(apply_list_track_fx(host, scripted_track(), list_track_fx_request{}));

	REQUIRE(result.fx.size() == 3);

	// Chain order, not a sort of anyone's choosing: the index is the prerequisite for
	// every other FX tool.
	CHECK(result.fx[0].index == 0);
	CHECK(result.fx[0].name == "ReaEQ");
	CHECK(result.fx[1].index == 1);
	CHECK(result.fx[1].bypassed == true);
	CHECK(result.fx[2].offline == true);
	CHECK(result.track.name == mix_bus_name);
}

TEST_CASE("list_track_fx reports an empty chain as empty", "[fx_tools]")
{
	scripted_fx_host host;

	const list_track_fx_result& result =
		result_of(apply_list_track_fx(host, scripted_track(), list_track_fx_request{}));

	CHECK(result.fx.empty());
}

TEST_CASE("list_track_fx fails rather than silently cutting a chain it cannot report", "[fx_tools]")
{
	scripted_fx_host host;

	std::vector<std::string> names;

	for (std::size_t index = 0; index <= maximum_reported_chain_length; ++index)
	{
		names.push_back("FX " + std::to_string(index));
	}

	host.chain = chain_named(std::move(names));

	const action_error& reason =
		failure_of(apply_list_track_fx(host, scripted_track(), list_track_fx_request{}));

	// This read's output schema carries no `truncated` field, so a cut list would be
	// indistinguishable from a shorter chain — which is exactly what requirement 23.9
	// forbids.
	CHECK(reason.code() == std::string{fx_chain_too_long_to_report_code});
	CHECK(reason.message().find(std::to_string(maximum_reported_chain_length)) != std::string::npos);
}

// ---------------------------------------------------------------------------
// list_installed_fx
// ---------------------------------------------------------------------------

namespace
{
	scripted_fx_host host_with_installed_set()
	{
		scripted_fx_host host;
		host.installed = {
			scripted_installed("VST3: Pro-Q 3 (FabFilter)", "Pro-Q 3", "FabFilter"),
			scripted_installed("VST3: Pro-C 2 (FabFilter)", "Pro-C 2", "FabFilter"),
			scripted_installed("VST: ReaComp (Cockos)", "ReaComp", "Cockos"),
			scripted_installed("JS: Tape Saturation", "Tape Saturation", "Cockos"),
			scripted_installed("AU: Compressor", "Compressor", "Apple")};

		return host;
	}
}

TEST_CASE("list_installed_fx returns an exact name match before a partial one", "[fx_tools]")
{
	scripted_fx_host host = host_with_installed_set();

	list_installed_fx_request request;
	request.search_term = "compressor";

	const list_installed_fx_result& result = result_of(apply_list_installed_fx(host, request));

	REQUIRE(result.matches.size() >= 1);

	// "Compressor" matches the Apple one exactly; ReaComp does not match at all, and a
	// vendor hit would rank below a name hit.
	CHECK(result.matches[0].name == "Compressor");
	CHECK(result.truncated == false);
}

TEST_CASE("list_installed_fx matches a vendor as well as a name, ranked lower", "[fx_tools]")
{
	scripted_fx_host host = host_with_installed_set();

	list_installed_fx_request request;
	request.search_term = "fabfilter";

	const list_installed_fx_result& result = result_of(apply_list_installed_fx(host, request));

	REQUIRE(result.matches.size() == 2);

	// Equally good matches keep REAPER's own enumeration order, so the same call answers
	// the same way on two machines with the same FX installed.
	CHECK(result.matches[0].name == "Pro-Q 3");
	CHECK(result.matches[1].name == "Pro-C 2");
	CHECK(result.total_in_range == 2);
}

TEST_CASE("list_installed_fx returns the identifier exactly as REAPER gave it", "[fx_tools]")
{
	scripted_fx_host host = host_with_installed_set();

	list_installed_fx_request request;
	request.search_term = "Pro-Q";

	const list_installed_fx_result& result = result_of(apply_list_installed_fx(host, request));

	REQUIRE(result.matches.size() == 1);

	// The prefix and spacing are part of how REAPER resolves it, so it is not tidied up.
	CHECK(result.matches[0].identifier == "VST3: Pro-Q 3 (FabFilter)");
	CHECK(result.matches[0].name == "Pro-Q 3");
}

TEST_CASE("list_installed_fx returns an empty list as a complete answer", "[fx_tools]")
{
	scripted_fx_host host = host_with_installed_set();

	list_installed_fx_request request;
	request.search_term = "Oxford Inflator";

	const list_installed_fx_result& result = result_of(apply_list_installed_fx(host, request));

	// Not installed is a real answer, and the agent should say so rather than substitute
	// something that sounds similar.
	CHECK(result.matches.empty());
	CHECK(result.total_in_range == 0);
	CHECK(result.truncated == false);
}

TEST_CASE("list_installed_fx caps at twenty by default and says the list was capped", "[fx_tools]")
{
	scripted_fx_host host;

	for (std::size_t index = 0; index < 55; ++index)
	{
		host.installed.push_back(scripted_installed(
			"VST3: Compressor " + std::to_string(index),
			"Compressor " + std::to_string(index)));
	}

	list_installed_fx_request request;
	request.search_term = "compressor";

	const list_installed_fx_result& result = result_of(apply_list_installed_fx(host, request));

	// Requirement 23.9 for this tool: capped at the input schema's default, with the full
	// count carried so the cap is visible.
	CHECK(result.matches.size() == default_installed_fx_matches);
	CHECK(result.total_in_range == 55);
	CHECK(result.truncated == true);
}

TEST_CASE("list_installed_fx honours a requested cap and never exceeds the schema's", "[fx_tools]")
{
	scripted_fx_host host;

	for (std::size_t index = 0; index < 150; ++index)
	{
		host.installed.push_back(scripted_installed(
			"VST3: Compressor " + std::to_string(index),
			"Compressor " + std::to_string(index)));
	}

	SECTION("a cap the call asked for")
	{
		list_installed_fx_request request;
		request.search_term = "compressor";
		request.maximum_results = 5;

		const list_installed_fx_result& result = result_of(apply_list_installed_fx(host, request));

		CHECK(result.matches.size() == 5);
		CHECK(result.total_in_range == 150);
		CHECK(result.truncated == true);
	}

	SECTION("a cap beyond what the output schema accepts")
	{
		list_installed_fx_request request;
		request.search_term = "compressor";
		request.maximum_results = 900;

		const list_installed_fx_result& result = result_of(apply_list_installed_fx(host, request));

		CHECK(result.matches.size() == maximum_installed_fx_matches);
		CHECK(result.truncated == true);
	}
}

TEST_CASE("the installed FX cap falls back to the input schema's default and bounds", "[fx_tools]")
{
	CHECK(effective_installed_fx_cap(std::nullopt) == default_installed_fx_matches);
	CHECK(effective_installed_fx_cap(1) == 1);
	CHECK(effective_installed_fx_cap(0) == 1);
	CHECK(effective_installed_fx_cap(-4) == 1);
	CHECK(effective_installed_fx_cap(100) == maximum_installed_fx_matches);
	CHECK(effective_installed_fx_cap(101) == maximum_installed_fx_matches);
}

TEST_CASE("an empty search term matches nothing rather than everything", "[fx_tools]")
{
	const installed_fx candidate = scripted_installed("VST3: Pro-Q 3", "Pro-Q 3", "FabFilter");

	// Returning the whole installed set for an empty term is what the input schema's
	// `minLength: 1` is there to prevent, and this is the belt to that braces.
	CHECK(rank_installed_fx_match(candidate, "").has_value() == false);
}

// ---------------------------------------------------------------------------
// apply_fx_destructively — the dangerous one
// ---------------------------------------------------------------------------

namespace
{
	scripted_fx_host host_with_items()
	{
		scripted_fx_host host;
		host.chain = chain_named({"ReaEQ", "ReaComp", "ReaVerb"});
		host.item_guids = {item_guid_for('1'), item_guid_for('2'), item_guid_for('3')};

		return host;
	}

	apply_fx_destructively_outcomes outcomes_of(
		const std::variant<apply_fx_destructively_outcomes, action_error>& outcome)
	{
		REQUIRE(std::holds_alternative<apply_fx_destructively_outcomes>(outcome));

		return std::get<apply_fx_destructively_outcomes>(outcome);
	}

	action_error apply_failure_of(
		const std::variant<apply_fx_destructively_outcomes, action_error>& outcome)
	{
		REQUIRE(std::holds_alternative<action_error>(outcome));

		return std::get<action_error>(outcome);
	}
}

TEST_CASE("apply_fx_destructively processes every item on the track when none is named", "[fx_tools]")
{
	scripted_fx_host host = host_with_items();

	apply_fx_destructively_request request;

	const apply_fx_destructively_outcomes& outcomes =
		outcomes_of(apply_fx_destructively(host, scripted_track(), request));

	CHECK(outcomes.every_item_landed());
	CHECK(outcomes.result.processed_item_guids == host.item_guids);
	CHECK(outcomes.actions.size() == 3);
	CHECK(count_applied_actions(outcomes.actions) == 3);

	// Preserving the original take is the default and the recoverable direction.
	CHECK(outcomes.result.replaced_existing_take == false);
	CHECK(host.applied_replace_flags == std::vector<bool>{false, false, false});
}

TEST_CASE("apply_fx_destructively reports replacing the take when that is what it did", "[fx_tools]")
{
	scripted_fx_host host = host_with_items();

	apply_fx_destructively_request request;
	request.replace_existing_take = true;

	const apply_fx_destructively_outcomes& outcomes =
		outcomes_of(apply_fx_destructively(host, scripted_track(), request));

	// The producer's route back differs entirely between the two, so this is the field
	// that says how recoverable the operation was — and it is asserted against what the
	// host was actually asked to do, not against the request.
	CHECK(outcomes.result.replaced_existing_take == true);
	CHECK(host.applied_replace_flags == std::vector<bool>{true, true, true});
}

TEST_CASE("apply_fx_destructively counts only the FX that would actually be rendered in", "[fx_tools]")
{
	scripted_fx_host host = host_with_items();
	host.chain[1].bypassed = true;
	host.chain[2].offline = true;

	apply_fx_destructively_request request;

	const apply_fx_destructively_outcomes& outcomes =
		outcomes_of(apply_fx_destructively(host, scripted_track(), request));

	// A producer who bypassed a plugin to audition without it would not expect it baked
	// in, and an offline FX REAPER has not loaded at all.
	CHECK(outcomes.result.applied_fx_count == 1);
}

TEST_CASE("an empty or fully bypassed chain applies nothing and says so", "[fx_tools]")
{
	CHECK(count_applicable_fx({}) == 0);

	scripted_fx_host host = host_with_items();
	host.chain[0].bypassed = true;
	host.chain[1].bypassed = true;
	host.chain[2].bypassed = true;

	apply_fx_destructively_request request;

	const apply_fx_destructively_outcomes& outcomes =
		outcomes_of(apply_fx_destructively(host, scripted_track(), request));

	// Zero is a real answer rather than an error: the output schema's `minimum: 0`
	// contemplates it, and applying an empty chain is how a producer consolidates items.
	CHECK(outcomes.result.applied_fx_count == 0);
	CHECK(outcomes.every_item_landed());
}

TEST_CASE("apply_fx_destructively processes only the items the call named", "[fx_tools]")
{
	scripted_fx_host host = host_with_items();

	apply_fx_destructively_request request;
	request.item_guids = {item_guid_for('2')};

	const apply_fx_destructively_outcomes& outcomes =
		outcomes_of(apply_fx_destructively(host, scripted_track(), request));

	CHECK(outcomes.result.processed_item_guids == std::vector<std::string>{item_guid_for('2')});
	CHECK(host.applied_item_guids == std::vector<std::string>{item_guid_for('2')});
}

TEST_CASE("apply_fx_destructively refuses to rewrite an item that is on another track", "[fx_tools]")
{
	scripted_fx_host host = host_with_items();

	apply_fx_destructively_request request;
	request.item_guids = {item_guid_for('1'), item_guid_for('9'), item_guid_for('3')};

	const apply_fx_destructively_outcomes& outcomes =
		outcomes_of(apply_fx_destructively(host, scripted_track(), request));

	REQUIRE(outcomes.actions.size() == 3);
	CHECK(outcomes.every_item_landed() == false);

	// Requirement 9.4: a sibling's failure does not undo what landed, and for this tool it
	// could not — the take audio has already been rewritten.
	CHECK(action_was_applied(outcomes.actions[0]));
	CHECK(action_was_applied(outcomes.actions[1]) == false);
	CHECK(action_was_applied(outcomes.actions[2]));

	// The failure names the item it belongs to, which is what makes the partial as
	// informative as `processedItemGuids` would have been.
	CHECK(action_target(outcomes.actions[1]) == item_guid_for('9'));
	CHECK(std::get<action_failed>(outcomes.actions[1]).error.code()
		== std::string{fx_item_not_on_track_code});

	// A GUID from another track names audio the producer did not ask to have rewritten.
	CHECK(
		host.applied_item_guids
		== std::vector<std::string>{item_guid_for('1'), item_guid_for('3')});
	CHECK(outcomes.result.processed_item_guids
		== std::vector<std::string>{item_guid_for('1'), item_guid_for('3')});
}

TEST_CASE("apply_fx_destructively reports an item REAPER would not process as failed", "[fx_tools]")
{
	scripted_fx_host host = host_with_items();
	host.items_that_refuse_apply = {item_guid_for('2')};

	apply_fx_destructively_request request;

	const apply_fx_destructively_outcomes& outcomes =
		outcomes_of(apply_fx_destructively(host, scripted_track(), request));

	CHECK(count_failed_actions(outcomes.actions) == 1);
	CHECK(std::get<action_failed>(outcomes.actions[1]).error.code()
		== std::string{fx_apply_failed_code});

	// The item that failed is not reported as processed, which is what keeps the producer
	// from being told audio was rewritten when it was not.
	CHECK(outcomes.result.processed_item_guids
		== std::vector<std::string>{item_guid_for('1'), item_guid_for('3')});
}

TEST_CASE("every failure apply_fx_destructively reports carries a reason", "[fx_tools]")
{
	// Requirement 9.5 across a mixed array: a reasonless failure is not a representable
	// result, and this sweeps the shapes rather than trusting the type.
	scripted_fx_host host = host_with_items();
	host.items_that_refuse_apply = {item_guid_for('3')};

	apply_fx_destructively_request request;
	request.item_guids = {
		item_guid_for('1'),
		item_guid_for('8'),
		item_guid_for('3'),
		item_guid_for('9')};

	const apply_fx_destructively_outcomes& outcomes =
		outcomes_of(apply_fx_destructively(host, scripted_track(), request));

	REQUIRE(outcomes.actions.size() == 4);
	CHECK(count_failed_actions(outcomes.actions) == 3);

	for (const action_outcome& outcome : outcomes.actions)
	{
		if (action_was_applied(outcome))
		{
			continue;
		}

		const action_failed& failure = std::get<action_failed>(outcome);

		CHECK(failure.error.code().empty() == false);
		CHECK(failure.error.message().empty() == false);
		CHECK(failure.target.empty() == false);
	}
}

TEST_CASE("apply_fx_destructively fails a track with no items rather than doing nothing quietly", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaComp"});

	apply_fx_destructively_request request;

	const action_error& reason = apply_failure_of(apply_fx_destructively(host, scripted_track(), request));

	CHECK(reason.code() == std::string{fx_item_not_on_track_code});
	CHECK(host.applied_item_guids.empty());
}

// ---------------------------------------------------------------------------
// Registration through the executor's seam
// ---------------------------------------------------------------------------

namespace
{
	// The payload type the framework is templated on, standing in for the codec.
	//
	// It carries the request on the way in and a description of the result on the way
	// out, because the framework uses one template parameter for both. Crude, and
	// deliberately so: the point of the parameter is that the framework never reads a
	// field, so the suite needs no JSON library to drive all eight handlers.
	struct test_payload
	{
		std::string described_result;

		std::optional<add_fx_request> add_fx;
		std::optional<remove_fx_request> remove_fx;
		std::optional<set_fx_bypass_request> set_fx_bypass;
		std::optional<set_fx_parameter_request> set_fx_parameter;
		std::optional<get_fx_parameters_request> get_fx_parameters;
		std::optional<list_track_fx_request> list_track_fx;
		std::optional<list_installed_fx_request> list_installed_fx;
		std::optional<apply_fx_destructively_request> apply_fx_destructively;
	};

	// Reads a request out of the payload, and reports when the payload does not carry
	// one — which is the unreadable-input path.
	struct test_request_reader
	{
		bool operator()(const test_payload& input, add_fx_request& request) const
		{
			if (!input.add_fx.has_value())
			{
				return false;
			}

			request = *input.add_fx;

			return true;
		}

		bool operator()(const test_payload& input, remove_fx_request& request) const
		{
			if (!input.remove_fx.has_value())
			{
				return false;
			}

			request = *input.remove_fx;

			return true;
		}

		bool operator()(const test_payload& input, set_fx_bypass_request& request) const
		{
			if (!input.set_fx_bypass.has_value())
			{
				return false;
			}

			request = *input.set_fx_bypass;

			return true;
		}

		bool operator()(const test_payload& input, set_fx_parameter_request& request) const
		{
			if (!input.set_fx_parameter.has_value())
			{
				return false;
			}

			request = *input.set_fx_parameter;

			return true;
		}

		bool operator()(const test_payload& input, get_fx_parameters_request& request) const
		{
			if (!input.get_fx_parameters.has_value())
			{
				return false;
			}

			request = *input.get_fx_parameters;

			return true;
		}

		bool operator()(const test_payload& input, list_track_fx_request& request) const
		{
			if (!input.list_track_fx.has_value())
			{
				return false;
			}

			request = *input.list_track_fx;

			return true;
		}

		bool operator()(const test_payload& input, list_installed_fx_request& request) const
		{
			if (!input.list_installed_fx.has_value())
			{
				return false;
			}

			request = *input.list_installed_fx;

			return true;
		}

		bool operator()(const test_payload& input, apply_fx_destructively_request& request) const
		{
			if (!input.apply_fx_destructively.has_value())
			{
				return false;
			}

			request = *input.apply_fx_destructively;

			return true;
		}
	};

	// Turns a result into a payload. Enough of each result is described that a test can
	// tell which handler produced it and what it said.
	struct test_result_writer
	{
		test_payload operator()(const add_fx_result& result) const
		{
			return test_payload{
				"add_fx " + result.fx_name + " at " + std::to_string(result.fx_index)};
		}

		test_payload operator()(const remove_fx_result& result) const
		{
			return test_payload{
				"remove_fx " + result.fx_name + " leaving " + std::to_string(result.chain_length)};
		}

		test_payload operator()(const set_fx_bypass_result& result) const
		{
			return test_payload{
				std::string{"set_fx_bypass "} + (result.bypassed ? "on" : "off") + " for "
					+ result.fx_name};
		}

		test_payload operator()(const set_fx_parameter_result& result) const
		{
			return test_payload{
				"set_fx_parameter " + result.parameter_name + " to " + std::to_string(result.value)};
		}

		test_payload operator()(const get_fx_parameters_result& result) const
		{
			return test_payload{
				"get_fx_parameters " + std::to_string(result.parameters.size()) + " of "
					+ std::to_string(result.total_in_range)};
		}

		test_payload operator()(const list_track_fx_result& result) const
		{
			return test_payload{"list_track_fx " + std::to_string(result.fx.size())};
		}

		test_payload operator()(const list_installed_fx_result& result) const
		{
			return test_payload{
				"list_installed_fx " + std::to_string(result.matches.size()) + " of "
					+ std::to_string(result.total_in_range)};
		}

		test_payload operator()(const apply_fx_destructively_result& result) const
		{
			return test_payload{
				"apply_fx_destructively " + std::to_string(result.processed_item_guids.size())
					+ " items through " + std::to_string(result.applied_fx_count) + " FX"};
		}
	};

	using test_registry = tool_handler_registry<test_payload>;
	using test_executor = tool_executor_of<test_payload>;
	using test_call = tool_call<test_payload>;
	using test_outcome = dispatch_outcome<test_payload>;
	using test_result = tool_result<test_payload>;

	// A scripted REAPER undo stack, counting everything the framework does to it.
	class scripted_undo_stack final : public undo_stack
	{
	public:
		int current_position() override { return position_; }

		std::string entry_description_at(int position) override
		{
			if (position < 0 || static_cast<std::size_t>(position) >= entries_.size())
			{
				return {};
			}

			return entries_[static_cast<std::size_t>(position)];
		}

		void begin_block() override
		{
			++begin_block_call_count;
			++open_block_depth;
		}

		void end_block(const std::string& description, int) override
		{
			++end_block_call_count;
			--open_block_depth;
			end_block_descriptions.push_back(description);

			entries_.resize(static_cast<std::size_t>(position_));
			entries_.push_back(description);
			position_ = static_cast<int>(entries_.size());
		}

		bool undo_one_entry() override
		{
			++undo_call_count;

			if (position_ <= 0)
			{
				return false;
			}

			--position_;

			return true;
		}

		int begin_block_call_count = 0;
		int end_block_call_count = 0;
		int undo_call_count = 0;
		int open_block_depth = 0;
		std::vector<std::string> end_block_descriptions;

	private:
		std::vector<std::string> entries_;
		int position_ = 0;
	};

	class scripted_track_list final : public TrackListSource
	{
	public:
		explicit scripted_track_list(std::vector<ResolvableTrack> tracks)
			: tracks_{std::move(tracks)}
		{
		}

		std::vector<ResolvableTrack> tracks_in_project_order() const override { return tracks_; }

	private:
		std::vector<ResolvableTrack> tracks_;
	};

	std::vector<ResolvableTrack> one_scripted_track()
	{
		ResolvableTrack track;
		track.guid = mix_bus_guid;
		track.name = mix_bus_name;
		track.project_index = 0;

		return {track};
	}

	// Everything one dispatch needs, kept together so a test reads as the call it makes.
	struct scripted_executor
	{
		explicit scripted_executor(scripted_fx_host& host)
			: tracks{one_scripted_track()},
			undo{stack},
			executor{registry, undo, tracks, aliases}
		{
			registrations = register_fx_tools<test_payload>(
				registry,
				host,
				test_request_reader{},
				test_result_writer{});
		}

		test_registry registry;
		scripted_undo_stack stack;
		scripted_track_list tracks;
		NoLearnedAliases aliases;
		undo_manager undo;
		test_executor executor;
		std::vector<fx_tool_registration> registrations;
	};

	test_call call_for(std::string tool_name, const test_payload& input)
	{
		test_call call;
		call.tool_name = std::move(tool_name);
		call.request_id = "request-1";
		call.validated_input = &input;
		call.track_selectors = {TrackSelector{TrackGuidSelector{mix_bus_guid}}};

		return call;
	}

	test_result dispatched(const test_outcome& outcome)
	{
		REQUIRE(std::holds_alternative<test_result>(outcome));

		return std::get<test_result>(outcome);
	}

	tool_success<test_payload> success_of(const test_outcome& outcome)
	{
		const test_result result = dispatched(outcome);

		REQUIRE(std::holds_alternative<tool_success<test_payload>>(result));

		return std::get<tool_success<test_payload>>(result);
	}

	tool_partial_outcome<test_payload> partial_of(const test_outcome& outcome)
	{
		const test_result result = dispatched(outcome);

		REQUIRE(std::holds_alternative<tool_partial_outcome<test_payload>>(result));

		return std::get<tool_partial_outcome<test_payload>>(result);
	}
}

TEST_CASE("all eight FX tools register, and through the seam each one belongs to", "[fx_tools]")
{
	scripted_fx_host host;
	scripted_executor scripted{host};

	REQUIRE(scripted.registrations.size() == fx_tool_count);
	CHECK(every_fx_tool_registered(scripted.registrations));
	CHECK(scripted.registry.registered_tool_count() == fx_tool_count);

	// The three reads open no undo block and capture no marker; the five that change
	// something open one each.
	const std::vector<std::string_view> reads{
		get_fx_parameters_tool_name,
		list_track_fx_tool_name,
		list_installed_fx_tool_name};

	for (const std::string_view tool_name : reads)
	{
		const auto* const registered = scripted.registry.find_tool(tool_name);

		REQUIRE(registered != nullptr);
		CHECK(registered->undo_effect() == tool_undo_effect::none);
	}

	const std::vector<std::string_view> mutations{
		add_fx_tool_name,
		remove_fx_tool_name,
		set_fx_bypass_tool_name,
		set_fx_parameter_tool_name,
		apply_fx_destructively_tool_name};

	for (const std::string_view tool_name : mutations)
	{
		const auto* const registered = scripted.registry.find_tool(tool_name);

		REQUIRE(registered != nullptr);
		CHECK(registered->undo_effect() == tool_undo_effect::undo_block);
	}
}

TEST_CASE("a registered FX tool runs inside exactly one undo block", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ"});
	host.installed = {scripted_installed("VST3: Pro-Q 3", "Pro-Q 3")};

	scripted_executor scripted{host};

	test_payload input;
	add_fx_request request;
	request.fx_identifier = "VST3: Pro-Q 3";
	input.add_fx = request;

	const test_outcome outcome = scripted.executor.execute(call_for("add_fx", input));
	const tool_success<test_payload>& success = success_of(outcome);

	CHECK(success.fields.described_result == "add_fx Pro-Q 3 at 1");

	// One block, closed, and a marker captured for the turn.
	CHECK(scripted.stack.begin_block_call_count == 1);
	CHECK(scripted.stack.end_block_call_count == 1);
	CHECK(scripted.stack.open_block_depth == 0);
	REQUIRE(success.undo.has_value());
	CHECK(scripted.undo.has_undo_position_marker());

	// And nothing was rewound.
	CHECK(scripted.stack.undo_call_count == 0);
}

TEST_CASE("a read tool opens no undo block and records no marker", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ", "ReaComp"});

	scripted_executor scripted{host};

	test_payload input;
	input.list_track_fx = list_track_fx_request{};

	const tool_success<test_payload>& success =
		success_of(scripted.executor.execute(call_for("list_track_fx", input)));

	CHECK(success.fields.described_result == "list_track_fx 2");
	CHECK(success.undo.has_value() == false);
	CHECK(scripted.stack.begin_block_call_count == 0);

	// Requirement 10.3: a read-only turn records no marker, so it offers no "revert all".
	CHECK(scripted.undo.has_undo_position_marker() == false);
}

TEST_CASE("an FX failure is a failed action carrying a reason, never a refusal", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ"});

	scripted_executor scripted{host};

	test_payload input;
	remove_fx_request request;
	request.fx_index = 9;
	input.remove_fx = request;

	const test_outcome outcome = scripted.executor.execute(call_for("remove_fx", input));

	// The refusal schema's `reason` enum has no value for an FX index past the end of a
	// chain, and no acknowledgement would make the call valid — so it is a failure.
	CHECK(std::holds_alternative<tool_refusal>(dispatched(outcome)) == false);

	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	REQUIRE(partial.actions.size() == 1);
	CHECK(action_was_applied(partial.actions[0]) == false);
	CHECK(std::get<action_failed>(partial.actions[0]).error.code()
		== std::string{fx_index_out_of_range_code});

	// Nothing landed, so there is no position for "revert all" to walk back to.
	CHECK(partial.undo.has_value() == false);
}

TEST_CASE("apply_fx_destructively reports per item when one of them fails", "[fx_tools]")
{
	scripted_fx_host host = host_with_items();
	host.items_that_refuse_apply = {item_guid_for('2')};

	scripted_executor scripted{host};

	test_payload input;
	input.apply_fx_destructively = apply_fx_destructively_request{};

	const tool_partial_outcome<test_payload>& partial =
		partial_of(scripted.executor.execute(call_for("apply_fx_destructively", input)));

	CHECK(partial.applied_action_count() == 2);
	CHECK(partial.failed_action_count() == 1);

	// Requirement 10.8: one block for the whole array, whatever landed.
	CHECK(scripted.stack.begin_block_call_count == 1);
	CHECK(scripted.stack.end_block_call_count == 1);

	// Something landed, so the undo report is present — and REAPER's undo is the only way
	// back from rewritten take audio.
	CHECK(partial.undo.has_value());

	// No rollback: what was applied stays applied.
	CHECK(
		host.applied_item_guids
		== std::vector<std::string>{item_guid_for('1'), item_guid_for('2'), item_guid_for('3')});
}

TEST_CASE("apply_fx_destructively returns the tool's own fields when every item lands", "[fx_tools]")
{
	scripted_fx_host host = host_with_items();

	scripted_executor scripted{host};

	test_payload input;
	input.apply_fx_destructively = apply_fx_destructively_request{};

	const tool_success<test_payload>& success =
		success_of(scripted.executor.execute(call_for("apply_fx_destructively", input)));

	CHECK(success.fields.described_result == "apply_fx_destructively 3 items through 3 FX");
	CHECK(success.undo.has_value());
}

TEST_CASE("an input the handler cannot read fails the action rather than crashing", "[fx_tools]")
{
	scripted_fx_host host;
	host.chain = chain_named({"ReaEQ"});

	scripted_executor scripted{host};

	// A payload carrying no request for this tool, which is what an upstream mismatch
	// looks like from in here.
	test_payload input;

	const tool_partial_outcome<test_payload>& partial =
		partial_of(scripted.executor.execute(call_for("set_fx_bypass", input)));

	REQUIRE(partial.actions.size() == 1);
	CHECK(action_was_applied(partial.actions[0]) == false);
	CHECK(host.bypass_write_count == 0);
}

TEST_CASE("list_installed_fx dispatches without addressing a track", "[fx_tools]")
{
	scripted_fx_host host = host_with_installed_set();

	scripted_executor scripted{host};

	test_payload input;
	list_installed_fx_request request;
	request.search_term = "fabfilter";
	input.list_installed_fx = request;

	test_call call = call_for("list_installed_fx", input);

	// The producer's installed set is a property of the machine, not of a track.
	call.track_selectors.clear();

	const tool_success<test_payload>& success = success_of(scripted.executor.execute(call));

	CHECK(success.fields.described_result == "list_installed_fx 2 of 2");
	CHECK(success.undo.has_value() == false);
}
