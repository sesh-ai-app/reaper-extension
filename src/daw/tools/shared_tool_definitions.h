// The two definitions more than one tool family needs, in the one place they are
// defined.
//
// ---------------------------------------------------------------------------
// Why this header exists, which is worth recording because the answer is a bug
//
// `routing_tools.h` and `track_structure_tools.h` each defined `missing_tool_input_code`
// and `folder_depth_repair`, in `sesh_ai::daw::tools`, independently. The two
// `folder_depth_repair` structs were field for field identical and the two codes were
// the same string, so neither was wrong about the concept — they were written by
// separate pieces of work that both needed it and neither could see the other.
//
// Nothing caught it, and the reason is instructive. Two definitions of one name in one
// namespace are an error only in a translation unit that sees both, and until the
// composition root there was none: `tests/daw/tools/routing_tools_test.cpp` and
// `tests/daw/tools/track_structure_tools_test.cpp` are separate translation units, and
// the extension's own sources reached one family at a time. The first file that has to
// register all 42 tools is the first file that includes both headers, and it does not
// compile — which is the composition root earning its keep before it routes anything.
//
// So the definitions move here and both headers include this one. The qualified names
// are unchanged — `sesh_ai::daw::tools::folder_depth_repair` and
// `sesh_ai::daw::tools::missing_tool_input_code` still name exactly what they named —
// so no caller and no test moves.
//
// The bar for adding anything else here is high. A definition shared by two families is
// one thing; a drawer for anything that looks reusable is how a tools directory grows a
// header every family includes and nobody can change. Both entries below are here
// because a translation unit that needs both families cannot compile otherwise, which
// is a specific and checkable reason.

#ifndef SESH_AI_DAW_TOOLS_SHARED_TOOL_DEFINITIONS_H
#define SESH_AI_DAW_TOOLS_SHARED_TOOL_DEFINITIONS_H

#include <string>
#include <string_view>

namespace sesh_ai::daw::tools {

	// A call whose `validated_input` was null.
	//
	// Reported rather than assumed away: `tool_call` carries the input as a pointer the
	// framework never dereferences, so a handler that dereferenced a null one would
	// crash REAPER rather than report anything. The MCP Tool Server validated the input
	// against the authoritative schema before it arrived, so reaching this means
	// something upstream is wrong — which is the case worth naming.
	inline constexpr std::string_view missing_tool_input_code{"missing_tool_input"};

	// `folderDepthRepair`. One track whose folder depth delta changed as a consequence
	// of the call rather than because the call named it.
	//
	// The Folder Invariant Keeper's repairs, in the shape the output schemas report
	// them. Routing and track structure both produce them — a bus inserted above a
	// folder's children and a track deleted out of the middle of a folder are the same
	// kind of consequence — which is why this is the shared definition rather than
	// either family's.
	struct folder_depth_repair
	{
		std::string guid;
		std::string name;
		int previous_folder_depth = 0;
		int folder_depth = 0;
	};

	inline bool operator==(const folder_depth_repair& left, const folder_depth_repair& right)
	{
		return left.guid == right.guid
			&& left.name == right.name
			&& left.previous_folder_depth == right.previous_folder_depth
			&& left.folder_depth == right.folder_depth;
	}

	inline bool operator!=(const folder_depth_repair& left, const folder_depth_repair& right)
	{
		return !(left == right);
	}

}

#endif
