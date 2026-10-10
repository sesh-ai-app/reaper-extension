// The one translation unit in this component that includes the REAPER SDK.
//
// Everything here is translation: an `fx_host` call in, one or a few REAPER C API calls
// out. No decisions — the range checks, the parameter matching, the match ranking, the
// removal ordering, and the read caps all live in tools/fx_tools.h, which is what makes
// them testable without REAPER. If a change to this file needs a branch on anything
// other than "did REAPER give me a value", the logic belongs on the other side of the
// seam.
//
// The function pointers are resolved from the registration table's `GetFunc` rather than
// through REAPERAPI_IMPLEMENT. See reaper_fx_host.h for why.

#include <daw/reaper_fx_host.h>

#include <reaper_plugin.h>

#include <cstddef>
#include <string>
#include <utility>

namespace
{
	// Buffer sizes. Each is comfortably past the matching `maxLength` in the output
	// schemas, so REAPER's own answer is what gets bounded by fx_tools.h rather than
	// being cut here first — a name truncated twice, once by a buffer and once by a
	// schema bound, is a name nobody can trace back.
	constexpr int fx_name_buffer_size = 1024;
	constexpr int parameter_name_buffer_size = 1024;
	constexpr int formatted_value_buffer_size = 512;

	// `guidToString` documents a 64-byte destination.
	constexpr std::size_t guid_buffer_size = 64;

	// `TrackFX_AddByName`'s `instantiate` argument. Zero queries without adding, 1 adds
	// only if the FX is not already on the chain, and -1 always adds a new instance.
	//
	// Always, deliberately: a producer asking for a second instance of the same
	// compressor gets one, and `add_fx`'s result would otherwise report the index of the
	// instance that was already there as though it had just been inserted.
	constexpr int always_instantiate_new_fx = -1;

	// `TrackFX_AddByName`'s `recFX` argument. False is the track's normal FX chain; true
	// is the input (record) chain, which no tool in the contract addresses.
	constexpr bool normal_fx_chain = false;

	// REAPER action IDs, addressed numerically because built-in actions are —
	// `NamedCommandLookup` resolves the `_RS...` identifiers scripts register, not these.
	//
	// These two are, with the render host's queue action, the values in this codebase
	// that cannot be checked from the SDK headers: the action list is REAPER's, not the
	// SDK's. They belong in the SDK validation step alongside ADR 0018's other open
	// items, and they are named here rather than written inline so that verifying them is
	// a one-line change.
	//
	// "Item: Apply track/take FX to items, multichannel output". Multichannel rather than
	// the mono variant, because the mono one folds a stereo item down and the producer
	// asked for their FX printed, not their width discarded.
	constexpr int apply_track_fx_to_items_command = 41993;

	// "Take: Crop to active take in items". Run after the apply when the call asked for
	// the existing take to be replaced: the apply always leaves the original take behind
	// the processed one, so discarding it is a second step. This is the step that makes
	// the operation unrecoverable outside REAPER's undo, which is why
	// `replacedExistingTake` is reported.
	constexpr int crop_to_active_take_command = 40131;
}

namespace sesh_ai::daw
{
	// The functions this component needs, and only those. Resolved once at construction;
	// every one of them is required, because an FX path missing any of them reports a
	// chain it cannot see or drops a write it claims to have made.
	struct ReaperFxHost::reaper_fx_api
	{
		int (*CountTracks)(ReaProject*) = nullptr;
		MediaTrack* (*GetTrack)(ReaProject*, int) = nullptr;
		MediaTrack* (*GetMasterTrack)(ReaProject*) = nullptr;
		bool (*GetSetMediaTrackInfo_String)(MediaTrack*, const char*, char*, bool) = nullptr;

		int (*TrackFX_GetCount)(MediaTrack*) = nullptr;
		bool (*TrackFX_GetFXName)(MediaTrack*, int, char*, int) = nullptr;
		bool (*TrackFX_GetEnabled)(MediaTrack*, int) = nullptr;
		void (*TrackFX_SetEnabled)(MediaTrack*, int, bool) = nullptr;
		bool (*TrackFX_GetOffline)(MediaTrack*, int) = nullptr;

		int (*TrackFX_GetNumParams)(MediaTrack*, int) = nullptr;
		bool (*TrackFX_GetParamName)(MediaTrack*, int, int, char*, int) = nullptr;
		double (*TrackFX_GetParam)(MediaTrack*, int, int, double*, double*) = nullptr;
		bool (*TrackFX_SetParam)(MediaTrack*, int, int, double) = nullptr;
		bool (*TrackFX_GetFormattedParamValue)(MediaTrack*, int, int, char*, int) = nullptr;

		int (*TrackFX_AddByName)(MediaTrack*, const char*, bool, int) = nullptr;
		bool (*TrackFX_Delete)(MediaTrack*, int) = nullptr;
		void (*TrackFX_CopyToTrack)(MediaTrack*, int, MediaTrack*, int, bool) = nullptr;

		bool (*EnumInstalledFX)(int, const char**, const char**) = nullptr;

		int (*CountTrackMediaItems)(MediaTrack*) = nullptr;
		MediaItem* (*GetTrackMediaItem)(MediaTrack*, int) = nullptr;
		bool (*GetSetMediaItemInfo_String)(MediaItem*, const char*, char*, bool) = nullptr;
		void (*SetMediaItemSelected)(MediaItem*, bool) = nullptr;
		void (*SelectAllMediaItems)(ReaProject*, bool) = nullptr;

		void (*Main_OnCommand)(int, int) = nullptr;
	};

	namespace
	{
		// Resolves one function and records the name when it is missing, so an unusable
		// host can say which functions REAPER did not supply rather than only that it is
		// unusable.
		template <typename FunctionPointer>
		void resolve_function(
			reaper_plugin_info_t* plugin_info,
			const char* name,
			FunctionPointer& destination,
			std::vector<std::string>& unresolved_names)
		{
			void* const resolved = plugin_info->GetFunc(name);

			if (resolved == nullptr)
			{
				unresolved_names.emplace_back(name);
				return;
			}

			destination = reinterpret_cast<FunctionPointer>(resolved);
		}

		std::string read_track_guid(
			const ReaperFxHost::reaper_fx_api& api,
			MediaTrack* track)
		{
			char guid_buffer[guid_buffer_size] = {};

			if (!api.GetSetMediaTrackInfo_String(track, "GUID", guid_buffer, false))
			{
				return {};
			}

			return std::string{guid_buffer};
		}

		std::string read_item_guid(
			const ReaperFxHost::reaper_fx_api& api,
			MediaItem* item)
		{
			char guid_buffer[guid_buffer_size] = {};

			if (!api.GetSetMediaItemInfo_String(item, "GUID", guid_buffer, false))
			{
				return {};
			}

			return std::string{guid_buffer};
		}

		// REAPER reports an FX's format through the prefix on its identifier — "VST3:",
		// "AU:", "JS:" — which is also the part a producer never says. Mapped to the
		// output schema's `format` enum here, and to `unknown` for a prefix this build
		// does not recognise rather than to a guess: a wrong format would have the agent
		// telling a producer why an FX will not load for a reason that is not the reason.
		std::string classify_fx_format(const std::string& identifier)
		{
			struct format_prefix
			{
				const char* prefix;
				const char* format;
			};

			// Longest-first where one prefix is a prefix of another, so "VST3i:" is not
			// read as "VST".
			static constexpr format_prefix prefixes[] = {
				{"VST3i:", "vst3"},
				{"VST3:", "vst3"},
				{"VSTi:", "vst"},
				{"VST:", "vst"},
				{"AUi:", "au"},
				{"AU:", "au"},
				{"CLAPi:", "clap"},
				{"CLAP:", "clap"},
				{"LV2i:", "lv2"},
				{"LV2:", "lv2"},
				{"DXi:", "dx"},
				{"DX:", "dx"},
				{"JS:", "js"},
			};

			for (const format_prefix& candidate : prefixes)
			{
				if (identifier.rfind(candidate.prefix, 0) == 0)
				{
					return candidate.format;
				}
			}

			return "unknown";
		}

		// REAPER marks an instrument with an `i` on the format prefix — "VSTi:",
		// "VST3i:", "AUi:". An instrument placed mid-chain replaces the signal rather
		// than processing it, so the distinction changes where the FX belongs.
		bool identifier_names_an_instrument(const std::string& identifier)
		{
			static constexpr const char* instrument_prefixes[] = {
				"VST3i:", "VSTi:", "AUi:", "CLAPi:", "LV2i:", "DXi:"};

			for (const char* const prefix : instrument_prefixes)
			{
				if (identifier.rfind(prefix, 0) == 0)
				{
					return true;
				}
			}

			return false;
		}

		// REAPER's own identifier carries the format prefix and, for some formats, a
		// trailing vendor in parentheses — "VST3: Pro-Q 3 (FabFilter)". The name REAPER
		// reports for the chain entry is the one to say to the producer, and the vendor is
		// worth separating out because two compressors can share a name.
		//
		// Read off the name REAPER gives rather than parsed out of the identifier: the
		// identifier's shape differs by format, and a parser that got it wrong would put
		// half a plugin name in the vendor field.
		std::string extract_vendor(const std::string& fx_name)
		{
			const std::size_t opening = fx_name.rfind(" (");

			if (opening == std::string::npos || fx_name.empty() || fx_name.back() != ')')
			{
				return {};
			}

			const std::size_t start = opening + 2;

			if (start >= fx_name.size() - 1)
			{
				return {};
			}

			return fx_name.substr(start, fx_name.size() - start - 1);
		}

		// GUID to `MediaTrack*`. The master track is checked too: FX on the master chain
		// are legitimate and the master does not appear in `CountTracks`/`GetTrack`, so
		// leaving it out would make the master's chain unreachable through every FX tool.
		//
		// A free function rather than a member, so that `MediaTrack*` never has to appear
		// in reaper_fx_host.h — which is what keeps that header free of the SDK and
		// compilable by the test target.
		MediaTrack* find_track(
			const ReaperFxHost::reaper_fx_api& api,
			ReaProject* project,
			const std::string& track_guid)
		{
			if (track_guid.empty())
			{
				return nullptr;
			}

			MediaTrack* const master = api.GetMasterTrack(project);

			if (master != nullptr && read_track_guid(api, master) == track_guid)
			{
				return master;
			}

			const int track_count = api.CountTracks(project);

			for (int track_index = 0; track_index < track_count; ++track_index)
			{
				MediaTrack* const track = api.GetTrack(project, track_index);

				if (track == nullptr)
				{
					continue;
				}

				if (read_track_guid(api, track) == track_guid)
				{
					return track;
				}
			}

			return nullptr;
		}

		// GUID to `MediaItem*`, across every track. The seam addresses an item by GUID
		// alone because `apply_fx_to_item` names one item, and whether that item is on the
		// track the call meant is checked on the other side of the seam against
		// `read_track_item_guids` — so this lookup deliberately does not filter by track.
		MediaItem* find_item(
			const ReaperFxHost::reaper_fx_api& api,
			ReaProject* project,
			const std::string& item_guid)
		{
			if (item_guid.empty())
			{
				return nullptr;
			}

			const int track_count = api.CountTracks(project);

			for (int track_index = 0; track_index < track_count; ++track_index)
			{
				MediaTrack* const track = api.GetTrack(project, track_index);

				if (track == nullptr)
				{
					continue;
				}

				const int item_count = api.CountTrackMediaItems(track);

				for (int item_index = 0; item_index < item_count; ++item_index)
				{
					MediaItem* const item = api.GetTrackMediaItem(track, item_index);

					if (item == nullptr)
					{
						continue;
					}

					if (read_item_guid(api, item) == item_guid)
					{
						return item;
					}
				}
			}

			return nullptr;
		}
	}

	ReaperFxHost::ReaperFxHost(reaper_plugin_info_t* plugin_info, ReaProject* project)
		: api_{std::make_unique<reaper_fx_api>()}
		, project_{project}
	{
		if (plugin_info == nullptr || plugin_info->GetFunc == nullptr)
		{
			unresolved_function_names_.emplace_back("GetFunc");
			return;
		}

		resolve_function(plugin_info, "CountTracks", api_->CountTracks, unresolved_function_names_);
		resolve_function(plugin_info, "GetTrack", api_->GetTrack, unresolved_function_names_);
		resolve_function(plugin_info, "GetMasterTrack", api_->GetMasterTrack, unresolved_function_names_);
		resolve_function(plugin_info, "GetSetMediaTrackInfo_String", api_->GetSetMediaTrackInfo_String, unresolved_function_names_);

		resolve_function(plugin_info, "TrackFX_GetCount", api_->TrackFX_GetCount, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_GetFXName", api_->TrackFX_GetFXName, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_GetEnabled", api_->TrackFX_GetEnabled, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_SetEnabled", api_->TrackFX_SetEnabled, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_GetOffline", api_->TrackFX_GetOffline, unresolved_function_names_);

		resolve_function(plugin_info, "TrackFX_GetNumParams", api_->TrackFX_GetNumParams, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_GetParamName", api_->TrackFX_GetParamName, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_GetParam", api_->TrackFX_GetParam, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_SetParam", api_->TrackFX_SetParam, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_GetFormattedParamValue", api_->TrackFX_GetFormattedParamValue, unresolved_function_names_);

		resolve_function(plugin_info, "TrackFX_AddByName", api_->TrackFX_AddByName, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_Delete", api_->TrackFX_Delete, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_CopyToTrack", api_->TrackFX_CopyToTrack, unresolved_function_names_);

		resolve_function(plugin_info, "EnumInstalledFX", api_->EnumInstalledFX, unresolved_function_names_);

		resolve_function(plugin_info, "CountTrackMediaItems", api_->CountTrackMediaItems, unresolved_function_names_);
		resolve_function(plugin_info, "GetTrackMediaItem", api_->GetTrackMediaItem, unresolved_function_names_);
		resolve_function(plugin_info, "GetSetMediaItemInfo_String", api_->GetSetMediaItemInfo_String, unresolved_function_names_);
		resolve_function(plugin_info, "SetMediaItemSelected", api_->SetMediaItemSelected, unresolved_function_names_);
		resolve_function(plugin_info, "SelectAllMediaItems", api_->SelectAllMediaItems, unresolved_function_names_);

		resolve_function(plugin_info, "Main_OnCommand", api_->Main_OnCommand, unresolved_function_names_);
	}

	ReaperFxHost::~ReaperFxHost() = default;

	bool ReaperFxHost::is_usable() const
	{
		return unresolved_function_names_.empty();
	}

	std::vector<std::string> ReaperFxHost::unresolved_function_names() const
	{
		return unresolved_function_names_;
	}

	std::vector<tools::fx_chain_entry> ReaperFxHost::read_track_fx(const std::string& track_guid)
	{
		std::vector<tools::fx_chain_entry> chain;

		if (!is_usable())
		{
			return chain;
		}

		MediaTrack* const track = find_track(*api_, project_, track_guid);

		if (track == nullptr)
		{
			return chain;
		}

		const int fx_count = api_->TrackFX_GetCount(track);
		chain.reserve(static_cast<std::size_t>(fx_count < 0 ? 0 : fx_count));

		for (int fx_index = 0; fx_index < fx_count; ++fx_index)
		{
			char name_buffer[fx_name_buffer_size] = {};
			api_->TrackFX_GetFXName(track, fx_index, name_buffer, fx_name_buffer_size);

			tools::fx_chain_entry entry;
			entry.index = fx_index;
			entry.name = std::string{name_buffer};

			// REAPER answers the inverse question: `TrackFX_GetEnabled` is true when the
			// FX is *not* bypassed.
			entry.bypassed = !api_->TrackFX_GetEnabled(track, fx_index);
			entry.offline = api_->TrackFX_GetOffline(track, fx_index);

			chain.push_back(std::move(entry));
		}

		return chain;
	}

	std::vector<tools::fx_parameter> ReaperFxHost::read_fx_parameters(
		const std::string& track_guid,
		int fx_index)
	{
		std::vector<tools::fx_parameter> parameters;

		MediaTrack* const track = is_usable() ? find_track(*api_, project_, track_guid) : nullptr;

		if (track == nullptr || fx_index < 0)
		{
			return parameters;
		}

		const int parameter_count = api_->TrackFX_GetNumParams(track, fx_index);
		parameters.reserve(static_cast<std::size_t>(parameter_count < 0 ? 0 : parameter_count));

		for (int parameter_index = 0; parameter_index < parameter_count; ++parameter_index)
		{
			char name_buffer[parameter_name_buffer_size] = {};
			api_->TrackFX_GetParamName(track, fx_index, parameter_index, name_buffer, parameter_name_buffer_size);

			double minimum_value = 0.0;
			double maximum_value = 0.0;
			const double value =
				api_->TrackFX_GetParam(track, fx_index, parameter_index, &minimum_value, &maximum_value);

			tools::fx_parameter parameter;
			parameter.index = parameter_index;
			parameter.name = std::string{name_buffer};
			parameter.value = value;
			parameter.minimum_value = minimum_value;
			parameter.maximum_value = maximum_value;

			char formatted_buffer[formatted_value_buffer_size] = {};

			if (api_->TrackFX_GetFormattedParamValue(
					track,
					fx_index,
					parameter_index,
					formatted_buffer,
					formatted_value_buffer_size))
			{
				std::string formatted{formatted_buffer};

				// Absent rather than present and empty: the output schema's
				// `formattedValue` is the string to quote to the producer, and quoting an
				// empty one would read as a value of nothing.
				if (!formatted.empty())
				{
					parameter.formatted_value = std::move(formatted);
				}
			}

			parameters.push_back(std::move(parameter));
		}

		return parameters;
	}

	std::vector<tools::installed_fx> ReaperFxHost::read_installed_fx()
	{
		std::vector<tools::installed_fx> installed;

		if (!is_usable())
		{
			return installed;
		}

		// `EnumInstalledFX` returns false at the end of the list. No cap here: the
		// filtering and the result cap belong to fx_tools.h, and capping the enumeration
		// would cap it before the search term had been applied — which would drop
		// matches rather than drop non-matches.
		for (int index = 0;; ++index)
		{
			const char* name = nullptr;
			const char* identifier = nullptr;

			if (!api_->EnumInstalledFX(index, &name, &identifier))
			{
				break;
			}

			tools::installed_fx candidate;
			candidate.identifier = identifier == nullptr ? std::string{} : std::string{identifier};
			candidate.name = name == nullptr ? std::string{} : std::string{name};
			candidate.vendor = extract_vendor(candidate.name);
			candidate.format = classify_fx_format(candidate.identifier);
			candidate.is_instrument = identifier_names_an_instrument(candidate.identifier);

			installed.push_back(std::move(candidate));
		}

		return installed;
	}

	std::vector<std::string> ReaperFxHost::read_track_item_guids(const std::string& track_guid)
	{
		std::vector<std::string> item_guids;

		MediaTrack* const track = is_usable() ? find_track(*api_, project_, track_guid) : nullptr;

		if (track == nullptr)
		{
			return item_guids;
		}

		// REAPER enumerates a track's items in timeline order, which is the order the
		// output schema asks `processedItemGuids` to be in.
		const int item_count = api_->CountTrackMediaItems(track);
		item_guids.reserve(static_cast<std::size_t>(item_count < 0 ? 0 : item_count));

		for (int item_index = 0; item_index < item_count; ++item_index)
		{
			MediaItem* const item = api_->GetTrackMediaItem(track, item_index);

			if (item == nullptr)
			{
				continue;
			}

			std::string item_guid = read_item_guid(*api_, item);

			if (item_guid.empty())
			{
				continue;
			}

			item_guids.push_back(std::move(item_guid));
		}

		return item_guids;
	}

	std::optional<int> ReaperFxHost::insert_fx(
		const std::string& track_guid,
		const std::string& fx_identifier,
		std::optional<int> chain_position)
	{
		MediaTrack* const track = is_usable() ? find_track(*api_, project_, track_guid) : nullptr;

		if (track == nullptr || fx_identifier.empty())
		{
			return std::nullopt;
		}

		// `TrackFX_AddByName` appends and has no insert-at form, so an insert is an
		// append followed by a move. The order matters: appending first means a failure to
		// load leaves the chain untouched, where reserving a slot first would leave a gap.
		const int appended_at =
			api_->TrackFX_AddByName(track, fx_identifier.c_str(), normal_fx_chain, always_instantiate_new_fx);

		if (appended_at < 0)
		{
			return std::nullopt;
		}

		if (!chain_position.has_value() || *chain_position == appended_at)
		{
			return appended_at;
		}

		// `is_move` true, and source and destination are the same track: REAPER's own way
		// of reordering a chain.
		api_->TrackFX_CopyToTrack(track, appended_at, track, *chain_position, true);

		return *chain_position;
	}

	bool ReaperFxHost::delete_fx(const std::string& track_guid, int fx_index)
	{
		MediaTrack* const track = is_usable() ? find_track(*api_, project_, track_guid) : nullptr;

		if (track == nullptr || fx_index < 0)
		{
			return false;
		}

		return api_->TrackFX_Delete(track, fx_index);
	}

	bool ReaperFxHost::set_fx_bypassed(const std::string& track_guid, int fx_index, bool bypassed)
	{
		MediaTrack* const track = is_usable() ? find_track(*api_, project_, track_guid) : nullptr;

		if (track == nullptr || fx_index < 0)
		{
			return false;
		}

		// `TrackFX_SetEnabled` returns nothing, so the write is confirmed by reading the
		// state back rather than by trusting it. An FX that did not take the change is
		// the case `set_fx_bypass`'s failure path exists for, and without the read-back
		// there would be nothing for it to detect.
		api_->TrackFX_SetEnabled(track, fx_index, !bypassed);

		const bool bypassed_now = !api_->TrackFX_GetEnabled(track, fx_index);

		return bypassed_now == bypassed;
	}

	bool ReaperFxHost::set_fx_parameter_value(
		const std::string& track_guid,
		int fx_index,
		int parameter_index,
		double value)
	{
		MediaTrack* const track = is_usable() ? find_track(*api_, project_, track_guid) : nullptr;

		if (track == nullptr || fx_index < 0 || parameter_index < 0)
		{
			return false;
		}

		return api_->TrackFX_SetParam(track, fx_index, parameter_index, value);
	}

	bool ReaperFxHost::apply_fx_to_item(const std::string& item_guid, bool replace_existing_take)
	{
		MediaItem* const item = is_usable() ? find_item(*api_, project_, item_guid) : nullptr;

		if (item == nullptr)
		{
			return false;
		}

		// REAPER's apply-FX actions operate on the item selection, so the selection is
		// narrowed to this one item. One item per call is what makes a per-item outcome
		// possible: an action run across a selection reports nothing about which member
		// of it failed.
		//
		// The producer's own selection is a casualty of this, and that is a real cost. It
		// is accepted rather than saved and restored because restoring it would need the
		// whole project's selection captured and rewritten around a destructive
		// operation, and a half-restored selection after a partial failure is a worse
		// state than a known one.
		api_->SelectAllMediaItems(project_, false);
		api_->SetMediaItemSelected(item, true);

		api_->Main_OnCommand(apply_track_fx_to_items_command, 0);

		if (replace_existing_take)
		{
			// The apply leaves the original take behind the processed one. Cropping to the
			// active take is what discards it, and it is the step that puts this beyond
			// anything but REAPER's undo.
			api_->Main_OnCommand(crop_to_active_take_command, 0);
		}

		// `Main_OnCommand` reports nothing, so there is no failure to detect here. The
		// truthful answer is that the action was dispatched: an apply REAPER declined
		// silently would be reported as a success, and the alternative — comparing take
		// counts or source file names across the call — would be this file making a
		// decision, which is what the seam exists to prevent. Recorded as a known gap
		// rather than papered over.
		return true;
	}
}
