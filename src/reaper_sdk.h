// The only way this codebase includes the REAPER SDK.
//
// Everything below the standard-library block is one line: `#include <reaper_plugin.h>`.
// The block above it is the entire point of the file, and it is load-bearing rather than
// tidy. Do not sort it, trim it, or move it below the SDK include.
//
// -----------------------------------------------------------------------------
// What goes wrong without it
//
// On macOS and Linux `reaper_plugin.h` reaches for WDL's swell
// (`sdk/reaper_plugin.h:49` -> `WDL/swell/swell.h:54` -> `WDL/swell/swell-types.h`),
// and swell-types.h:128 defines `max` and `min` as function-like macros:
//
//     #define max(x,y) ((x)<(y)?(y):(x))
//     #define min(x,y) ((x)<(y)?(x):(y))
//
// A function-like `min` macro is a wrecking ball through libstdc++. Any standard header
// parsed afterwards that declares a member named `min` or `max` has the declaration
// eaten before the compiler sees it, and the error is reported inside the standard
// library rather than anywhere near the include that caused it. GCC 11 on `ubuntu-22.04`
// reports hundreds of them, starting with:
//
//     /usr/include/c++/11/chrono:627:9: error: 'constexpr' static data member 'min'
//         must have an initializer
//
// `std::chrono::duration_values<Rep>::min()` is a static member function. The macro
// turns it into a data member named by a macro expansion, and the cascade runs through
// `<codecvt>`, `<locale>`, `<sstream>`, `<istream>`, `<streambuf>`, `<filesystem>` and
// the `bits/fs_*.h` family. GCC 13 on `ubuntu-24.04` happened to tolerate the tree as it
// stood; GCC 11 does not, and 22.04 is where the Linux leg has to build to honour the
// glibc 2.35 floor (ADR 0023).
//
// Worth being exact about who pulls those headers, because it is not swell. swell
// includes seven C headers and `<cstddef>`, all of them above its own macro definitions,
// so it damages nothing on its own way in. The trigger is a standard header named
// *after* the SDK by us or by a project header — most directly
// `daw/reaper_render_host.cpp`, which asked for `<filesystem>` two lines below
// `reaper_plugin.h`, and libstdc++'s `<filesystem>` pulls in exactly the set above.
//
// That is also why only one of the three CI legs ever saw it. MSVC takes `<windows.h>`
// at `sdk/reaper_plugin.h:43` and never reaches swell. libc++ defends itself: `<chrono>`
// includes `<__undef_macros>`, which undefines `min` and `max` if it finds them, so the
// macOS legs are immune to the ordering no matter what it is. libstdc++ has no such
// guard, which leaves Linux carrying the whole problem.
//
// -----------------------------------------------------------------------------
// Why pre-inclusion fixes it, and why `#undef` is the wrong instinct
//
// Include guards. A standard header that has already been fully parsed is a no-op the
// second time it is named, so its declarations of `min` and `max` were resolved while
// those words still meant nothing. Any later expansion of the macro has no declaration
// left to damage. That is the whole mechanism: ordering, not suppression.
//
// `#undef min` / `#undef max` after the SDK is the remedy REAPER extensions usually
// reach for, and it is strictly weaker here. It only helps a translation unit that has
// not yet included the offending standard header, so it has to be repeated in all
// fourteen, and it silently stops working the moment a project header included after the
// SDK pulls in `<chrono>`. Worse, it would hide the second half of the hazard below.
//
// swell's definition is guarded — `swell-types.h:127` reads
// `#if !defined(max) && !defined(WDL_NO_DEFINE_MINMAX) && !defined(NOMINMAX)` — so
// defining `WDL_NO_DEFINE_MINMAX` in the build would suppress the macros outright, and
// nothing in swell or `reaper_plugin.h` actually calls them. It was not chosen because it
// puts the fix in `CMakeLists.txt`, several hundred lines away from any file it rescues,
// where a reader hitting the chrono error has no path back to it. This header is the fix
// standing where the problem is.
//
// -----------------------------------------------------------------------------
// The macros are still defined after this header
//
// Pre-inclusion makes the standard library safe. It does not make the macros go away:
// they are live for the rest of the translation unit, and a call to `std::min` or
// `std::max` written *below* an include of this header will still be eaten. That is why
// `daw/tools/reaper_track_state_host.cpp` and `daw/tools/reaper_track_structure_host.cpp`
// spell `track_count > 0 ? track_count : 0` by hand, and why project headers that call
// `std::min` are included above this one rather than below it.
//
// -----------------------------------------------------------------------------
// Rules for changing this file
//
// 1. Adding a standard header to the list is the correct response to a new cascade.
//    Being generous is free: swell pulls none of these itself, and the ones that matter
//    are the ones *we* or our dependencies name after the SDK. Pre-including a header
//    nobody uses costs include order and nothing else.
// 2. Nothing here may be removed because it looks unused. Nothing in this file uses any
//    of it. That is not a sign the list is dead, it is the design.
// 3. No header may include this one. The SDK include path is attached to
//    `sesh_ai_extension` alone and deliberately not to `SESH_AI_INCLUDE_DIRECTORIES`,
//    which the Catch2 target shares — that is what keeps the suite runnable without
//    REAPER installed. Only the fourteen `*.cpp` files that genuinely talk to REAPER
//    include this, and a header that picked it up would take the whole suite down with
//    it.
// 4. `<codecvt>` is deprecated in C++17 and belongs in the list anyway. The deprecation
//    is attached to the entities inside it in every standard library that ships one
//    (libstdc++, libc++, and MSVC's STL4017), so it is diagnosed on use and not on
//    inclusion. This file uses nothing from it, and including it is silent under
//    `-Wall -Wextra`. It is also what libstdc++'s `<filesystem>` pulls in, which is how
//    it reached the GCC 11 error list in the first place.

#ifndef SESH_AI_REAPER_SDK_H
#define SESH_AI_REAPER_SDK_H

// Read the file header before touching this list.
#include <algorithm>
#include <chrono>
#include <codecvt>
#include <filesystem>
#include <istream>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <reaper_plugin.h>

#endif
