// Test doubles for the Project Context Builder.
//
// Two substitutions, both for the same reason: this machine has no JSON dependency
// and no REAPER, and the things worth checking about this component are what it puts
// in a snapshot and when it decides to send one. Neither needs either.
//
// `StubDocument` stands in for nlohmann::json. It provides only the operations the
// serialiser performs on a document — `object()`, `array()`, `operator[]`,
// `push_back`, and assignment from a string, a double, an int, a bool, or a nested
// document — and it records what was written, which is what turns requirement 6.2
// into an assertion over real output rather than over a struct that happens not to
// carry the fields. That the real nlohmann::json satisfies the same calls is settled
// by the explicit instantiation in project_context_builder.cpp, compiled wherever the
// dependency is available.
//
// It keeps integers and doubles apart on purpose. `sampleRate`, `tempoChangeCount`,
// `markerCount`, `regionCount`, and a track's `index` are declared `"type":
// "integer"` in the schema while `tempo` and every position are `"type": "number"`,
// and a serialiser that wrote 48000.0 where an integer was wanted would pass every
// value assertion and fail validation. So the document remembers which it was told.
//
// `StubProjectContextSource` stands in for REAPER. It counts its own calls, because
// "the project is not walked on a tick where nothing is due" is a claim about calls
// rather than about output, and it is most of what the debounce is for.

#ifndef SESH_AI_TESTS_CONTEXT_PROJECT_CONTEXT_TEST_DOUBLES_H
#define SESH_AI_TESTS_CONTEXT_PROJECT_CONTEXT_TEST_DOUBLES_H

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <context/project_context_builder.h>

namespace sesh_ai_tests
{
	class StubDocument
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

		StubDocument() = default;

		static StubDocument object()
		{
			StubDocument document;
			document.kind_ = Kind::object_value;

			return document;
		}

		static StubDocument array()
		{
			StubDocument document;
			document.kind_ = Kind::array_value;

			return document;
		}

		// Vivifies to an object the way nlohmann::json does, so the serialiser can write
		// into a default-constructed document.
		StubDocument& operator[](const std::string& property_name)
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

		StubDocument& operator=(std::string value)
		{
			kind_ = Kind::string_value;
			string_value_ = std::move(value);

			return *this;
		}

		StubDocument& operator=(double value)
		{
			kind_ = Kind::number_value;
			number_value_ = value;

			return *this;
		}

		StubDocument& operator=(int value)
		{
			kind_ = Kind::integer_value;
			integer_value_ = value;

			return *this;
		}

		StubDocument& operator=(bool value)
		{
			kind_ = Kind::boolean_value;
			boolean_value_ = value;

			return *this;
		}

		void push_back(StubDocument element)
		{
			if (kind_ == Kind::null_value)
			{
				kind_ = Kind::array_value;
			}

			elements_.push_back(std::move(element));
		}

		void push_back(const std::string& element)
		{
			StubDocument element_document;
			element_document = element;

			push_back(std::move(element_document));
		}

		Kind kind() const { return kind_; }

		bool is_object() const { return kind_ == Kind::object_value; }
		bool is_array() const { return kind_ == Kind::array_value; }
		bool is_string() const { return kind_ == Kind::string_value; }
		bool is_integer() const { return kind_ == Kind::integer_value; }
		bool is_number() const { return kind_ == Kind::number_value; }
		bool is_boolean() const { return kind_ == Kind::boolean_value; }

		bool contains(const std::string& property_name) const
		{
			return properties_.count(property_name) > 0;
		}

		// In the order the serialiser wrote them, which is worth having: a diff of two
		// snapshots is readable only if the order is stable.
		const std::vector<std::string>& property_names() const { return property_order_; }

		const StubDocument& at(const std::string& property_name) const
		{
			return properties_.at(property_name);
		}

		const std::vector<StubDocument>& elements() const { return elements_; }

		const std::string& string_value() const { return string_value_; }
		double number_value() const { return number_value_; }
		int integer_value() const { return integer_value_; }
		bool boolean_value() const { return boolean_value_; }

	private:
		Kind kind_ = Kind::null_value;

		std::map<std::string, StubDocument> properties_;
		std::vector<std::string> property_order_;
		std::vector<StubDocument> elements_;

		std::string string_value_;
		double number_value_ = 0.0;
		int integer_value_ = 0;
		bool boolean_value_ = false;
	};

	// The outbound envelope, reduced to the three fields the builder writes.
	struct StubOutboundEnvelope
	{
		std::string type;
		std::string request_id;
		StubDocument payload;
	};

	// REAPER, replaced by a project the case wrote by hand.
	class StubProjectContextSource final : public sesh_ai::context::ProjectContextSource
	{
	public:
		sesh_ai::context::ProjectContextReading read_project_context() override
		{
			++project_reads;

			return reading;
		}

		int read_project_state_change_count() override
		{
			++change_count_reads;

			return change_count;
		}

		sesh_ai::context::ProjectContextReading reading{};
		int change_count = 0;

		std::size_t project_reads = 0;
		std::size_t change_count_reads = 0;
	};

	// A track with a valid GUID and nothing else going on, so a case sets only the
	// field it is about.
	inline sesh_ai::context::TrackReading plain_track(std::string guid, std::string name)
	{
		sesh_ai::context::TrackReading track;

		track.guid = std::move(guid);
		track.name = std::move(name);

		return track;
	}

	// Well-formed GUIDs, distinct, in REAPER's braced form.
	inline std::string test_guid(int ordinal)
	{
		static constexpr const char* hexadecimal_digits = "0123456789ABCDEF";

		std::string guid{"{00000000-0000-0000-0000-0000000000"};

		guid.push_back(hexadecimal_digits[(ordinal / 16) % 16]);
		guid.push_back(hexadecimal_digits[ordinal % 16]);
		guid.push_back('}');

		return guid;
	}

	// A globals reading that is already inside every schema bound, so a case that is
	// about one field does not have to set the other sixteen.
	inline sesh_ai::context::ProjectGlobalsReading plain_globals()
	{
		sesh_ai::context::ProjectGlobalsReading globals;

		globals.project_name = "Sesh Test Session";
		globals.tempo = 120.0;
		globals.time_signature_numerator = 4;
		globals.time_signature_denominator = 4;
		globals.sample_rate = 48000;
		globals.project_length = 180.5;
		globals.cursor_position = 12.25;
		globals.reaper_play_state = 0;
		globals.loop_start = 8.0;
		globals.loop_end = 16.0;
		globals.loop_enabled = false;
		globals.time_selection_present = false;
		globals.tempo_change_count = 0;
		globals.marker_count = 0;
		globals.region_count = 0;

		return globals;
	}

	// A readable one-track project.
	inline sesh_ai::context::ProjectContextReading plain_reading()
	{
		sesh_ai::context::ProjectContextReading reading;

		reading.readable = true;
		reading.globals = plain_globals();
		reading.tracks_in_project_order.push_back(plain_track(test_guid(1), "Kick"));

		reading.master_track.present = true;
		reading.master_track.guid = test_guid(0);
		reading.master_track.name = "MASTER";

		return reading;
	}
}

#endif
