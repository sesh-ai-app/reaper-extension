// Test doubles for the composition root.
//
// The composition root is the one thing in this extension that touches every seam at
// once, so this is the one place every seam has to be substituted at once. Nine
// REAPER-backed adapters, one stand-in for nlohmann/json, one recording browser, and
// the graph assembled over all of them.
//
// None of the host stubs does anything. That is not laziness — the registration calls
// `compose_tool_handler_registry` makes capture references to the hosts and do not call
// them, so what the hosts would have answered is beside the point here and is each
// family's own test's subject. What this file has to provide is objects of the right
// types with the right lifetimes, which is precisely what the composition's reference
// topology is about.
//
// `StubJson` is the exception: it has to work, because the Confirmation Coordinator,
// the Transport Handler, the Stream Presenter, and the Context Builder all read or
// write real documents on the paths these cases drive. It is the union of what those
// four do to a document and nothing more. `tests/context/project_context_test_doubles.h`
// has a narrower one for the write half only; this one needs both directions, so it is
// its own rather than an extension of that one — a stub that grew a second purpose is
// how a test double starts having behaviour worth testing.

#ifndef SESH_AI_TESTS_ENTRY_COMPOSITION_TEST_DOUBLES_H
#define SESH_AI_TESTS_ENTRY_COMPOSITION_TEST_DOUBLES_H

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <entry/extension_composition.h>
#include <transport/queue_pair.h>
#include <ui/deferred_browser_host.h>
#include <ui/ui_host.h>

#include "../context/project_context_test_doubles.h"

namespace sesh_ai_tests
{
	// ---------------------------------------------------------------------------
	// The document
	// ---------------------------------------------------------------------------

	// nlohmann::json, reduced to what the four reading-and-writing components on these
	// paths actually do to a document.
	//
	// Reading: `is_object`, `contains`, `at`, `is_string`, `is_number_integer`,
	// `is_boolean`, `is_null`, and `get<T>`. Writing: `object`, `array`,
	// `operator[]`, `push_back`, and assignment from each scalar.
	//
	// Integers and doubles are kept apart for the reason
	// `project_context_test_doubles.h` records: the schemas declare some fields
	// `integer` and some `number`, and a serialiser that wrote 48000.0 where an integer
	// was wanted would pass every value assertion and fail validation.
	class StubJson
	{
	public:
		enum class Kind
		{
			null_value,
			object_value,
			array_value,
			string_value,
			integer_value,
			number_value,
			boolean_value
		};

		StubJson() = default;

		static StubJson object()
		{
			StubJson document;
			document.kind_ = Kind::object_value;

			return document;
		}

		static StubJson array()
		{
			StubJson document;
			document.kind_ = Kind::array_value;

			return document;
		}

		static StubJson string_value(std::string value)
		{
			StubJson document;
			document.kind_ = Kind::string_value;
			document.string_ = std::move(value);

			return document;
		}

		static StubJson integer_value(long long value)
		{
			StubJson document;
			document.kind_ = Kind::integer_value;
			document.integer_ = value;

			return document;
		}

		static StubJson boolean_value(bool value)
		{
			StubJson document;
			document.kind_ = Kind::boolean_value;
			document.boolean_ = value;

			return document;
		}

		static StubJson object_with(std::vector<std::pair<std::string, StubJson>> properties)
		{
			StubJson document = object();

			for (auto& property : properties)
			{
				document[property.first] = std::move(property.second);
			}

			return document;
		}

		// Vivifies to an object the way nlohmann::json does, so a serialiser can write
		// into a default-constructed document.
		StubJson& operator[](const std::string& property_name)
		{
			if (kind_ == Kind::null_value)
			{
				kind_ = Kind::object_value;
			}

			if (properties_.count(property_name) == 0)
			{
				property_order_.push_back(property_name);
			}

			return properties_[property_name];
		}

		StubJson& operator=(std::string value)
		{
			kind_ = Kind::string_value;
			string_ = std::move(value);

			return *this;
		}

		StubJson& operator=(const char* value)
		{
			return *this = std::string{value};
		}

		StubJson& operator=(double value)
		{
			kind_ = Kind::number_value;
			number_ = value;

			return *this;
		}

		StubJson& operator=(int value)
		{
			kind_ = Kind::integer_value;
			integer_ = value;

			return *this;
		}

		StubJson& operator=(long long value)
		{
			kind_ = Kind::integer_value;
			integer_ = value;

			return *this;
		}

		StubJson& operator=(std::size_t value)
		{
			kind_ = Kind::integer_value;
			integer_ = static_cast<long long>(value);

			return *this;
		}

		StubJson& operator=(bool value)
		{
			kind_ = Kind::boolean_value;
			boolean_ = value;

			return *this;
		}

		void push_back(StubJson element)
		{
			if (kind_ == Kind::null_value)
			{
				kind_ = Kind::array_value;
			}

			elements_.push_back(std::move(element));
		}

		void push_back(const std::string& element)
		{
			push_back(string_value(element));
		}

		bool is_object() const { return kind_ == Kind::object_value; }
		bool is_array() const { return kind_ == Kind::array_value; }
		bool is_string() const { return kind_ == Kind::string_value; }
		bool is_number_integer() const { return kind_ == Kind::integer_value; }
		bool is_number() const { return kind_ == Kind::number_value || kind_ == Kind::integer_value; }
		bool is_boolean() const { return kind_ == Kind::boolean_value; }
		bool is_null() const { return kind_ == Kind::null_value; }

		bool contains(const std::string& property_name) const
		{
			return properties_.count(property_name) > 0;
		}

		const StubJson& at(const std::string& property_name) const
		{
			static const StubJson absent;

			const auto found = properties_.find(property_name);

			return found == properties_.end() ? absent : found->second;
		}

		const std::vector<StubJson>& elements() const { return elements_; }
		const std::vector<std::string>& property_names() const { return property_order_; }

		template <typename ValueType>
		ValueType get() const
		{
			if constexpr (std::is_same_v<ValueType, std::string>)
			{
				return string_;
			}
			else if constexpr (std::is_same_v<ValueType, bool>)
			{
				return boolean_;
			}
			else if constexpr (std::is_floating_point_v<ValueType>)
			{
				return static_cast<ValueType>(kind_ == Kind::integer_value ? static_cast<double>(integer_) : number_);
			}
			else if constexpr (std::is_integral_v<ValueType>)
			{
				return static_cast<ValueType>(integer_);
			}
			else
			{
				return ValueType{};
			}
		}

		Kind kind() const { return kind_; }

	private:
		Kind kind_ = Kind::null_value;

		std::map<std::string, StubJson> properties_;
		std::vector<std::string> property_order_;
		std::vector<StubJson> elements_;

		std::string string_;
		double number_ = 0.0;
		long long integer_ = 0;
		bool boolean_ = false;
	};

	// ---------------------------------------------------------------------------
	// The envelopes
	// ---------------------------------------------------------------------------

	struct CompositionInboundEnvelope
	{
		std::string type;
		std::string request_id;
		StubJson payload;
	};

	// The four fields `transport/envelope.h` declares, including task 22.2's override.
	struct CompositionOutboundEnvelope
	{
		std::string type;
		std::string request_id;
		StubJson payload;
		std::string payload_schema_path_override;
	};

	inline CompositionInboundEnvelope an_envelope(std::string type, std::string request_id)
	{
		CompositionInboundEnvelope envelope;
		envelope.type = std::move(type);
		envelope.request_id = std::move(request_id);
		envelope.payload = StubJson::object();

		return envelope;
	}

	// `confirmation-request.schema.json`'s three fields, in its own snake_case — the one
	// protocol payload that does not read as camelCase, which the bridge normalises.
	inline CompositionInboundEnvelope a_confirmation_request(std::string request_id)
	{
		CompositionInboundEnvelope envelope = an_envelope("confirm:request", std::move(request_id));

		envelope.payload = StubJson::object_with({
			{"action_summary", StubJson::string_value("Delete the track \"Kick\"")},
			{"risk_level", StubJson::string_value("high")},
			{"details", StubJson::string_value("One track, with 4 items on it.")}
		});

		return envelope;
	}

	// ---------------------------------------------------------------------------
	// The nine REAPER-backed adapters, replaced
	// ---------------------------------------------------------------------------
	//
	// Each answers the emptiest readable thing its interface allows. Nothing here is
	// called by registration, and nothing these cases drive reaches a host — the one
	// handler path that runs is a tool call whose target resolution refuses, and that
	// happens before any handler.

	class StubFxHost final : public sesh_ai::daw::tools::fx_host
	{
	public:
		bool is_usable() const override { return true; }
		std::vector<std::string> unresolved_function_names() const override { return {}; }

		std::vector<sesh_ai::daw::tools::fx_chain_entry> read_track_fx(const std::string&) override
		{
			return {};
		}

		std::vector<sesh_ai::daw::tools::fx_parameter> read_fx_parameters(
			const std::string&,
			int) override
		{
			return {};
		}

		std::vector<sesh_ai::daw::tools::installed_fx> read_installed_fx() override { return {}; }

		std::vector<std::string> read_track_item_guids(const std::string&) override { return {}; }

		std::optional<int> insert_fx(
			const std::string&,
			const std::string&,
			std::optional<int>) override
		{
			return std::nullopt;
		}

		bool delete_fx(const std::string&, int) override { return false; }
		bool set_fx_bypassed(const std::string&, int, bool) override { return false; }
		bool set_fx_parameter_value(const std::string&, int, int, double) override { return false; }
		bool apply_fx_to_item(const std::string&, bool) override { return false; }
	};

	class StubItemHost final : public sesh_ai::daw::tools::item_host
	{
	public:
		bool is_usable() const override { return true; }
		std::vector<std::string> unresolved_function_names() const override { return {}; }

		std::vector<sesh_ai::daw::tools::item_state> read_project_items() override { return {}; }

		std::optional<sesh_ai::daw::tools::item_time_range> read_time_selection() override
		{
			return std::nullopt;
		}

		sesh_ai::daw::tools::item_file_access probe_file(const std::string&) override { return {}; }

		bool set_item_position(const std::string&, double) override { return false; }
		bool set_item_length(const std::string&, double) override { return false; }
		bool set_item_fade_in(const std::string&, double) override { return false; }
		bool set_item_fade_out(const std::string&, double) override { return false; }
		bool set_item_volume(const std::string&, double) override { return false; }
		bool set_item_muted(const std::string&, bool) override { return false; }
		bool move_item_to_track(const std::string&, const std::string&) override { return false; }

		std::optional<sesh_ai::daw::tools::item_split_halves> split_item(
			const std::string&,
			double) override
		{
			return std::nullopt;
		}

		bool delete_item(const std::string&) override { return false; }

		std::optional<sesh_ai::daw::tools::imported_media> import_media_file(
			const std::string&,
			const std::string&,
			double) override
		{
			return std::nullopt;
		}
	};

	class StubMarkerRegionHost final : public sesh_ai::daw::tools::MarkerRegionHost
	{
	public:
		std::vector<sesh_ai::daw::tools::marker_or_region>
		read_markers_and_regions_in_enumeration_order() override
		{
			return {};
		}

		std::optional<sesh_ai::daw::tools::marker_or_region> create_marker_or_region(
			const sesh_ai::daw::tools::marker_or_region_creation&) override
		{
			return std::nullopt;
		}

		bool write_marker_or_region(
			const sesh_ai::daw::tools::marker_or_region_write&) override
		{
			return false;
		}

		bool delete_marker_or_region(const std::string&) override { return false; }

		std::vector<sesh_ai::daw::tools::tempo_map_point> read_tempo_map() override { return {}; }

		std::optional<sesh_ai::daw::tools::initial_tempo_and_time_signature>
		read_initial_tempo_and_time_signature() override
		{
			return std::nullopt;
		}

		bool write_tempo_point(
			std::optional<std::size_t>,
			const sesh_ai::daw::tools::tempo_map_point&) override
		{
			return false;
		}

		bool clear_tempo_map() override { return false; }
		std::vector<double> read_item_positions() override { return {}; }
		bool write_time_selection(double, double) override { return false; }
	};

	class StubRoutingHost final : public sesh_ai::daw::tools::routing_host
	{
	public:
		sesh_ai::daw::tools::routing_snapshot read_routing_snapshot() override { return {}; }

		std::optional<sesh_ai::daw::tools::routing_send> read_send(
			const std::string&,
			int) override
		{
			return std::nullopt;
		}

		std::optional<int> create_send(const std::string&, const std::string&) override
		{
			return std::nullopt;
		}

		bool remove_send(const std::string&, int) override { return false; }

		bool write_send_state(
			const std::string&,
			int,
			const sesh_ai::daw::tools::send_state_change&) override
		{
			return false;
		}

		bool write_parent_send(const std::string&, bool) override { return false; }

		std::optional<sesh_ai::daw::TrackReference> insert_track(int, const std::string&) override
		{
			return std::nullopt;
		}

		bool delete_track(const std::string&) override { return false; }

		bool apply_track_order(const std::vector<std::string>&) override { return false; }

		bool write_folder_depth(const std::string&, int) override { return false; }

		std::optional<sesh_ai::daw::tools::routing_fx_placement> add_fx(
			const std::string&,
			const std::string&) override
		{
			return std::nullopt;
		}
	};

	class StubTrackStateHost final : public sesh_ai::daw::tools::track_state_host
	{
	public:
		bool is_usable() const override { return true; }
		std::vector<std::string> unresolved_function_names() const override { return {}; }

		std::vector<sesh_ai::daw::tools::track_state_reading> read_tracks_in_project_order(
			bool) override
		{
			return {};
		}

		sesh_ai::daw::tools::project_summary_reading read_project_summary() override { return {}; }

		bool write_track_name(const std::string&, const std::string&) override { return false; }
		bool write_track_volume(const std::string&, double) override { return false; }
		bool write_track_pan(const std::string&, double) override { return false; }
		bool write_track_muted(const std::string&, bool) override { return false; }
		bool write_track_soloed(const std::string&, bool) override { return false; }
		bool write_track_armed(const std::string&, bool) override { return false; }
		bool write_track_color(const std::string&, int) override { return false; }
	};

	class StubTrackStructureHost final : public sesh_ai::daw::tools::TrackStructureHost
	{
	public:
		std::vector<sesh_ai::daw::tools::structural_track> read_tracks_in_project_order() override
		{
			return {};
		}

		std::string insert_track(int, const std::string&, std::optional<int>) override { return {}; }

		bool delete_track(const std::string&) override { return false; }

		std::vector<std::string> duplicate_tracks(const std::vector<std::string>&) override
		{
			return {};
		}

		bool reorder_tracks(const std::vector<std::string>&) override { return false; }

		bool write_folder_depth_delta(const std::string&, int) override { return false; }
	};

	class StubRenderHost final : public sesh_ai::daw::RenderHost
	{
	public:
		sesh_ai::daw::render_project_state read_project_state() override { return {}; }
		std::vector<sesh_ai::daw::render_track> read_selected_tracks() override { return {}; }

		std::optional<sesh_ai::daw::time_span> find_region_span(const std::string&) override
		{
			return std::nullopt;
		}

		std::string read_render_directory() override { return "/tmp/sesh-ai-renders"; }

		void write_render_settings(sesh_ai::daw::render_settings_word) override {}
		void write_render_bounds(int, const sesh_ai::daw::time_span&) override {}
		void write_render_sample_rate(int) override {}
		void write_render_format(const sesh_ai::daw::render_format_request&) override {}
		void write_render_output(const std::string&, const std::string&) override {}

		std::string read_render_targets() override { return {}; }
		bool output_file_exists(const std::string&) override { return false; }
		void set_track_solo(const std::string&, bool) override {}
		void add_project_to_render_queue() override {}
	};

	// `reaper_undo_stack.cpp`'s seam. The position is held so the Undo Manager's
	// arithmetic has something consistent to read.
	class StubUndoStack final : public sesh_ai::daw::undo_stack
	{
	public:
		int current_position() override { return position; }
		std::string entry_description_at(int) override { return {}; }
		void begin_block() override { ++blocks_opened; }

		void end_block(const std::string&, int) override
		{
			++blocks_closed;
			++position;
		}

		bool undo_one_entry() override
		{
			if (position == 0)
			{
				return false;
			}

			--position;

			return true;
		}

		int position = 0;
		std::size_t blocks_opened = 0;
		std::size_t blocks_closed = 0;
	};

	// `reaper_alias_storage.cpp`'s seam. An empty project with nothing learned.
	class StubAliasStorage final : public sesh_ai::daw::AliasStorage
	{
	public:
		int track_count() const override { return 0; }
		sesh_ai::daw::TrackHandle track_at(int) const override { return {}; }

		std::string track_extension_value(
			sesh_ai::daw::TrackHandle,
			const std::string&) const override
		{
			return {};
		}

		bool set_track_extension_value(
			sesh_ai::daw::TrackHandle,
			const std::string&,
			const std::string&) override
		{
			return false;
		}

		std::string project_extension_value(
			const std::string&,
			const std::string&) const override
		{
			return {};
		}

		bool set_project_extension_value(
			const std::string&,
			const std::string&,
			const std::string&) override
		{
			return false;
		}

		bool project_has_been_saved() const override { return true; }
		bool project_has_unsaved_changes() const override { return false; }
	};

	// An empty project. Which is what makes a name-pattern selector refuse, and that
	// refusal is how these cases reach the refusal shape through the real path rather
	// than by substituting an executor.
	class StubTrackListSource final : public sesh_ai::daw::TrackListSource
	{
	public:
		std::vector<sesh_ai::daw::ResolvableTrack> tracks_in_project_order() const override
		{
			return {};
		}
	};

	class StubLearnedAliasLookup final : public sesh_ai::daw::LearnedAliasLookup
	{
	public:
		std::vector<std::string> learned_aliases_for_track(
			const sesh_ai::daw::ResolvableTrack&) const override
		{
			return {};
		}
	};

	// The seven transport commands' one REAPER call apiece, recorded.
	class StubTransportActionInvoker final : public sesh_ai::transport::TransportActionInvoker
	{
	public:
		bool invoke_main_action(int reaper_action_command_id) override
		{
			invoked_command_ids.push_back(reaper_action_command_id);

			return true;
		}

		std::vector<int> invoked_command_ids;
	};

	// ---------------------------------------------------------------------------
	// CEF, replaced
	// ---------------------------------------------------------------------------

	class RecordingBrowserHost final : public sesh_ai::ui::BrowserHost
	{
	public:
		struct PublishedMessage
		{
			std::string message_name;
			std::string serialized_json;
		};

		bool create_browser(const std::string& local_asset_url) override
		{
			loaded_url = local_asset_url;

			return true;
		}

		bool navigate(const std::string& absolute_url) override
		{
			loaded_url = absolute_url;

			return true;
		}

		bool post_message_to_javascript(
			const std::string& message_name,
			const std::string& serialized_json) override
		{
			published.push_back(PublishedMessage{message_name, serialized_json});

			return true;
		}

		void close_browser() override {}
		void shut_down_cef() override {}

		std::size_t count_of(std::string_view message_name) const
		{
			std::size_t count = 0;

			for (const PublishedMessage& message : published)
			{
				if (message.message_name == message_name)
				{
					++count;
				}
			}

			return count;
		}

		std::vector<PublishedMessage> published;
		std::string loaded_url;
	};

	// ---------------------------------------------------------------------------
	// The JSON seams
	// ---------------------------------------------------------------------------

	// fx, item, and track_state's reader, as the overload set those three take.
	//
	// Generic over every request type, because what these cases are about is that the
	// families register rather than what any handler reads. The one explicit overload
	// is `set_item_properties_request`, and it earns its place: a request naming an item
	// no project has produces a failed action, which is the only way to reach the
	// framework's partial-outcome shape through the real executor rather than by
	// substituting one.
	struct StubRequestReader
	{
		template <typename RequestType>
		bool operator()(const StubJson&, RequestType&) const
		{
			return true;
		}

		template <typename RequestType>
		bool operator()(const StubJson&, RequestType&, std::string_view) const
		{
			return true;
		}

		// The one explicit overload, and it answers false.
		//
		// `add_fx`'s handler turns an unreadable input into `handler_action_outcomes`
		// carrying one failed action, and `handler_action_outcomes` is what the
		// framework reports as a `tool_partial_outcome`. That is the only route to the
		// partial shape that goes through the real executor rather than substituting
		// one, and the partial is the shape task 22.2's schema override exists for —
		// so the case that reads the override has to reach it honestly.
		bool operator()(const StubJson&, sesh_ai::daw::tools::add_fx_request&) const
		{
			return false;
		}
	};

	struct StubResultWriter
	{
		template <typename ResultType>
		StubJson operator()(const ResultType&) const
		{
			return StubJson::object();
		}
	};

	// Which of the seven per-family codecs to leave incomplete, so a case can drive the
	// gap the registry report exists to surface.
	struct CodecGaps
	{
		bool marker_region = false;
		bool marker_region_read = false;
		bool render = false;
		bool routing = false;
		bool tempo = false;
		bool track_structure = false;
		bool undo = false;
	};

	// The seven codecs, filled in with readers that answer a default request and writers
	// that answer an empty object.
	inline sesh_ai::entry::ToolPayloadCodecs<StubJson> stub_tool_payload_codecs(CodecGaps gaps)
	{
		using namespace sesh_ai::daw;
		using Context = tool_execution_context<StubJson>;

		sesh_ai::entry::ToolPayloadCodecs<StubJson> codecs;

		if (!gaps.marker_region)
		{
			codecs.marker_region.read_create_marker_request =
				[](const Context&) { return tools::create_marker_request{}; };
			codecs.marker_region.read_create_region_request =
				[](const Context&) { return tools::create_region_request{}; };
			codecs.marker_region.read_update_marker_or_region_request =
				[](const Context&) { return tools::update_marker_or_region_request{}; };
			codecs.marker_region.read_delete_marker_or_region_request =
				[](const Context&) { return tools::delete_marker_or_region_request{}; };
			codecs.marker_region.read_change_tempo_map_request =
				[](const Context&) { return tools::change_tempo_map_request{}; };
			codecs.marker_region.read_set_time_selection_request =
				[](const Context&) { return tools::set_time_selection_request{}; };

			codecs.marker_region.write_create_marker_result =
				[](const tools::create_marker_result&) { return StubJson::object(); };
			codecs.marker_region.write_create_region_result =
				[](const tools::create_region_result&) { return StubJson::object(); };
			codecs.marker_region.write_update_marker_or_region_result =
				[](const tools::update_marker_or_region_result&) { return StubJson::object(); };
			codecs.marker_region.write_delete_marker_or_region_result =
				[](const tools::delete_marker_or_region_result&) { return StubJson::object(); };
			codecs.marker_region.write_change_tempo_map_result =
				[](const tools::change_tempo_map_result&) { return StubJson::object(); };
			codecs.marker_region.write_set_time_selection_result =
				[](const tools::set_time_selection_result&) { return StubJson::object(); };
		}

		if (!gaps.marker_region_read)
		{
			codecs.marker_region_read.read_list_markers_request =
				[](const Context&) { return tools::timeline_read_window{}; };
			codecs.marker_region_read.read_list_regions_request =
				[](const Context&) { return tools::timeline_read_window{}; };
			codecs.marker_region_read.write_list_markers_result =
				[](const tools::list_markers_result&) { return StubJson::object(); };
			codecs.marker_region_read.write_list_regions_result =
				[](const tools::list_regions_result&) { return StubJson::object(); };
		}

		if (!gaps.render)
		{
			codecs.render.read_render_request =
				[](const StubJson&) { return render_request{}; };
			codecs.render.write_render_result =
				[](const render_result&) { return StubJson::object(); };
		}

		if (!gaps.routing)
		{
			codecs.routing.read_create_send_request =
				[](const StubJson&) { return tools::create_send_request{}; };
			codecs.routing.read_remove_send_request =
				[](const StubJson&) { return tools::remove_send_request{}; };
			codecs.routing.read_set_send_state_request =
				[](const StubJson&) { return tools::set_send_state_request{}; };
			codecs.routing.read_set_parent_send_request =
				[](const StubJson&) { return tools::set_parent_send_request{}; };
			codecs.routing.read_create_bus_request =
				[](const StubJson&) { return tools::bus_construction_request{}; };

			codecs.routing.write_create_send_result =
				[](const tools::create_send_result&) { return StubJson::object(); };
			codecs.routing.write_remove_send_result =
				[](const tools::remove_send_result&) { return StubJson::object(); };
			codecs.routing.write_set_send_state_result =
				[](const tools::set_send_state_result&) { return StubJson::object(); };
			codecs.routing.write_set_parent_send_result =
				[](const tools::set_parent_send_result&) { return StubJson::object(); };
			codecs.routing.write_create_bus_result =
				[](const tools::create_bus_result&) { return StubJson::object(); };
			codecs.routing.write_remove_bus_result =
				[](const tools::remove_bus_result&) { return StubJson::object(); };
			codecs.routing.write_get_routing_result =
				[](const tools::get_routing_result&) { return StubJson::object(); };
		}

		if (!gaps.tempo)
		{
			codecs.tempo.read_list_tempo_changes_request =
				[](const Context&) { return tools::timeline_read_window{}; };
			codecs.tempo.write_list_tempo_changes_result =
				[](const tools::list_tempo_changes_result&) { return StubJson::object(); };
		}

		if (!gaps.track_structure)
		{
			codecs.track_structure.read_create_track_request =
				[](const Context&) { return tools::create_track_request{}; };
			codecs.track_structure.read_delete_track_request =
				[](const Context&) { return tools::delete_track_request{}; };
			codecs.track_structure.read_duplicate_track_request =
				[](const Context&) { return tools::duplicate_track_request{}; };
			codecs.track_structure.read_set_folder_structure_request =
				[](const Context&) { return tools::set_folder_structure_request{}; };

			codecs.track_structure.write_create_track_result =
				[](const tools::create_track_result&) { return StubJson::object(); };
			codecs.track_structure.write_delete_track_result =
				[](const tools::delete_track_result&) { return StubJson::object(); };
			codecs.track_structure.write_duplicate_track_result =
				[](const tools::duplicate_track_result&) { return StubJson::object(); };
			codecs.track_structure.write_set_folder_structure_result =
				[](const tools::set_folder_structure_result&) { return StubJson::object(); };
		}

		if (!gaps.undo)
		{
			codecs.undo.write_undo_last_action_result =
				[](const undo_last_action_outcome&) { return StubJson::object(); };
			codecs.undo.write_revert_agent_changes_result =
				[](const revert_outcome&) { return StubJson::object(); };
		}

		return codecs;
	}

	// ---------------------------------------------------------------------------
	// The graph
	// ---------------------------------------------------------------------------

	using Graph = sesh_ai::entry::ExtensionObjectGraph<
		StubJson,
		CompositionInboundEnvelope,
		CompositionOutboundEnvelope
	>;

	inline std::string describe_failures(
		const sesh_ai::entry::ToolRegistryCompositionReport& report)
	{
		std::string description;

		for (const sesh_ai::entry::ToolRegistrationFailure& failure : report.failures)
		{
			description.append(failure.tool_name);
			description.append(" (");
			description.append(sesh_ai::daw::describe_tool_registration_outcome(failure.outcome));
			description.append(") ");
		}

		return description;
	}

	inline std::string describe_unregistered(
		const sesh_ai::entry::ToolRegistryCompositionReport& report)
	{
		std::string description;

		for (const std::string& tool_name : report.unregistered_constrained_tool_names)
		{
			description.append(tool_name);
			description.push_back(' ');
		}

		return description;
	}

	inline bool contains(const std::vector<std::string>& values, std::string_view value)
	{
		for (const std::string& candidate : values)
		{
			if (candidate == value)
			{
				return true;
			}
		}

		return false;
	}

	// Everything a case needs, assembled.
	//
	// The doubles are members and the graph holds references into them, so declaration
	// order here is the same constraint the plugin entry has: the doubles before the
	// graph, and the graph destroyed first.
	class ComposedGraph
	{
	public:
		explicit ComposedGraph(CodecGaps gaps = CodecGaps{})
		{
			// A readable one-track project, so the Context Builder has something to
			// answer a `request:project_context` with. The default reading is
			// `readable` false, which is a different case and one
			// `project_context_builder_test.cpp` already covers.
			project_context_source_.reading = plain_reading();

			Graph::Dependencies dependencies{
				outbound_queue_,
				tool_host_references(),
				track_list_,
				project_context_source_,
				transport_actions_,
				[this] { ++ui_state_publication_requests_; },
				tool_call_codec(),
				stub_tool_payload_codecs(gaps),
				parse_bridge_payload(),
				read_bridge_request_identifier(),
				[](const StubJson&) { return std::string{"{\"fileName\":\"generated.lua\"}"}; }
			};

			graph_ = std::make_unique<Graph>(
				std::move(dependencies),
				StubRequestReader{},
				StubResultWriter{}
			);

			// The panel, opened. Every publish refuses with `browser_not_created`
			// otherwise, and that refusal is `ui_host_test.cpp`'s subject rather than
			// this file's.
			REQUIRE(graph_->deferred_browser_host().bind(&browser_host_));

			const auto application_entry_point =
				sesh_ai::ui::build_local_asset_url("/opt/sesh-ai/ui", "index.html");

			REQUIRE(application_entry_point.valid);
			REQUIRE(graph_->ui_host().create_browser(application_entry_point.url).created);
		}

		ComposedGraph(const ComposedGraph&) = delete;
		ComposedGraph& operator=(const ComposedGraph&) = delete;
		ComposedGraph(ComposedGraph&&) = delete;
		ComposedGraph& operator=(ComposedGraph&&) = delete;

		Graph& graph() { return *graph_; }
		const Graph& graph() const { return *graph_; }

		RecordingBrowserHost& browser_host() { return browser_host_; }
		const RecordingBrowserHost& browser_host() const { return browser_host_; }

		StubProjectContextSource& project_context_source() { return project_context_source_; }
		StubTransportActionInvoker& transport_actions() { return transport_actions_; }

		std::size_t ui_state_publication_requests() const
		{
			return ui_state_publication_requests_;
		}

		std::vector<CompositionOutboundEnvelope> drain_outbound()
		{
			return outbound_queue_.drain_up_to(64);
		}

		bool send_from_javascript(std::string message_name, std::string serialized_json)
		{
			return graph_->ui_host()
				.receive_from_javascript(
					std::move(message_name),
					sesh_ai::ui::SerializedBridgePayload{std::move(serialized_json)}
				)
				.forwarded;
		}

		// Makes every tool call resolve a name pattern against an empty project, which
		// the Object Resolver refuses with `unresolved_track_selector`. The refusal is
		// the executor's own, through the real path — which is what makes the schema
		// path assertion worth anything.
		void refuse_every_tool_call() { refuse_targets_ = true; }

	private:
		Graph::ToolCallCodec tool_call_codec()
		{
			Graph::ToolCallCodec codec;

			codec.read_tool_call =
				[this](std::string_view tool_name, const CompositionInboundEnvelope& inbound_envelope) {
					sesh_ai::daw::tool_call<StubJson> call;

					call.tool_name.assign(tool_name);
					call.request_id = inbound_envelope.request_id;
					call.validated_input = &inbound_envelope.payload;

					if (refuse_targets_)
					{
						call.track_selectors.push_back(
							sesh_ai::daw::TrackNamePatternSelector{"the kick"}
						);
					}

					return call;
				};

			codec.write_dispatch_outcome =
				[](const sesh_ai::daw::dispatch_outcome<StubJson>&) {
					return StubJson::object();
				};

			return codec;
		}

		static Graph::Sink::PayloadParser parse_bridge_payload()
		{
			// Enough of a parser for the three messages the UI originates, which carry
			// a `requestId` and a `promptText` and nothing structured. A real JSON
			// library is `extension_composition.cpp`'s to supply.
			return [](const std::string& serialized_json) -> std::optional<StubJson> {
				if (serialized_json.empty() || serialized_json.front() != '{')
				{
					return std::nullopt;
				}

				StubJson document = StubJson::object();

				const std::size_t identifier_at = serialized_json.find("\"requestId\":\"");

				if (identifier_at != std::string::npos) {
					const std::size_t value_at =
						identifier_at + std::string_view{"\"requestId\":\""}.size();
					const std::size_t value_ends = serialized_json.find('"', value_at);

					document["requestId"] = StubJson::string_value(
						serialized_json.substr(value_at, value_ends - value_at)
					);
				}

				return document;
			};
		}

		static Graph::Sink::RequestIdentifierReader read_bridge_request_identifier()
		{
			return [](const StubJson& payload) -> std::string {
				if (!payload.is_object() || !payload.contains("requestId"))
				{
					return {};
				}

				return payload.at("requestId").get<std::string>();
			};
		}

		sesh_ai::entry::ToolHostReferences tool_host_references()
		{
			return sesh_ai::entry::ToolHostReferences{
				fx_host_,
				item_host_,
				marker_region_host_,
				routing_host_,
				track_state_host_,
				track_structure_host_,
				render_coordinator_,
				undo_manager_,
				alias_store_,
				learned_aliases_
			};
		}

		// The doubles. Before the graph, which holds references into them.
		sesh_ai::transport::ConcurrentQueue<CompositionOutboundEnvelope> outbound_queue_;

		StubFxHost fx_host_;
		StubItemHost item_host_;
		StubMarkerRegionHost marker_region_host_;
		StubRoutingHost routing_host_;
		StubTrackStateHost track_state_host_;
		StubTrackStructureHost track_structure_host_;
		StubRenderHost render_host_;
		sesh_ai::daw::render_coordinator render_coordinator_{render_host_};

		StubUndoStack undo_stack_;
		sesh_ai::daw::undo_manager undo_manager_{undo_stack_};

		StubAliasStorage alias_storage_;
		sesh_ai::daw::AliasStore alias_store_{alias_storage_};
		StubLearnedAliasLookup learned_aliases_;

		StubTrackListSource track_list_;
		StubProjectContextSource project_context_source_;
		StubTransportActionInvoker transport_actions_;

		RecordingBrowserHost browser_host_;

		std::size_t ui_state_publication_requests_ = 0;
		bool refuse_targets_ = false;

		// Held by pointer so the doubles above are all constructed before the graph
		// binds references to them, without the member order being a puzzle.
		std::unique_ptr<Graph> graph_;
	};
}

#endif
