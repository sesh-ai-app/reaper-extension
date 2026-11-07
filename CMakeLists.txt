cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------------------------------
# Supported-platform floors
#
# The oldest macOS, Windows, and glibc the extension supports, declared here
# rather than inherited from whichever CI runner image GitHub happens to serve.
#
# That distinction is the whole reason this section exists. With nothing set, the
# binary's compatibility is whatever the build machine's SDK defaulted to — so
# the floor silently tracks the runner image, and the images move: `ubuntu-latest`
# migrates from Ubuntu 24.04 to 26.04 gradually between 19 October and
# 19 November 2026, which during that window means two different glibc versions
# for the same workflow with nothing in the log saying which. The runner labels
# are pinned in `.github/workflows/` for the same reason; this is the half that
# makes the floor a property of the build rather than of the machine.
#
# Each number, and why it is that number:
#
#   macOS 13.0 Ventura — set by CEF, not chosen. The pinned distribution is
#     Chromium 154 (SESH_AI_CEF_VERSION below), and Chromium 151 dropped macOS 12
#     Monterey, so Ventura is the oldest CEF permits. Moving lower would mean
#     downgrading four Chromium milestones and re-deriving six published digests.
#     Applies to both arm64 and x86_64; Intel macOS is a required target
#     (requirement 1.4).
#
#   glibc 2.35 — Ubuntu 22.04 LTS and Debian 12, the oldest distributions worth
#     supporting for a DAW extension. A glibc floor is not a flag: it is whatever
#     the machine that links the binary provides, which is why the Linux CI leg
#     builds on `ubuntu-22.04` rather than on a newer image.
#
#   Windows 10, as _WIN32_WINNT=0x0A00 — the oldest Windows still receiving
#     updates (10 22H2). Little in this codebase binds to it, which is the point:
#     stating it means a future API call above the floor fails at compile time
#     here rather than at load time on a producer's machine.
#
# CEF sets the real minimum on macOS and Linux, so declaring a floor is only half
# the job — `scripts/check-cef-platform-floor.sh` reads CEF's own requirements off
# the downloaded distribution and fails the build when they exceed these numbers.
# It parses the two values below out of this file, so these `set()` lines are the
# single declaration and the check cannot drift from them.
# ---------------------------------------------------------------------------

# A cache variable rather than a plain one, and set before project() rather than
# after: CMake runs its compiler checks during project(), and a deployment target
# arriving afterwards would not have been in force for them. The cache default
# leaves -DCMAKE_OSX_DEPLOYMENT_TARGET=<other> able to override it.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING
	"Oldest macOS the extension supports"
)

set(SESH_AI_LINUX_GLIBC_FLOOR "2.35" CACHE STRING
	"Oldest glibc the extension supports — Ubuntu 22.04 LTS and Debian 12"
)

set(SESH_AI_WINDOWS_NT_FLOOR "0x0A00" CACHE STRING
	"Oldest Windows the extension supports, as a _WIN32_WINNT value — 0x0A00 is Windows 10"
)

# Carrying the macOS floor into vcpkg's own port builds.
#
# This is a real gap rather than belt-and-braces. json-schema-validator and
# Catch2 are compiled from source by vcpkg, in separate CMake invocations that
# know nothing about this project's cache. If they are built with a higher minos
# than the extension declares, the dylib says 13.0, links cleanly, and then fails
# to load on 13 — on exactly the machines the floor existed for.
#
# vcpkg's documented control for this is the triplet variable
# VCPKG_OSX_DEPLOYMENT_TARGET, and the only way to set a triplet variable is to
# write a triplet file: vcpkg's maintainers say so directly, and the toolchain at
# the pinned baseline (scripts/toolchains/osx.cmake) never reads it from anywhere
# else. Carrying it that way would mean two overlay triplet files, an overlay path
# to plumb through, and new triplet names in the release workflow's matrix.
#
# So the environment variable is used instead, which is both less machinery and
# strictly broader. CMake documents MACOSX_DEPLOYMENT_TARGET as the default for
# CMAKE_OSX_DEPLOYMENT_TARGET when nothing sets it explicitly, and every port
# build vcpkg launches is a child process of this configure, so each inherits it.
# It also reaches ports that are not CMake-based, where
# VCPKG_OSX_DEPLOYMENT_TARGET is known not to apply at all (microsoft/vcpkg#10038)
# — clang reads the environment variable directly. None of this repository's three
# ports is autotools today, but the next one might be.
#
# Set unconditionally rather than only when absent, on purpose: an inherited value
# that disagreed with the cache variable would mean the extension and its
# dependencies compiled against different floors, which is the exact failure this
# is here to prevent.
#
# One caveat worth knowing before chasing it: the deployment target is not part of
# vcpkg's package ABI hash, so a binary-cache entry built before this was added
# can be restored without it. The CI cache key covers the baseline and the
# manifest, not this — if a floor violation ever appears on a cache hit, clear the
# vcpkg archives rather than looking for a bug here.
if(CMAKE_HOST_APPLE)
	set(ENV{MACOSX_DEPLOYMENT_TARGET} "${CMAKE_OSX_DEPLOYMENT_TARGET}")

	# vcpkg filters the environment it hands to port builds, and this list is how
	# a variable survives that. Appended rather than assigned, so a developer who
	# is already keeping other variables does not lose them.
	if(DEFINED ENV{VCPKG_KEEP_ENV_VARS})
		set(ENV{VCPKG_KEEP_ENV_VARS} "$ENV{VCPKG_KEEP_ENV_VARS};MACOSX_DEPLOYMENT_TARGET")
	else()
		set(ENV{VCPKG_KEEP_ENV_VARS} "MACOSX_DEPLOYMENT_TARGET")
	endif()
endif()

# ---------------------------------------------------------------------------
# Dependency toolchain (vcpkg)
#
# C++ dependencies are declared in vcpkg.json, which pins exact versions against
# a fixed registry baseline. CMake has to know about the vcpkg toolchain file
# before project() runs, so resolving it is the first thing that happens here.
#
# The toolchain is taken from, in order: an explicit -DCMAKE_TOOLCHAIN_FILE, the
# SESH_AI_VCPKG_ROOT cache variable, or the VCPKG_ROOT environment variable. On a
# machine with no vcpkg, configuration continues without it rather than failing —
# the test suite can still be built and run (see "Tests" below), and anything
# that genuinely needs a dependency says so with instructions instead of
# producing a confusing find_package error.
# ---------------------------------------------------------------------------

set(SESH_AI_VCPKG_ROOT "" CACHE PATH
	"Path to a vcpkg checkout used to resolve the dependencies in vcpkg.json"
)

set(SESH_AI_VCPKG_ACTIVE OFF)

if(CMAKE_TOOLCHAIN_FILE)
	# An explicitly supplied toolchain wins. It is only reported as vcpkg when it
	# actually is vcpkg, since the flag is also how cross-compilation toolchains
	# arrive.
	if(CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg\\.cmake$")
		set(SESH_AI_VCPKG_ACTIVE ON)
	endif()
else()
	if(SESH_AI_VCPKG_ROOT)
		set(sesh_ai_vcpkg_candidate_root "${SESH_AI_VCPKG_ROOT}")
	elseif(DEFINED ENV{VCPKG_ROOT})
		set(sesh_ai_vcpkg_candidate_root "$ENV{VCPKG_ROOT}")
	else()
		set(sesh_ai_vcpkg_candidate_root "")
	endif()

	if(sesh_ai_vcpkg_candidate_root
		AND EXISTS "${sesh_ai_vcpkg_candidate_root}/scripts/buildsystems/vcpkg.cmake")
		set(CMAKE_TOOLCHAIN_FILE
			"${sesh_ai_vcpkg_candidate_root}/scripts/buildsystems/vcpkg.cmake"
			CACHE FILEPATH "Dependency toolchain"
		)
		set(SESH_AI_VCPKG_ACTIVE ON)
	endif()

	unset(sesh_ai_vcpkg_candidate_root)
endif()

if(SESH_AI_VCPKG_ACTIVE)
	message(STATUS "Sesh AI extension: resolving dependencies through vcpkg — ${CMAKE_TOOLCHAIN_FILE}")
else()
	message(STATUS
		"Sesh AI extension: no vcpkg toolchain found. Dependencies will be looked up on the system. "
		"Point -DSESH_AI_VCPKG_ROOT=<path> or the VCPKG_ROOT environment variable at a vcpkg checkout "
		"to use the pinned versions in vcpkg.json. See DEPENDENCIES.md."
	)
endif()

# C as well as CXX, and not because this project has any C in it.
#
# CEF's own CMake support requires it. `cef_variables.cmake` probes compiler flags
# with `CHECK_C_COMPILER_FLAG`, which fails outright — "C: needs to be enabled
# before use" — in a project that declared only CXX. It is reached through
# `find_package(CEF)` further down. While the library could still be built without
# CEF, that was every build anyone ran, so the first CI run with a real distribution
# failed at configure time on every CEF leg.
#
# Declared here rather than as a conditional `enable_language(C)` beside
# `find_package(CEF)`. The conditional version is narrower and would keep a configure
# that builds only the Catch2 suite, and so never reaches CEF, from probing for a
# compiler it does not use. But the probe costs nothing — every toolchain carrying a
# C++ compiler carries a C one — and a FetchContent dependency that wants C would
# otherwise fail the same way, one dependency at a time.
project(sesh-ai-extension
	VERSION 0.1.0
	DESCRIPTION "Sesh AI — REAPER extension"
	LANGUAGES C CXX
)

# Objective-C++ on macOS, for the same reason C is in project(): a language a
# dependency needs has to be enabled before the dependency is reached.
#
# The project's own need is WDL's SWELL module stub, swell-modstub.mm, compiled
# into the library further down. But once OBJCXX is enabled, CMake treats every
# .mm source in the build as Objective-C++, including the two in CEF's wrapper
# (cef_scoped_library_loader_mac.mm and cef_scoped_sandbox_context_mac.mm). A
# directory processed before the language was enabled has no rule to compile it.
# Enabling it beside the module stub, after the wrapper's add_subdirectory(),
# configured cleanly and then failed at generate time, once per wrapper .mm file:
# "Missing variable is: CMAKE_OBJCXX_COMPILE_OBJECT". Only the macOS CEF legs
# broke: the builds without CEF that CI also ran then had no wrapper, and no other
# platform enables the language.
#
# A conditional enable_language() rather than a conditional LANGUAGES list,
# because APPLE is only reliably set once project() has loaded the platform.
# SESH_AI_PLATFORM_MACOS is derived from it further down, so it is not yet
# defined here.
if(APPLE)
	enable_language(OBJCXX)
endif()

# ---------------------------------------------------------------------------
# Language standard
#
# C++17 across every platform. No compiler extensions, so the same source
# compiles under AppleClang, MSVC, and GCC without dialect drift.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Compiler warnings
#
# An interface target rather than CMAKE_CXX_FLAGS, and that distinction is the
# whole point of it. The build compiles third-party code it did not write and
# cannot fix: libcef_dll_wrapper from the CEF distribution, socket.io-client-cpp
# through FetchContent, and Catch2 when vcpkg did not supply it. Warnings set
# globally would reach all of them, and the result is a wall of output nobody
# can act on — which is the same reasoning that marks the REAPER SDK includes
# SYSTEM further down, written there as "warnings nobody can fix train people to
# ignore warnings."
#
# So the flags are a usage requirement that exactly two targets opt into: the
# shared library and the Catch2 suite. The suite is included deliberately — it is
# two thirds of the C++ in this repository, and the house-style lint already
# holds it to the same standard as src/.
#
# -Wall -Wextra and nothing more, for now. -Wpedantic, -Wshadow, and
# -Wconversion are each defensible and each likely noisy here: the view-model
# serialiser narrows std::size_t to long long on purpose, and no src/*.cpp in
# this repository had been compiled by any toolchain when these flags were
# added. The honest order is to learn the volume from one CI run across
# AppleClang, MSVC, and GCC, then widen.
#
# Deliberately NOT -Werror. Fatal warnings are the right end state and the wrong
# starting point: turning them on before the count is known would mean the first
# build of 25 never-compiled translation units fails on style rather than on
# anything real, across three compilers at once. Decide after the first green
# build, and note that CI already keeps the legs independent, so a warning
# cleanup can land per platform.
# ---------------------------------------------------------------------------

add_library(sesh_ai_compiler_warnings INTERFACE)

if(MSVC)
	# /W4 rather than /Wall: MSVC's /Wall includes per-header informational
	# diagnostics from the standard library itself and is not usable as a project
	# setting. /W4 is the level that corresponds to -Wall -Wextra in practice.
	#
	# Limited to C and C++ because the library also compiles a resource script on
	# Windows (src/entry/panel_window.rc), and a compile option with no language
	# condition reaches the resource compiler too — under the Ninja and Makefile
	# generators at least — where /W4 is not an option rc.exe accepts.
	target_compile_options(sesh_ai_compiler_warnings INTERFACE $<$<COMPILE_LANGUAGE:C,CXX>:/W4>)
else()
	target_compile_options(sesh_ai_compiler_warnings INTERFACE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Platform floor, as a compile definition
#
# A sibling of the warnings target rather than a global definition, for the same
# reason the warnings are not global: the build compiles third-party code it did
# not write — libcef_dll_wrapper, socket.io-client-cpp, Catch2 when vcpkg did not
# supply it — and none of it should inherit this project's choices. So this is a
# usage requirement that the same two targets opt into, the shared library and the
# Catch2 suite.
#
# Only Windows has anything to put here. The macOS floor is a compiler and linker
# flag that CMake derives from CMAKE_OSX_DEPLOYMENT_TARGET, and the glibc floor is
# a property of the machine doing the linking, not a definition — which is why
# both are enforced against CEF in CI instead.
# ---------------------------------------------------------------------------

add_library(sesh_ai_platform_floor INTERFACE)

if(WIN32)
	# Both of these have to be in force before any system header is read, which is
	# what makes a compile definition the right instrument and a #define in one of
	# our headers the wrong one. <windows.h> is reached from three independent
	# directions here — CEF's own headers on the way to cef_types_wrappers.h,
	# reaper_plugin.h, and a handful of direct includes — so no header we own is
	# reliably first.
	#
	# _WIN32_WINNT is the floor: sdkddkver.h picks a default only when it is
	# undefined, so setting it means an API newer than Windows 10 is a compile
	# error rather than a binary that loads on the build machine and not on a
	# producer's.
	#
	# NOMINMAX is not a floor, and is here because this is the one place that
	# runs before the system headers. Without it <windows.h> defines min and max
	# as function-like macros, and CEF does not defend every use against them:
	#
	#   template <class struct_type>
	#   class CefStructBaseSimple : public struct_type {
	#     CefStructBaseSimple& operator=(const struct_type& r) {
	#       memcpy(static_cast<struct_type*>(this), &r,
	#              std::min(r.size, sizeof(struct_type)));
	#
	# `std::min(...)` there is unparenthesised, so the macro expands after the
	# `::` and MSVC reports `C2589: '(': illegal token on right side of '::'`
	# inside CEF's header while compiling src/ui/cef_browser_host.cpp. CEF writes
	# `(std::numeric_limits<uint32_t>::max)()` with the defensive parentheses a few
	# lines away, so this is an inconsistency upstream rather than a rule we broke.
	#
	# CEF avoids it on its own targets by putting NOMINMAX in CEF_COMPILER_DEFINES,
	# which `SET_COMMON_TARGET_PROPERTIES` applies to the wrapper and which nothing
	# in this file consumes — the same gap that left our target at C++17 while the
	# wrapper compiled at C++20. CEF pairs it with WIN32_LEAN_AND_MEAN, deliberately
	# not copied here: it removes declarations rather than adding them, including
	# the old winsock, and no local build can currently prove what that breaks.
	target_compile_definitions(sesh_ai_platform_floor INTERFACE
		_WIN32_WINNT=${SESH_AI_WINDOWS_NT_FLOOR}
		NOMINMAX
	)
endif()

# REAPER loads the extension as a shared library, so everything linked into it
# must be position-independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Keep binaries out of the per-target subdirectories the generators default to,
# so the install and packaging steps have one predictable place to look.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# CEF's own CMake support refuses to configure without a build type on
# single-configuration generators, and its binary distribution is laid out as
# Debug/ and Release/ directories. Defaulting rather than erroring keeps a plain
# `cmake -S . -B build` working.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# ---------------------------------------------------------------------------
# Platform detection
#
# The extension ships on macOS, Windows, and Linux. Each platform differs in
# how REAPER discovers the binary and how CEF is distributed, so detection
# happens once here and later configuration reads these variables rather than
# re-testing APPLE / WIN32 / UNIX inline.
# ---------------------------------------------------------------------------

set(SESH_AI_PLATFORM_MACOS OFF)
set(SESH_AI_PLATFORM_WINDOWS OFF)
set(SESH_AI_PLATFORM_LINUX OFF)

if(APPLE)
	set(SESH_AI_PLATFORM_MACOS ON)
	set(SESH_AI_PLATFORM_NAME "macOS")
	# REAPER loads UserPlugins/reaper_*.dylib on macOS.
	set(SESH_AI_EXTENSION_SUFFIX ".dylib")
elseif(WIN32)
	set(SESH_AI_PLATFORM_WINDOWS ON)
	set(SESH_AI_PLATFORM_NAME "Windows")
	# REAPER loads UserPlugins/reaper_*.dll on Windows.
	set(SESH_AI_EXTENSION_SUFFIX ".dll")
elseif(UNIX)
	set(SESH_AI_PLATFORM_LINUX ON)
	set(SESH_AI_PLATFORM_NAME "Linux")
	# REAPER loads UserPlugins/reaper_*.so on Linux.
	set(SESH_AI_EXTENSION_SUFFIX ".so")
else()
	message(FATAL_ERROR
		"Unsupported platform. The Sesh AI extension targets macOS, Windows, and Linux."
	)
endif()

# REAPER only scans for shared libraries whose file name begins with `reaper_`,
# so the output name is a hard requirement rather than a preference.
set(SESH_AI_EXTENSION_OUTPUT_NAME "reaper_sesh_ai")

# The CEF subprocess helper's executable name, without a platform suffix. The other half
# of this is `subprocess_executable_name` in `cef_platform_configuration_for`
# (src/ui/ui_host.h), which is where the extension looks for it at run time. Set here,
# outside the CEF section, because the Catch2 suite is handed it as a compile definition
# and asserts the two agree, and the suite is built with or without CEF.
#
# macOS gets the name of the executable inside `Sesh AI Helper.app/Contents/MacOS/`,
# because CEF derives the Alerts, GPU, Plugin, and Renderer helper names from it.
if(SESH_AI_PLATFORM_MACOS)
	set(SESH_AI_CEF_SUBPROCESS_HELPER_OUTPUT_NAME "Sesh AI Helper")
else()
	set(SESH_AI_CEF_SUBPROCESS_HELPER_OUTPUT_NAME "sesh_ai_cef_helper")
endif()

# The directory Vite's output is installed as, beside the extension binary. The other
# half is `application_asset_directory_name` in src/entry/panel_asset_directory.h, which
# is where the extension looks for it at run time. Set here for the same reason as the
# helper name above: the Catch2 suite is handed it and asserts the two agree.
set(SESH_AI_APPLICATION_ASSET_DIRECTORY_NAME "sesh_ai_ui")

message(STATUS "Sesh AI extension: configuring for ${SESH_AI_PLATFORM_NAME}")
message(STATUS "Sesh AI extension: output name ${SESH_AI_EXTENSION_OUTPUT_NAME}${SESH_AI_EXTENSION_SUFFIX}")

# The floor this build is actually producing, reported rather than assumed. The
# complaint the floors answer is that nothing in a build log said what the binary
# would run on; a line per platform is the cheap half of fixing that.
if(SESH_AI_PLATFORM_MACOS)
	message(STATUS
		"Sesh AI extension: macOS deployment target ${CMAKE_OSX_DEPLOYMENT_TARGET} "
		"(vcpkg ports inherit it through MACOSX_DEPLOYMENT_TARGET)"
	)
elseif(SESH_AI_PLATFORM_WINDOWS)
	message(STATUS "Sesh AI extension: Windows floor _WIN32_WINNT=${SESH_AI_WINDOWS_NT_FLOOR} (Windows 10)")
else()
	message(STATUS
		"Sesh AI extension: glibc floor ${SESH_AI_LINUX_GLIBC_FLOOR} — a property of the linking "
		"machine, so build on ubuntu-22.04 or a container with that glibc"
	)
endif()

# ---------------------------------------------------------------------------
# Dependencies: JSON and schema validation
#
# nlohmann/json parses every envelope; nlohmann/json-schema-validator is the C++
# half of "one schema, two validators" from the schema-bundles steering. Exact
# versions are pinned in vcpkg.json — the version arguments are deliberately
# omitted here so that a system-installed copy on a developer machine is usable
# without argument, while CI gets the pinned build through vcpkg.
#
# The lookups are QUIET because nothing is compiled against them yet. The moment
# there are sources, the guard further down turns a missing dependency into a
# hard error with instructions rather than a link failure.
# ---------------------------------------------------------------------------

find_package(nlohmann_json CONFIG QUIET)
find_package(nlohmann_json_schema_validator CONFIG QUIET)

if(nlohmann_json_FOUND)
	message(STATUS "Sesh AI extension: nlohmann/json ${nlohmann_json_VERSION}")
else()
	message(STATUS "Sesh AI extension: nlohmann/json not found (declared in vcpkg.json)")
endif()

if(nlohmann_json_schema_validator_FOUND)
	message(STATUS "Sesh AI extension: nlohmann/json-schema-validator found")
else()
	message(STATUS "Sesh AI extension: nlohmann/json-schema-validator not found (declared in vcpkg.json)")
endif()

# ---------------------------------------------------------------------------
# Dependency: Socket.IO client
#
# socketio/socket.io-client-cpp on its 3.x line, which speaks to a Socket.IO v4
# server — what the Sesh server runs.
#
# This one is NOT in vcpkg.json, and the reason is worth recording. The
# `socket-io-client` port was removed from the vcpkg registry along with its
# `websocketpp` dependency, which upstream no longer maintains. Neither port
# exists at the baseline pinned in vcpkg.json, and vcpkg fails manifest
# resolution with an opaque error when a manifest names a port the baseline has
# dropped. So the client is acquired from source at a pinned commit instead.
#
# The pin is an untagged `master` commit rather than a release, and that is
# deliberate. Requirement 3.1 puts the access token, `clientType`, and
# `protocolVersion` in the Socket.IO handshake `auth`, and the server reads
# `socket.handshake.auth` with no fallback to the query string or the headers.
# The `sio::client::connect` overloads taking `const message::ptr& auth` landed
# on master in upstream #335 (2021-11-23) and have never been released — 3.1.0,
# still the newest tag, predates them by six weeks. So tag 3.1.0 cannot satisfy
# requirement 3.1 at all. The commit below is the tip of master as of 2025-08-28,
# read at that commit to confirm `src/sio_client.h` declares the four-argument
# overload this extension calls. DEPENDENCIES.md records the supply-chain
# position that comes with pinning past a release.
#
# Pinned to the full 40-character commit SHA, never a branch and never a tag: a
# branch pin is not reproducible and fails the OSSF Scorecard
# Pinned-Dependencies check.
#
# Declaring is free — nothing is downloaded until a FetchContent_MakeAvailable
# call, which waits until there are transport sources to link against. Leaving it
# declared but unpopulated is what lets a contributor with no vcpkg configure,
# build, and run the whole Catch2 suite without touching the network. The
# TLS-enabled target also needs OpenSSL, which the transport work will add.
# ---------------------------------------------------------------------------

set(SESH_AI_SOCKET_IO_CLIENT_VERSION "3.1.0+3b7be7e (unreleased master)")
set(SESH_AI_SOCKET_IO_CLIENT_COMMIT "3b7be7e4173b5bdeed393966e3274f65d513a280")

include(FetchContent)

FetchContent_Declare(sesh_ai_socket_io_client
	GIT_REPOSITORY "https://github.com/socketio/socket.io-client-cpp.git"
	GIT_TAG "${SESH_AI_SOCKET_IO_CLIENT_COMMIT}"
	GIT_SUBMODULES_RECURSE ON
	GIT_SHALLOW OFF
)

message(STATUS
	"Sesh AI extension: Socket.IO client pinned to socket.io-client-cpp ${SESH_AI_SOCKET_IO_CLIENT_VERSION} "
	"commit ${SESH_AI_SOCKET_IO_CLIENT_COMMIT} "
	"(declared, populated once there are transport sources — see DEPENDENCIES.md)"
)

# ---------------------------------------------------------------------------
# Dependency: CEF binary distribution
#
# CEF ships as a large per-platform binary distribution rather than a package in
# any registry, so it is located rather than resolved. Search order:
#
#   1. -DSESH_AI_CEF_ROOT=<path>
#   2. the CEF_ROOT environment variable
#   3. third_party/cef/cef_binary_<version>_<platform>_minimal in this repository
#
# A directory counts as a distribution when it carries cmake/FindCEF.cmake, the
# module CEF ships for exactly this purpose. When none of the three holds one, the
# pinned archive is downloaded into (3) and checked against its published hash.
#
# The shared library is always built against CEF, because a library without it
# loads in REAPER and can never show its panel. The Catch2 suite includes no CEF
# and does not need it. So this section only declares and locates. The download,
# find_package(CEF), and the wrapper happen in "The shared library's
# prerequisites" further down, once every other prerequisite of the library is
# known to be present. There a distribution that cannot be located or downloaded
# stops configuration with instructions, and a configure that is not creating the
# library, such as one without vcpkg, never reaches the network for it.
#
# SESH_AI_DOWNLOAD_CEF is ON by default, unlike the REAPER SDK's clone below. The
# archive is hundreds of megabytes, but it is fetched only by a configure that is
# about to build the extension and cannot do so without it. OFF forbids the
# download, and then one of the three places has to hold a distribution already.
# ---------------------------------------------------------------------------

set(SESH_AI_CEF_VERSION "154.0.28+g564dd6c+chromium-154.0.8037.58" CACHE STRING
	"CEF binary distribution version to locate or download"
)

set(SESH_AI_CEF_ROOT "" CACHE PATH
	"Path to an extracted CEF binary distribution"
)

option(SESH_AI_DOWNLOAD_CEF
	"Download the pinned CEF binary distribution when the shared library needs it and it is not already present" ON
)

# CEF names its distributions by platform and architecture. The extension builds
# for the six combinations REAPER and CEF have in common.
if(SESH_AI_PLATFORM_MACOS)
	if(CMAKE_OSX_ARCHITECTURES)
		set(sesh_ai_cef_architecture "${CMAKE_OSX_ARCHITECTURES}")
	else()
		set(sesh_ai_cef_architecture "${CMAKE_SYSTEM_PROCESSOR}")
	endif()

	if(sesh_ai_cef_architecture MATCHES "x86_64")
		set(SESH_AI_CEF_PLATFORM "macosx64")
	else()
		set(SESH_AI_CEF_PLATFORM "macosarm64")
	endif()
elseif(SESH_AI_PLATFORM_WINDOWS)
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
		set(SESH_AI_CEF_PLATFORM "windowsarm64")
	else()
		set(SESH_AI_CEF_PLATFORM "windows64")
	endif()
else()
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
		set(SESH_AI_CEF_PLATFORM "linuxarm64")
	else()
		set(SESH_AI_CEF_PLATFORM "linux64")
	endif()
endif()

unset(sesh_ai_cef_architecture)

# SHA1 digests as published in the CEF build index for
# SESH_AI_CEF_VERSION. Changing the version means replacing these too — the
# download refuses to proceed on a mismatch rather than trusting the CDN.
set(SESH_AI_CEF_ARCHIVE_SHA1_macosarm64 "f7c50c2719deb8aa0c78de517c1228d0b8c3fced")
set(SESH_AI_CEF_ARCHIVE_SHA1_macosx64 "60bb4e9d525d40a8ee721c55557a9edd058873dc")
set(SESH_AI_CEF_ARCHIVE_SHA1_windows64 "34a32277db8addfa355a009ed16c8dae939daed1")
set(SESH_AI_CEF_ARCHIVE_SHA1_windowsarm64 "6d56fae32e0ee61e8aa0b22af3f88913dc84e758")
set(SESH_AI_CEF_ARCHIVE_SHA1_linux64 "5dfe49d73b0d3635354c705938e26074f287b1de")
set(SESH_AI_CEF_ARCHIVE_SHA1_linuxarm64 "07493740447bfe59069f1d5e3ec1682f8a101900")

set(SESH_AI_CEF_DISTRIBUTION_NAME "cef_binary_${SESH_AI_CEF_VERSION}_${SESH_AI_CEF_PLATFORM}_minimal")
set(SESH_AI_CEF_VENDOR_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cef")
set(SESH_AI_CEF_DEFAULT_ROOT "${SESH_AI_CEF_VENDOR_DIRECTORY}/${SESH_AI_CEF_DISTRIBUTION_NAME}")

function(sesh_ai_locate_cef_distribution output_variable)
	set(candidate_roots "")

	if(SESH_AI_CEF_ROOT)
		list(APPEND candidate_roots "${SESH_AI_CEF_ROOT}")
	endif()

	if(DEFINED ENV{CEF_ROOT})
		list(APPEND candidate_roots "$ENV{CEF_ROOT}")
	endif()

	list(APPEND candidate_roots "${SESH_AI_CEF_DEFAULT_ROOT}")

	foreach(candidate_root IN LISTS candidate_roots)
		if(EXISTS "${candidate_root}/cmake/FindCEF.cmake")
			set(${output_variable} "${candidate_root}" PARENT_SCOPE)
			return()
		endif()
	endforeach()

	set(${output_variable} "" PARENT_SCOPE)
endfunction()

sesh_ai_locate_cef_distribution(SESH_AI_CEF_LOCATED_ROOT)

# ---------------------------------------------------------------------------
# Dependency: REAPER extension SDK
#
# The plugin boundary itself — reaper_plugin.h for the registration table REAPER
# hands the extension at load, and reaper_plugin_functions.h for the C API the
# tool implementations call. Cockos distributes it freely as a header-only
# mini-SDK, so it is located and optionally acquired rather than resolved from a
# registry, the same way CEF is above.
#
# Two repositories, because the headers need each other. On macOS and Linux
# reaper_plugin.h includes "../WDL/swell/swell.h" — a path relative to its own
# location — so a usable SDK root is the merged layout the SDK's own README
# describes:
#
#   <root>/sdk/     from justinfrankel/reaper-sdk
#   <root>/WDL/     the inner WDL directory of justinfrankel/WDL
#
# On Windows reaper_plugin.h includes <windows.h> instead and WDL is not needed,
# but the acquisition produces the same layout on all three platforms so that a
# developer moving between them does not meet a different tree.
#
# Search order matches CEF's: -DSESH_AI_REAPER_SDK_ROOT, the REAPER_SDK_ROOT
# environment variable, then third_party/reaper-sdk in this repository. With
# -DSESH_AI_DOWNLOAD_REAPER_SDK=ON the pinned commits are cloned into (3).
#
# Nothing large is committed here: third_party/ is ignored, and the SDK arrives at
# configure time or not at all. It is ~15 MB of headers, so the download is cheap
# enough that it could have been unconditional — it is opt-in anyway, because a
# build that reaches the network without being asked is a worse default than one
# that explains what it is missing.
# ---------------------------------------------------------------------------

set(SESH_AI_REAPER_SDK_VERSION "7.80" CACHE STRING
	"REAPER version whose extension SDK headers are pinned below"
)

set(SESH_AI_REAPER_SDK_COMMIT "08e885d14c87c2973ec301d220ff9fe6b6a7cbeb")
set(SESH_AI_WDL_COMMIT "2775dda3329de19dd36f7edac63eed9f7ed9e490")

set(SESH_AI_REAPER_SDK_ROOT "" CACHE PATH
	"Path to a REAPER extension SDK tree laid out as <root>/sdk and <root>/WDL"
)

option(SESH_AI_DOWNLOAD_REAPER_SDK
	"Clone the pinned REAPER extension SDK headers when they are not already present" OFF
)

option(SESH_AI_REQUIRE_REAPER_SDK
	"Fail configuration when no REAPER extension SDK can be located" OFF
)

set(SESH_AI_REAPER_SDK_VENDOR_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(SESH_AI_REAPER_SDK_DEFAULT_ROOT "${SESH_AI_REAPER_SDK_VENDOR_DIRECTORY}/reaper-sdk")

# A directory counts as an SDK when it carries both headers the extension needs,
# plus swell on the platforms where reaper_plugin.h reaches for it. Checking the
# headers rather than the directory means a half-finished clone is reported as
# missing instead of failing later in a compile.
function(sesh_ai_locate_reaper_sdk output_variable)
	set(candidate_roots "")

	if(SESH_AI_REAPER_SDK_ROOT)
		list(APPEND candidate_roots "${SESH_AI_REAPER_SDK_ROOT}")
	endif()

	if(DEFINED ENV{REAPER_SDK_ROOT})
		list(APPEND candidate_roots "$ENV{REAPER_SDK_ROOT}")
	endif()

	list(APPEND candidate_roots "${SESH_AI_REAPER_SDK_DEFAULT_ROOT}")

	foreach(candidate_root IN LISTS candidate_roots)
		if(NOT EXISTS "${candidate_root}/sdk/reaper_plugin.h")
			continue()
		endif()

		if(NOT EXISTS "${candidate_root}/sdk/reaper_plugin_functions.h")
			continue()
		endif()

		if(NOT SESH_AI_PLATFORM_WINDOWS AND NOT EXISTS "${candidate_root}/WDL/swell/swell.h")
			continue()
		endif()

		set(${output_variable} "${candidate_root}" PARENT_SCOPE)
		return()
	endforeach()

	set(${output_variable} "" PARENT_SCOPE)
endfunction()

function(sesh_ai_clone_repository_at_commit repository_url repository_commit destination_directory)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" clone --quiet "${repository_url}" "${destination_directory}"
		RESULT_VARIABLE clone_result
		ERROR_VARIABLE clone_error
	)

	if(NOT clone_result EQUAL 0)
		message(FATAL_ERROR "Sesh AI extension: cloning ${repository_url} failed — ${clone_error}")
	endif()

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" -C "${destination_directory}" checkout --quiet "${repository_commit}"
		RESULT_VARIABLE checkout_result
		ERROR_VARIABLE checkout_error
	)

	if(NOT checkout_result EQUAL 0)
		message(FATAL_ERROR
			"Sesh AI extension: checking out ${repository_commit} in ${repository_url} failed — ${checkout_error}"
		)
	endif()

	# What is wanted is a pinned header tree, not a checkout to develop in. The
	# commits are recorded above, so dropping the history keeps the vendored
	# directory to the headers.
	file(REMOVE_RECURSE "${destination_directory}/.git")
endfunction()

sesh_ai_locate_reaper_sdk(SESH_AI_REAPER_SDK_LOCATED_ROOT)

if(NOT SESH_AI_REAPER_SDK_LOCATED_ROOT AND SESH_AI_DOWNLOAD_REAPER_SDK)
	find_package(Git REQUIRED)

	message(STATUS
		"Sesh AI extension: cloning the REAPER extension SDK headers for REAPER ${SESH_AI_REAPER_SDK_VERSION}"
	)

	# Assembled in a scratch directory and moved into place as a whole, so an
	# interrupted clone cannot leave behind a tree that passes the locate check.
	set(sesh_ai_reaper_sdk_scratch "${SESH_AI_REAPER_SDK_VENDOR_DIRECTORY}/reaper-sdk-acquire")

	file(REMOVE_RECURSE "${sesh_ai_reaper_sdk_scratch}")
	file(MAKE_DIRECTORY "${sesh_ai_reaper_sdk_scratch}")

	sesh_ai_clone_repository_at_commit(
		"https://github.com/justinfrankel/reaper-sdk.git"
		"${SESH_AI_REAPER_SDK_COMMIT}"
		"${sesh_ai_reaper_sdk_scratch}/reaper-sdk"
	)

	sesh_ai_clone_repository_at_commit(
		"https://github.com/justinfrankel/WDL.git"
		"${SESH_AI_WDL_COMMIT}"
		"${sesh_ai_reaper_sdk_scratch}/wdl"
	)

	# The WDL repository's own root holds a single WDL directory, which is the one
	# reaper_plugin.h resolves "../WDL" to.
	file(RENAME
		"${sesh_ai_reaper_sdk_scratch}/wdl/WDL"
		"${sesh_ai_reaper_sdk_scratch}/reaper-sdk/WDL"
	)

	file(REMOVE_RECURSE "${SESH_AI_REAPER_SDK_DEFAULT_ROOT}")
	file(RENAME "${sesh_ai_reaper_sdk_scratch}/reaper-sdk" "${SESH_AI_REAPER_SDK_DEFAULT_ROOT}")
	file(REMOVE_RECURSE "${sesh_ai_reaper_sdk_scratch}")

	unset(sesh_ai_reaper_sdk_scratch)

	sesh_ai_locate_reaper_sdk(SESH_AI_REAPER_SDK_LOCATED_ROOT)

	if(NOT SESH_AI_REAPER_SDK_LOCATED_ROOT)
		message(FATAL_ERROR
			"Sesh AI extension: the REAPER SDK clone did not produce a usable tree at "
			"${SESH_AI_REAPER_SDK_DEFAULT_ROOT}. Assemble it by hand and pass "
			"-DSESH_AI_REAPER_SDK_ROOT=<path>. See DEPENDENCIES.md."
		)
	endif()
endif()

set(SESH_AI_REAPER_SDK_FOUND OFF)

if(SESH_AI_REAPER_SDK_LOCATED_ROOT)
	set(SESH_AI_REAPER_SDK_FOUND ON)

	# Only the sdk/ directory goes on the include path. WDL is reached through
	# reaper_plugin.h's own relative include, which is how the SDK expects it, and
	# leaving the rest of WDL off the include path keeps its many headers from
	# shadowing anything.
	#
	# Kept out of SESH_AI_INCLUDE_DIRECTORIES on purpose. The test target must not
	# be able to reach these headers: that is what forces every REAPER dependency
	# behind an interface the suite can substitute, and it turns a header that
	# leaks the SDK into a build failure rather than a test nobody can run away
	# from REAPER.
	set(SESH_AI_REAPER_SDK_INCLUDE_DIRECTORIES "${SESH_AI_REAPER_SDK_LOCATED_ROOT}/sdk")

	message(STATUS
		"Sesh AI extension: REAPER extension SDK (REAPER ${SESH_AI_REAPER_SDK_VERSION}) at "
		"${SESH_AI_REAPER_SDK_LOCATED_ROOT}"
	)
elseif(SESH_AI_REQUIRE_REAPER_SDK)
	message(FATAL_ERROR
		"Sesh AI extension: no REAPER extension SDK found, and SESH_AI_REQUIRE_REAPER_SDK is ON. "
		"Configure with -DSESH_AI_DOWNLOAD_REAPER_SDK=ON, or pass -DSESH_AI_REAPER_SDK_ROOT=<path> "
		"pointing at a tree laid out as <root>/sdk and <root>/WDL. See DEPENDENCIES.md."
	)
else()
	message(STATUS
		"Sesh AI extension: no REAPER extension SDK found. The shared library cannot be built. "
		"Configure with -DSESH_AI_DOWNLOAD_REAPER_SDK=ON to clone the pinned headers into "
		"third_party/reaper-sdk, or pass -DSESH_AI_REAPER_SDK_ROOT=<path>. See DEPENDENCIES.md."
	)
endif()

# ---------------------------------------------------------------------------
# Vendored schema bundle
#
# schemas/ holds a generated copy of the protocol schemas the extension sends,
# receives, or produces, plus MANIFEST.json carrying a SHA-256 per file and a
# protocolVersion derived from those digests. The monorepo's bundler writes it;
# nothing here edits it (ADR 0019).
#
# Two things happen with the manifest, and the split matters.
#
# At configure time it is read for the protocolVersion, which is baked into a
# generated header so the Transport Client can put it in the handshake `auth`
# without parsing JSON at connection time.
#
# At build time every file is hashed and compared against the manifest, and a
# mismatch fails the build. Requirement 20.3 is explicit that this is never a
# runtime condition, and requirement 20.2 that it needs no network — this
# repository's CI cannot see the monorepo, so intact is what it can check and
# fresh is what the monorepo checks. VerifySchemaBundle.cmake has the detail.
# ---------------------------------------------------------------------------

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(SchemaBundle)

set(SESH_AI_SCHEMA_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/schemas")
set(SESH_AI_SCHEMA_MANIFEST "${SESH_AI_SCHEMA_DIRECTORY}/MANIFEST.json")

sesh_ai_read_schema_manifest("${SESH_AI_SCHEMA_MANIFEST}"
	SESH_AI_SCHEMA_PROTOCOL_VERSION
	SESH_AI_SCHEMA_RELATIVE_PATHS
	SESH_AI_SCHEMA_EXPECTED_DIGESTS
)

# Only the protocolVersion and the file count are wanted here — the digests are
# the build step's business. One reader serves both so the two halves cannot
# disagree about the manifest's shape, which is worth more than trimming an
# unused return.
list(LENGTH SESH_AI_SCHEMA_RELATIVE_PATHS SESH_AI_SCHEMA_FILE_COUNT)

# configure_file tracks its own input, but the manifest is read with file(READ),
# so CMake has to be told that editing it invalidates the configuration.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SESH_AI_SCHEMA_MANIFEST}")

set(SESH_AI_GENERATED_INCLUDE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")

# Generated under a domain directory so it is included the same way as everything
# else the transport layer owns: `#include <transport/schema_bundle_version.h>`.
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/schema_bundle_version.h.in"
	"${SESH_AI_GENERATED_INCLUDE_DIRECTORY}/transport/schema_bundle_version.h"
	@ONLY
)

# ALL, so a plain `cmake --build` verifies. This is the build step requirement
# 20.2 asks for — deliberately not a ctest test, because a suite someone can
# choose not to run is not a build gate.
#
# It runs on every build rather than behind a stamp file, which is the unusual
# choice here and the right one twice over. A check over file *contents* whose
# skip condition is a *timestamp* is only as strong as the timestamps, and the
# thing being defended against is a file that was replaced. And a stamp needs the
# 65 schema files in DEPENDS, which turns a deleted one into make reporting no
# rule to build it — an error that says nothing about the bundle. The cost is one
# short-lived process and 59 hashes of small files per build, which does not
# register beside a single C++ translation unit.
add_custom_target(sesh_ai_verify_schema_bundle ALL
	COMMAND "${CMAKE_COMMAND}"
		"-DSESH_AI_SCHEMA_DIRECTORY=${SESH_AI_SCHEMA_DIRECTORY}"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifySchemaBundle.cmake"
	COMMENT "Verifying the vendored schema bundle against MANIFEST.json"
	VERBATIM
)

message(STATUS
	"Sesh AI extension: vendored schema bundle — ${SESH_AI_SCHEMA_FILE_COUNT} files, "
	"protocolVersion ${SESH_AI_SCHEMA_PROTOCOL_VERSION}"
)

# ---------------------------------------------------------------------------
# Source layout
#
# Source domains live under src/ — entry, transport, daw, context, auth, ui —
# and include paths are rooted at src/ so headers are included by domain
# (`transport/envelope_codec.h`) rather than by relative path. The generated
# include directory is rooted the same way.
#
# Sources are discovered rather than listed, and that is a deliberate departure
# from the usual CMake advice. A glob normally risks a file that exists on disk
# but not in the build, because adding one changes nothing CMake is watching.
# CONFIGURE_DEPENDS is what makes it safe enough here: the generator re-checks the
# glob on every build and re-runs configuration when the matched set changes. The
# cost is a directory stat per build, which is not measurable against a C++
# compile.
#
# What buys it: src/ is filled in by many independent pieces of work, and a
# hand-maintained list is one file every one of them has to edit — so they collide
# with each other over a list that carries no information beyond what the
# directory already shows. If a platform ever needs a file compiled out, that is
# worth an explicit exclusion here rather than a return to hand-listing
# everything.
#
# The include path deliberately does not carry the REAPER SDK. That is added to the
# library target alone, so the test target cannot reach it — see the SDK section
# above for why that is the enforcement mechanism rather than a convention.
# ---------------------------------------------------------------------------

set(SESH_AI_INCLUDE_DIRECTORIES
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
	"${SESH_AI_GENERATED_INCLUDE_DIRECTORY}"
)

file(GLOB_RECURSE SESH_AI_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

# The explicit exclusions the glob comment above anticipates. There are two.
#
# The CEF subprocess helper's entry point lives under src/ui/ beside the rest of the CEF
# host, and it defines `main` (`wWinMain` on Windows). Compiled into the shared library
# it would be a second program entry point inside REAPER's process. It belongs to its
# own target, further down.
set(SESH_AI_CEF_SUBPROCESS_HELPER_SOURCE
	"${CMAKE_CURRENT_SOURCE_DIR}/src/ui/cef_subprocess_helper_main.cpp"
)

list(REMOVE_ITEM SESH_AI_SOURCES "${SESH_AI_CEF_SUBPROCESS_HELPER_SOURCE}")

# The second is by platform. src/ui/cef_library_loading_windows.cpp loads libcef.dll by
# its full path and defines the delay-load notification hook (task 32.2). It is Win32
# calls throughout, and only the Windows build delay-loads libcef.dll; see the CEF part
# of the library target below.
set(SESH_AI_CEF_LIBRARY_LOADING_WINDOWS_SOURCE
	"${CMAKE_CURRENT_SOURCE_DIR}/src/ui/cef_library_loading_windows.cpp"
)

if(NOT SESH_AI_PLATFORM_WINDOWS)
	list(REMOVE_ITEM SESH_AI_SOURCES "${SESH_AI_CEF_LIBRARY_LOADING_WINDOWS_SOURCE}")
endif()

# ---------------------------------------------------------------------------
# The panel's dialog resource, and SWELL
#
# `DockWindowAddEx` takes an `HWND`, and the extension gets one by creating a dialog
# from src/entry/panel_window.rc (task 25.1). One resource script, two routes to a
# window:
#
#   Windows       the resource compiler builds the .rc into the DLL, and
#                 `CreateDialogParam` finds it through the module handle. CMake's RC
#                 language does this for any .rc in a target's sources; it is enabled
#                 explicitly below rather than relied on as a side effect of MSVC.
#   macOS, Linux  nothing compiles the .rc. WDL's generator turns it into C++ that
#                 src/entry/reaper_panel_window.cpp includes, and SWELL builds the
#                 dialog from that at run time.
#
# What the SDK contains, read at the pinned WDL commit (SESH_AI_WDL_COMMIT above) —
# the six files below are byte-identical to that commit on GitHub:
#
#   WDL/swell/swell_resgen.sh   three lines of sh that run swell_resgen.pl beside it,
#                               passing the arguments through. A wrapper, not a
#                               generator.
#   WDL/swell/swell_resgen.pl   the generator. Perl 5 using only core modules
#                               (strict, warnings, Fcntl). Usage is
#                               `swell_resgen.pl [--force] [--quiet] file.rc ...`, and
#                               for each input it writes `<input>.rc_mac_dlg` and
#                               `<input>.rc_mac_menu` beside it.
#   WDL/swell/swell_resgen.php  a PHP port of the same thing. Not used, because PHP is
#                               not on every runner image (below). ReaPack uses this
#                               one, with PHP on its build machines.
#   WDL/swell/swell-dlggen.h    what the generated file is written against. Its header
#                               comment says to include it and then the .rc_mac_dlg,
#                               which is what WDL/swell/sample_project/myapp.cpp and the
#                               SDK's reaper-plugins/reaper_csurf/csurf_main.cpp both do.
#                               Each dialog expands to a static object that adds itself
#                               to SWELL_curmodule_dialogresource_head.
#   WDL/swell/swell-modstub.mm
#   WDL/swell/swell-modstub-generic.cpp
#                               define SWELL_curmodule_dialogresource_head, and one
#                               function pointer per SWELL function, under
#                               SWELL_PROVIDED_BY_APP. The pointers are filled from
#                               REAPER: on macOS by asking the application delegate for
#                               `swellGetAPPAPIFunc` when the module loads, on Linux
#                               through `SWELL_dllMain`, which REAPER calls. A function
#                               REAPER does not supply becomes a stub returning zero.
#
# The generator is not the whole of it, and the task that commissioned this did not
# know that. Creating the dialog calls SWELL, and without SWELL_PROVIDED_BY_APP
# swell.h declares its functions as ordinary externs that nothing in the process
# defines under those names — REAPER provides SWELL through the lookup above, not as
# exported symbols. So the module stub is compiled into the library, and the
# definition is set on every translation unit, because swell.h has to declare the
# same thing everywhere it is included.
#
# The interpreter on each CI runner image, from actions/runner-images' published
# readmes (images/ubuntu/Ubuntu2204-Readme.md, images/macos/macos-15-arm64-Readme.md,
# images/macos/macos-15-Readme.md, images/windows/Windows2025-Readme.md):
#
#   ubuntu-22.04     Perl 5.34.0                  no PHP
#   macos-15         Perl 5.44.0                  no PHP — the arm64 image
#   macos-15-intel   Perl 5.44.0                  PHP 8.5
#   windows-2025     Perl 5.42.0                  PHP 8.5 — and needs neither
#
# Perl is on all four, so no workflow installs anything. It is also on every
# developer machine this builds on: macOS ships /usr/bin/perl, and Ubuntu's dpkg
# depends on perl-base. find_package(Perl) still locates it, so a machine without
# one is told so at configure time, and the library is listed as not buildable in the
# same report as a missing SDK — rather than failing midway through a build.
#
# Generated at build time, not at configure time:
#
#   - The .rc is a source file people edit. As a custom command's dependency an edit
#     regenerates on the next `cmake --build` like any other source; at configure
#     time it would need CMAKE_CONFIGURE_DEPENDS and a full reconfigure per edit.
#   - It matches how WDL itself wires it: WDL/swell/sample_project/Makefile has a
#     build rule from res.rc to its two outputs, and ReaPack uses add_custom_command.
#   - The output is a build product. It goes under generated/, beside
#     schema_bundle_version.h, so `#include <entry/panel_window.rc_mac_dlg>` resolves
#     the same way as every other generated header and src/ stays clean.
#
# cmake/GenerateSwellDialogResource.cmake wraps the generator, because its exit
# status does not mean what a build step needs it to — that file has the detail.
#
# Verified on macOS only. The Linux path (GCC, swell-modstub-generic.cpp,
# SWELL_dllMain) and the Windows path (rc.exe reading an LF-only .rc) are exercised
# first by CI.
# ---------------------------------------------------------------------------

set(SESH_AI_PANEL_DIALOG_RESOURCE_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/src/entry/panel_window.rc")

# The macro the .rc's DIALOGEX line names, and src/entry/panel_window_resource.h
# defines. Checked in the generated output — see the script.
set(SESH_AI_PANEL_DIALOG_RESOURCE_NAME "SESH_AI_PANEL_DIALOG_RESOURCE_IDENTIFIER")

set(SESH_AI_PANEL_WINDOW_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/entry/reaper_panel_window.cpp")

set(SESH_AI_PANEL_DIALOG_RESOURCE_SOURCES "")

if(SESH_AI_PLATFORM_WINDOWS)
	enable_language(RC)

	list(APPEND SESH_AI_PANEL_DIALOG_RESOURCE_SOURCES "${SESH_AI_PANEL_DIALOG_RESOURCE_SCRIPT}")
elseif(SESH_AI_REAPER_SDK_FOUND)
	# The module stub on macOS is Objective-C++, because it asks NSApp's delegate for
	# SWELL. The language is enabled right after project(), not here where it is
	# used. Enabling it here came after CEF's wrapper had been added, and the wrapper's
	# own .mm files then had no compile rule; the comment there has the detail.
	find_package(Perl)

	set(SESH_AI_SWELL_DIRECTORY "${SESH_AI_REAPER_SDK_LOCATED_ROOT}/WDL/swell")
	set(SESH_AI_SWELL_RESOURCE_GENERATOR "${SESH_AI_SWELL_DIRECTORY}/swell_resgen.pl")

	if(SESH_AI_PLATFORM_MACOS)
		set(SESH_AI_SWELL_MODULE_STUB "${SESH_AI_SWELL_DIRECTORY}/swell-modstub.mm")
	else()
		set(SESH_AI_SWELL_MODULE_STUB "${SESH_AI_SWELL_DIRECTORY}/swell-modstub-generic.cpp")
	endif()

	set(SESH_AI_GENERATED_RESOURCE_DIRECTORY "${SESH_AI_GENERATED_INCLUDE_DIRECTORY}/entry")
	set(SESH_AI_GENERATED_PANEL_DIALOG "${SESH_AI_GENERATED_RESOURCE_DIRECTORY}/panel_window.rc_mac_dlg")

	if(PERL_FOUND)
		add_custom_command(
			OUTPUT "${SESH_AI_GENERATED_PANEL_DIALOG}"
			# Written by the generator whether or not the script has a menu. Named so
			# that a clean removes it, not because anything includes it.
			BYPRODUCTS
				"${SESH_AI_GENERATED_RESOURCE_DIRECTORY}/panel_window.rc"
				"${SESH_AI_GENERATED_RESOURCE_DIRECTORY}/panel_window.rc_mac_menu"
			COMMAND "${CMAKE_COMMAND}"
				"-DSESH_AI_PERL_EXECUTABLE=${PERL_EXECUTABLE}"
				"-DSESH_AI_SWELL_RESOURCE_GENERATOR=${SESH_AI_SWELL_RESOURCE_GENERATOR}"
				"-DSESH_AI_RESOURCE_SCRIPT=${SESH_AI_PANEL_DIALOG_RESOURCE_SCRIPT}"
				"-DSESH_AI_GENERATED_RESOURCE_DIRECTORY=${SESH_AI_GENERATED_RESOURCE_DIRECTORY}"
				"-DSESH_AI_EXPECTED_DIALOG_NAME=${SESH_AI_PANEL_DIALOG_RESOURCE_NAME}"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateSwellDialogResource.cmake"
			MAIN_DEPENDENCY "${SESH_AI_PANEL_DIALOG_RESOURCE_SCRIPT}"
			DEPENDS
				"${SESH_AI_SWELL_RESOURCE_GENERATOR}"
				"${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateSwellDialogResource.cmake"
			COMMENT "Generating the panel's SWELL dialog resource from src/entry/panel_window.rc"
			VERBATIM
		)

		# Listed as a source so every generator orders the generation before the
		# compile that includes it. HEADER_FILE_ONLY because the extension is not one
		# CMake knows, and it must never be handed to a compiler on its own.
		set_source_files_properties("${SESH_AI_GENERATED_PANEL_DIALOG}" PROPERTIES
			HEADER_FILE_ONLY ON
		)

		list(APPEND SESH_AI_PANEL_DIALOG_RESOURCE_SOURCES
			"${SESH_AI_GENERATED_PANEL_DIALOG}"
			"${SESH_AI_SWELL_MODULE_STUB}"
		)
	endif()
endif()

# What the shared library cannot be built without. Collected rather than checked
# one at a time so a developer with an empty machine is told everything they need
# in one pass instead of discovering it a configure at a time.
#
# This used to be a hard error the moment src/ had any source, on the reasoning
# that a missing JSON dependency is a real problem worth naming early. It is — but
# failing configuration for the whole project also takes the Catch2 suite down
# with it, and the suite is the part a contributor with nothing installed is most
# likely to want. The REAPER-independent logic is compiled into the test binary
# directly, so it does not need any of these.
#
# So this follows the REAPER SDK's pattern instead: report what is missing, skip
# the target that needs it, and let CI turn the same situation into a failure with
# -DSESH_AI_REQUIRE_EXTENSION_LIBRARY=ON. A build with no library is then
# impossible to ship by accident but perfectly possible to work in.
#
# CEF is the one prerequisite that does not skip, and it is checked last. A
# library without it can never show its panel, so once everything else is present
# a missing distribution is a configuration error rather than a reason to build
# something nobody wants. Checking it last is also what keeps the download off a
# configure that was never going to create the library.
option(SESH_AI_REQUIRE_EXTENSION_LIBRARY
	"Fail configuration when the extension's shared library target cannot be created" OFF
)

set(SESH_AI_LIBRARY_MISSING_PREREQUISITES "")

if(NOT SESH_AI_SOURCES)
	list(APPEND SESH_AI_LIBRARY_MISSING_PREREQUISITES "any source under src/")
endif()

if(NOT nlohmann_json_FOUND)
	list(APPEND SESH_AI_LIBRARY_MISSING_PREREQUISITES
		"nlohmann/json (declared in vcpkg.json — configure with a vcpkg toolchain)"
	)
endif()

if(NOT nlohmann_json_schema_validator_FOUND)
	list(APPEND SESH_AI_LIBRARY_MISSING_PREREQUISITES
		"nlohmann/json-schema-validator (declared in vcpkg.json — configure with a vcpkg toolchain)"
	)
endif()

if(NOT SESH_AI_REAPER_SDK_FOUND)
	list(APPEND SESH_AI_LIBRARY_MISSING_PREREQUISITES
		"the REAPER extension SDK (configure with -DSESH_AI_DOWNLOAD_REAPER_SDK=ON)"
	)
endif()

# Only asked where it is used, and only once there is an SDK to find the generator in —
# without one the SDK is already the line above.
if(NOT SESH_AI_PLATFORM_WINDOWS AND SESH_AI_REAPER_SDK_FOUND AND NOT PERL_FOUND)
	list(APPEND SESH_AI_LIBRARY_MISSING_PREREQUISITES
		"Perl, which runs WDL's swell_resgen.pl over the panel's dialog resource (macOS and Ubuntu both ship one, so check PATH before installing anything)"
	)
endif()

# ---------------------------------------------------------------------------
# The shared library's prerequisites: CEF
#
# Declared and located in "Dependency: CEF binary distribution" above. Acquired
# here, and only when the list above is empty, because only the shared library and
# the CEF subprocess helpers that serve it need CEF. SESH_AI_CEF_FOUND is therefore
# ON exactly when the shared library target is created. In a configure without the
# library, such as one without vcpkg, CEF is neither downloaded nor configured,
# even when a distribution is on disk, and the Catch2 suite builds as it always has.
# ---------------------------------------------------------------------------
if(NOT SESH_AI_LIBRARY_MISSING_PREREQUISITES AND NOT SESH_AI_CEF_LOCATED_ROOT AND SESH_AI_DOWNLOAD_CEF)
	set(sesh_ai_cef_archive_sha1 "${SESH_AI_CEF_ARCHIVE_SHA1_${SESH_AI_CEF_PLATFORM}}")

	if(NOT sesh_ai_cef_archive_sha1)
		message(FATAL_ERROR
			"Sesh AI extension: no published digest recorded for CEF ${SESH_AI_CEF_VERSION} on "
			"${SESH_AI_CEF_PLATFORM}. Add it to CMakeLists.txt or extract the distribution by hand and "
			"pass -DSESH_AI_CEF_ROOT=<path>."
		)
	endif()

	# The version string contains '+' characters, which have to be percent-encoded
	# in the request path.
	string(REPLACE "+" "%2B" sesh_ai_cef_archive_url_name "${SESH_AI_CEF_DISTRIBUTION_NAME}.tar.bz2")
	set(sesh_ai_cef_archive_path "${SESH_AI_CEF_VENDOR_DIRECTORY}/${SESH_AI_CEF_DISTRIBUTION_NAME}.tar.bz2")

	message(STATUS "Sesh AI extension: downloading CEF ${SESH_AI_CEF_VERSION} for ${SESH_AI_CEF_PLATFORM}")

	file(DOWNLOAD
		"https://cef-builds.spotifycdn.com/${sesh_ai_cef_archive_url_name}"
		"${sesh_ai_cef_archive_path}"
		EXPECTED_HASH "SHA1=${sesh_ai_cef_archive_sha1}"
		SHOW_PROGRESS
		STATUS sesh_ai_cef_download_status
	)

	list(GET sesh_ai_cef_download_status 0 sesh_ai_cef_download_result)

	if(NOT sesh_ai_cef_download_result EQUAL 0)
		list(GET sesh_ai_cef_download_status 1 sesh_ai_cef_download_message)

		# file(DOWNLOAD) leaves whatever it received, often an error page, at the
		# archive's path in third_party/cef.
		file(REMOVE "${sesh_ai_cef_archive_path}")

		message(FATAL_ERROR
			"Sesh AI extension: CEF download failed — ${sesh_ai_cef_download_message}. The shared "
			"library cannot be built without CEF. Download ${SESH_AI_CEF_DISTRIBUTION_NAME}.tar.bz2 "
			"by hand from https://cef-builds.spotifycdn.com/index.html, extract it, and pass "
			"-DSESH_AI_CEF_ROOT=<path>. See DEPENDENCIES.md."
		)
	endif()

	# A failed extraction is reported by file() itself and does not stop
	# configuration on its own, so the check that follows is what turns it into an
	# error with instructions.
	file(ARCHIVE_EXTRACT
		INPUT "${sesh_ai_cef_archive_path}"
		DESTINATION "${SESH_AI_CEF_VENDOR_DIRECTORY}"
	)

	file(REMOVE "${sesh_ai_cef_archive_path}")

	sesh_ai_locate_cef_distribution(SESH_AI_CEF_LOCATED_ROOT)

	if(NOT SESH_AI_CEF_LOCATED_ROOT)
		message(FATAL_ERROR
			"Sesh AI extension: the downloaded CEF archive did not extract to "
			"${SESH_AI_CEF_DEFAULT_ROOT}. The shared library cannot be built without CEF. Extract "
			"the archive by hand and pass -DSESH_AI_CEF_ROOT=<path>. See DEPENDENCIES.md."
		)
	endif()
endif()

if(NOT SESH_AI_LIBRARY_MISSING_PREREQUISITES AND NOT SESH_AI_CEF_LOCATED_ROOT)
	message(FATAL_ERROR
		"Sesh AI extension: no CEF binary distribution found for ${SESH_AI_CEF_PLATFORM}, and the "
		"shared library cannot be built without one. Configure with -DSESH_AI_DOWNLOAD_CEF=ON (the "
		"default) to fetch the pinned distribution into third_party/cef, or pass "
		"-DSESH_AI_CEF_ROOT=<path> or set CEF_ROOT to an extracted ${SESH_AI_CEF_DISTRIBUTION_NAME}. "
		"See DEPENDENCIES.md."
	)
endif()

set(SESH_AI_CEF_FOUND OFF)

if(NOT SESH_AI_LIBRARY_MISSING_PREREQUISITES)
	set(SESH_AI_CEF_FOUND ON)

	# FindCEF and cef_variables come from the distribution itself. CEF_ROOT is the
	# name they expect, and find_package(CEF) fills in CEF_BINARY_DIR,
	# CEF_RESOURCE_DIR, CEF_LIBCEF_DLL_WRAPPER_PATH, and the per-configuration
	# library variables.
	set(CEF_ROOT "${SESH_AI_CEF_LOCATED_ROOT}")
	list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")

	# One C runtime across the whole link, chosen here rather than in a workflow.
	#
	# CEF declares this itself, in the Windows half of cef_variables.cmake:
	#
	#   set(CEF_RUNTIME_LIBRARY_FLAG "/MT" CACHE STRING
	#       "Optional flag specifying which runtime to use")
	#
	# `/MT` is the static CRT, and CEF applies it to libcef_dll_wrapper. Nothing
	# else in this build uses it: MSVC's CMake default for a Release build is the
	# dynamic CRT, and both vcpkg triplets in play resolve to it too — plain
	# `x64-windows` in CI, `x64-windows-static-md` for a release, the `-md` suffix
	# meaning static libraries against the *dynamic* CRT. Mixing the two is
	# LNK2038, a hard link error rather than a warning. So the wrapper is moved to
	# `/MD` to match everything around it, instead of moving everything to `/MT`.
	#
	# Set before find_package, which is what makes it take: CEF's own line is a
	# plain `set(... CACHE ...)`, and that does not overwrite a cache entry that
	# already exists.
	#
	# It lives here, and not as a `-D` in `ci.yml` and `release.yml` where it
	# started, because it cannot survive the trip. Those workflows configure from
	# `shell: bash`, which on a Windows runner is Git Bash, and MSYS rewrites
	# arguments that look like POSIX paths — so `-DCEF_RUNTIME_LIBRARY_FLAG=/MD`
	# arrived as `C:/Program Files/Git/MD`, CEF appended it as a compiler flag, and
	# MSBuild reported `error C1083: Cannot open source file:
	# 'C:/Program Files/Git/MD'`. `MSYS_NO_PATHCONV` or a `//MD` would both have
	# worked and both would have left a flag whose correctness depends on the shell
	# that launched CMake. A build decision belongs in the build.
	if(WIN32)
		set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING
			"MSVC runtime flag applied to CEF's own targets"
		)
	endif()

	find_package(CEF REQUIRED)

	# Undo CEF's deployment target, which it sets without asking.
	#
	# cef_variables.cmake — included by the find_package above, into this scope —
	# ends its macOS section with:
	#
	#   set(CEF_TARGET_SDK               "12.0")
	#   set(CMAKE_OSX_DEPLOYMENT_TARGET  ${CEF_TARGET_SDK})
	#
	# A plain set(), so it shadows the cache entry this file established before
	# project() for the remainder of this directory scope and every subdirectory
	# added after it: the wrapper below, the library, and tests/. The vcpkg ports
	# are unaffected because they ran earlier, through MACOSX_DEPLOYMENT_TARGET.
	# The result is a 13.0 dependency graph linked into a 12.0 binary, which the
	# linker reports roughly a hundred times:
	#
	#   ld: warning: object file (libCatch2.a(...)) was built for newer 'macOS'
	#   version (13.0) than being linked (12.0)
	#
	# That 12.0 is upstream lag rather than a claim about what CEF 154 supports.
	# Chromium 151 dropped macOS 12 Monterey, four milestones before the pinned
	# build, and CEF has since moved both of those lines to "13.0" and dropped
	# 12.0 from its SDK search list. The pin predates the correction.
	#
	# So this restores the floor rather than negotiating with it — ADR 0023 fixes
	# macOS 13 for both architectures, and a dependency compiled for 13.0 is
	# correct. Only the variable needs restoring: CEF also appends
	# -mmacosx-version-min=${CEF_TARGET_SDK} to CEF_COMPILER_FLAGS, but nothing
	# here consumes CEF's flag lists, so that copy never reaches a command line.
	# It would if this file ever started using them, and then this would not be
	# enough.
	# $CACHE{} rather than a saved copy of the value: it reads the cache entry
	# established above, which is the one place the floor is written, and is
	# exactly the read a shadowing normal variable cannot affect. A -D on the
	# command line goes into the cache too, so an override still survives this.
	if(SESH_AI_PLATFORM_MACOS)
		set(CMAKE_OSX_DEPLOYMENT_TARGET "$CACHE{CMAKE_OSX_DEPLOYMENT_TARGET}")
	endif()

	# The wrapper is distributed as source and has to be compiled with the same
	# settings as the code that consumes it, so it is built here rather than
	# linked from a prebuilt archive.
	add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)

	# Deliberately not setting the wrapper's C++ standard here, though CEF 154's
	# headers do require C++20 — include/base/cef_scoped_refptr.h alone uses
	# std::same_as, std::derived_from, and a requires-clause over
	# std::convertible_to.
	#
	# The wrapper already gets it, from CEF rather than from this file. Its
	# libcef_dll/CMakeLists.txt calls SET_LIBRARY_TARGET_PROPERTIES, which reaches
	# SET_COMMON_TARGET_PROPERTIES and applies CEF_CXX_COMPILER_FLAGS — containing
	# -std=c++20, or /std:c++20 on MSVC — as a target_compile_options on the
	# wrapper. So it compiles at C++20 whatever CMAKE_CXX_STANDARD says here, and
	# a CXX_STANDARD 20 on this target would be a second way of saying it that
	# could later disagree with CEF's own number.
	#
	# Worth writing down because the asymmetry looks like an oversight: the
	# extension target below *does* need it set explicitly. Nothing in this file
	# consumes CEF's flag lists, so our own translation units never see them.

	# The CEF API version the wrapper was compiled at, read off the wrapper target
	# so the extension can be compiled at the same one (task 32.1). Applied to the
	# library below, beside its C++20.
	#
	# CEF_API_VERSION decides which declarations CEF's headers contain:
	# include/cef_api_hash.h turns each `CEF_API_ADDED(v)` / `CEF_API_REMOVED(v)`
	# into a comparison against it, and defaults it to CEF_API_VERSION_EXPERIMENTAL
	# (999999) when nothing sets it. cef_variables.cmake sets it in exactly two
	# cases, both into CEF_COMPILER_DEFINES and so onto the wrapper and the helper:
	# `-Dapi_version=<5 digits>` on any platform, and otherwise Windows with
	# USE_SANDBOX, CEF's default, where it is CEF_API_VERSION_LAST (15400 at the
	# pinned build). So on Windows the wrapper and the extension were compiled
	# against different headers. On macOS and Linux neither gets it today; the
	# local macOS build's flags.make shows no CEF_API_VERSION on either target.
	#
	# What differs between 15400 and the experimental default at the pinned build,
	# outside include/test/, is three declarations, all marked
	# `CEF_API_ADDED(CEF_EXPERIMENTAL)`. Every numbered gate is 15400 or lower, so
	# both values agree on the rest:
	#
	#   include/internal/cef_types.h      cef_browser_settings_t gains a last member,
	#                                     ax_viewport_collapse, and
	#                                     CefBrowserSettingsTraits::set in
	#                                     include/internal/cef_types_wrappers.h copies
	#                                     it
	#   include/cef_browser.h             CefBrowserHost gains a last pure virtual,
	#                                     SetAxViewportCollapse
	#   include/capi/cef_browser_capi.h   cef_browser_host_t gains its function pointer
	#
	# The extension uses the first two. It builds a CefBrowserSettings for
	# CreateBrowser, and the wrapper builds its own in OnBeforePopup's translation
	# (libcef_dll/cpptoc/life_span_handler_cpptoc.cc). The two compiles therefore
	# disagree about the definition of an inline function and a class, and the
	# linker keeps whichever copy it sees first. That is an ODR violation that
	# links. It does no damage at the pinned build only by coincidence. The new
	# member sits in what is otherwise tail padding: measured with this
	# distribution's headers on x86_64 macOS, chrome_zoom_bubble is at 256,
	# ax_viewport_collapse at 260, and sizeof is 264 at both versions. The member
	# types have the same sizes on 64-bit Windows, so the result is the same there,
	# but that is inferred and not compiled. The next experimental member could
	# change the size or move a field. The extra virtual comes after every slot the
	# extension calls. But the extension could call it and compile cleanly, and the
	# call would run past the end of the wrapper's vtable.
	#
	# Read off the wrapper target rather than repeating CEF's condition, so the
	# value is whatever the wrapper was actually compiled with, `-Dapi_version`
	# included, and it follows CEF if CEF changes when the definition applies. The
	# filter takes only that one definition. CEF's other definitions stay off the
	# extension, as the 2026-10-11 section of docs/STATUS.md records. Of the ones
	# the wrapper gets, none changes a declaration the extension shares with it.
	# Read in the CEF source at the pinned commit, where neither include/ nor
	# libcef_dll/ mentions them:
	#
	#   CEF_USE_BOOTSTRAP   Windows with USE_SANDBOX. Only CEF's sample programs read
	#                       it (tests/cefsimple/cefsimple_win.cc and
	#                       tests/cefclient/cefclient_win.cc), to export RunWinMain
	#                       for bootstrap.exe in place of wWinMain. The extension is
	#                       not a bootstrap client DLL, so defining it would be a
	#                       false claim with no effect.
	#   CEF_USE_SANDBOX     macOS and Linux with USE_SANDBOX. Read by the helper's own
	#                       source, which gets it from CEF's lists, and nothing else.
	#   __STDC_CONSTANT_MACROS, __STDC_FORMAT_MACROS
	#                       Change only C99 macros in the system's stdint.h and
	#                       inttypes.h, not a layout.
	#   WRAPPING_CEF_SHARED The wrapper's own definition. In include/ it only admits
	#                       the test headers.
	get_target_property(sesh_ai_cef_wrapper_compile_definitions libcef_dll_wrapper COMPILE_DEFINITIONS)

	if(NOT sesh_ai_cef_wrapper_compile_definitions)
		set(sesh_ai_cef_wrapper_compile_definitions "")
	endif()

	set(SESH_AI_CEF_API_VERSION_DEFINITIONS ${sesh_ai_cef_wrapper_compile_definitions})
	list(FILTER SESH_AI_CEF_API_VERSION_DEFINITIONS INCLUDE REGEX "^CEF_API_VERSION(=|$)")

	# The guard. The filter above can only see the definition where CEF puts it
	# today. If a later distribution passes it as a raw `-D` or `/D` compile
	# option, or as a directory definition, the filter finds nothing, and the
	# extension silently goes back to the experimental default. So any other
	# mention fails configuration here. A static_assert in a translation unit would
	# not be a stronger check: the value it compared against would come from this
	# same filter.
	get_target_property(sesh_ai_cef_wrapper_compile_options libcef_dll_wrapper COMPILE_OPTIONS)
	get_directory_property(sesh_ai_cef_wrapper_directory_definitions
		DIRECTORY "${CEF_LIBCEF_DLL_WRAPPER_PATH}" COMPILE_DEFINITIONS
	)

	if("${sesh_ai_cef_wrapper_compile_options};${sesh_ai_cef_wrapper_directory_definitions}"
		MATCHES "CEF_API_VERSION")
		message(FATAL_ERROR
			"Sesh AI extension: libcef_dll_wrapper is given CEF_API_VERSION somewhere other than its "
			"COMPILE_DEFINITIONS, so the extension cannot be compiled at the same API version as the "
			"wrapper it links. Extend the derivation of SESH_AI_CEF_API_VERSION_DEFINITIONS in "
			"CMakeLists.txt to read it from there."
		)
	endif()

	if(SESH_AI_CEF_API_VERSION_DEFINITIONS)
		message(STATUS
			"Sesh AI extension: CEF's wrapper and the shared library are both compiled with "
			"${SESH_AI_CEF_API_VERSION_DEFINITIONS}"
		)
	else()
		message(STATUS
			"Sesh AI extension: CEF's wrapper and the shared library are both compiled at CEF's "
			"default API version (CEF_API_VERSION unset)"
		)
	endif()

	unset(sesh_ai_cef_wrapper_compile_definitions)
	unset(sesh_ai_cef_wrapper_compile_options)
	unset(sesh_ai_cef_wrapper_directory_definitions)

	# The runtime artifact differs per platform, and so does what packaging has to
	# do with it. Recording it once keeps the packaging and UI host work from
	# re-deriving these paths.
	if(SESH_AI_PLATFORM_MACOS)
		# A framework bundle placed beside the extension binary and loaded at run time,
		# never linked; see "The CEF runtime beside the binary" below. Helper processes
		# are separate .app bundles beside it, each with its own signature
		# (requirement 15.7).
		set(SESH_AI_CEF_RUNTIME_ARTIFACT "${CEF_BINARY_DIR}/Chromium Embedded Framework.framework")
		set(SESH_AI_CEF_RUNTIME_ARTIFACT_KIND "framework")
	elseif(SESH_AI_PLATFORM_WINDOWS)
		# libcef.dll plus the import library. The loader has to sit next to the
		# extension binary and the helper in REAPER's UserPlugins directory.
		set(SESH_AI_CEF_RUNTIME_ARTIFACT "${CEF_BINARY_DIR}/libcef.dll")
		set(SESH_AI_CEF_RUNTIME_ARTIFACT_KIND "shared_library")
		set(SESH_AI_CEF_IMPORT_LIBRARY "${CEF_BINARY_DIR}/libcef.lib")
	else()
		# libcef.so, loaded from an rpath-relative directory. The sandbox helper
		# needs its own placement and permissions.
		set(SESH_AI_CEF_RUNTIME_ARTIFACT "${CEF_BINARY_DIR}/libcef.so")
		set(SESH_AI_CEF_RUNTIME_ARTIFACT_KIND "shared_library")
	endif()

	message(STATUS "Sesh AI extension: CEF ${SESH_AI_CEF_VERSION} at ${CEF_ROOT}")
	message(STATUS "Sesh AI extension: CEF runtime artifact (${SESH_AI_CEF_RUNTIME_ARTIFACT_KIND}) ${SESH_AI_CEF_RUNTIME_ARTIFACT}")
elseif(NOT SESH_AI_CEF_LOCATED_ROOT)
	# Listed with the rest, so that the one pass this report exists for also names
	# CEF. Nothing is downloaded yet, because the library is not being created.
	if(SESH_AI_DOWNLOAD_CEF)
		list(APPEND SESH_AI_LIBRARY_MISSING_PREREQUISITES
			"a CEF binary distribution for ${SESH_AI_CEF_PLATFORM} (downloaded into third_party/cef by this configure once everything above is present, or pass -DSESH_AI_CEF_ROOT=<path>)"
		)
	else()
		list(APPEND SESH_AI_LIBRARY_MISSING_PREREQUISITES
			"a CEF binary distribution for ${SESH_AI_CEF_PLATFORM} (SESH_AI_DOWNLOAD_CEF is OFF, so pass -DSESH_AI_CEF_ROOT=<path>, or turn the download back on)"
		)
	endif()
endif()

if(SESH_AI_LIBRARY_MISSING_PREREQUISITES)
	string(REPLACE ";" "\n  - " sesh_ai_library_missing_prerequisites_report
		"${SESH_AI_LIBRARY_MISSING_PREREQUISITES}"
	)

	if(SESH_AI_REQUIRE_EXTENSION_LIBRARY)
		message(FATAL_ERROR
			"Sesh AI extension: the shared library target cannot be created, and "
			"SESH_AI_REQUIRE_EXTENSION_LIBRARY is ON. Missing:\n  - "
			"${sesh_ai_library_missing_prerequisites_report}\nSee DEPENDENCIES.md."
		)
	endif()

	message(STATUS
		"Sesh AI extension: the shared library target is not created. Missing:\n  - "
		"${sesh_ai_library_missing_prerequisites_report}\nThe Catch2 suite is unaffected. "
		"See DEPENDENCIES.md."
	)

	unset(sesh_ai_library_missing_prerequisites_report)
else()
	add_library(sesh_ai_extension SHARED ${SESH_AI_SOURCES})

	# C++20 for the same reason the wrapper needs it: CEF 154's headers use concepts.
	#
	# This raises all of SESH_AI_SOURCES, not just the CEF translation units, because
	# the standard is a target property and CMake has no per-source equivalent. The
	# Catch2 target still compiles at the project's C++17, which covers the logic in
	# headers but not the library's own .cpp files.
	set_target_properties(sesh_ai_extension PROPERTIES
		OUTPUT_NAME "${SESH_AI_EXTENSION_OUTPUT_NAME}"
		PREFIX ""
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF
	)

	target_include_directories(sesh_ai_extension PRIVATE ${SESH_AI_INCLUDE_DIRECTORIES})

	# The SDK include path is added here and only here — not in
	# SESH_AI_INCLUDE_DIRECTORIES, which the test target shares.
	#
	# SYSTEM because reaper_plugin.h is full of inline virtuals with unused
	# parameters and produces around sixty-five warnings per translation unit under
	# -Wall -Wextra. Warnings nobody can fix train people to ignore warnings.
	target_include_directories(sesh_ai_extension SYSTEM PRIVATE
		${SESH_AI_REAPER_SDK_INCLUDE_DIRECTORIES}
	)

	target_link_libraries(sesh_ai_extension PRIVATE
		sesh_ai_compiler_warnings
		sesh_ai_platform_floor
		nlohmann_json::nlohmann_json
		nlohmann_json_schema_validator::validator
	)

	# The panel's dialog resource — see "The panel's dialog resource, and SWELL" above.
	# The .rc on Windows; the generated dialog and WDL's module stub elsewhere.
	target_sources(sesh_ai_extension PRIVATE ${SESH_AI_PANEL_DIALOG_RESOURCE_SOURCES})

	if(NOT SESH_AI_PLATFORM_WINDOWS)
		# On every translation unit, not only the two that create the dialog: swell.h
		# declares each SWELL function as a pointer under this definition and as a
		# function without it, and one name declared both ways across a library is an
		# ODR violation that links.
		target_compile_definitions(sesh_ai_extension PRIVATE SWELL_PROVIDED_BY_APP)

		# swell-dlggen.h for the one file that expands the generated dialog, without
		# putting the rest of WDL on the library's include path — the SDK section
		# above keeps it off for a reason. As -isystem rather than the
		# INCLUDE_DIRECTORIES property, so it is treated like the SDK headers and its
		# warnings are not this project's to read.
		set_source_files_properties("${SESH_AI_PANEL_WINDOW_SOURCE}" PROPERTIES
			COMPILE_OPTIONS "-isystem;${SESH_AI_SWELL_DIRECTORY}"
		)

		# WDL's code, compiled here because nothing else provides the definitions it
		# holds. Its warnings are as unfixable as the SDK's, hence -w.
		#
		# -fvisibility=hidden is required rather than tidy, and Linux is why. The stub
		# defines one *data* symbol per SWELL function — `DestroyWindow` is a function
		# pointer here — and REAPER's own SWELL, libSwell.so, exports *functions* under
		# the same names. With default visibility, ELF lets a shared object's
		# references to its own globals be interposed by the global scope, where
		# REAPER's executable and libSwell.so come first, so the stub's start-up loop
		# could end up writing through REAPER's function addresses instead of its own
		# pointers. A symbol defined hidden is hidden in the linked library whatever
		# other translation units declared, so the pointers bind inside it and are not
		# exported. SWELL_dllMain, which REAPER does look up, carries its own
		# default-visibility attribute and stays exported. macOS's two-level namespace
		# already binds references this way, so there it is uniformity rather than a
		# fix. Reasoned from the ELF rules, not observed: there is no Linux REAPER here.
		#
		# On the stub alone rather than as the target's visibility preset. The preset
		# was tried: the vcpkg-built json-schema-validator archive is compiled with
		# default visibility, and mixing the two produced 60 ld warnings about weak
		# typeinfo symbols on the macOS build — noise about a hazard that is not this
		# one.
		set_source_files_properties("${SESH_AI_SWELL_MODULE_STUB}" PROPERTIES
			COMPILE_OPTIONS "-w;-fvisibility=hidden"
		)

		if(SESH_AI_PLATFORM_MACOS)
			# The module stub imports Cocoa for NSApp and NSLog.
			target_link_libraries(sesh_ai_extension PRIVATE "-framework Cocoa")
		endif()
	endif()

	# CEF. The library is never created without it; see "The shared library's
	# prerequisites: CEF" above.
	#
	# It follows CEF's documented usage: CEF_INCLUDE_PATH on the include path, the
	# libcef_dll_wrapper target built from source above, and on Windows and Linux the
	# import library and the shared object named directly.
	#
	# macOS links no framework (task 26.2). A linked framework carries the install name
	# `@executable_path/../Frameworks/Chromium Embedded Framework.framework/...`, and
	# inside REAPER `@executable_path` is REAPER's executable, so dyld would look for it
	# inside REAPER's bundle and refuse to load the extension. CEF's wrapper on macOS
	# already resolves every CEF function at run time (libcef_dll/wrapper/
	# libcef_dll_dylib.cc), so nothing needs the link: CefPanelBrowserHost calls
	# `cef_load_library` with a path resolved beside the binary
	# (src/entry/cef_framework_location.h). cefsimple does the same, from a different path.
	target_include_directories(sesh_ai_extension SYSTEM PRIVATE "${CEF_INCLUDE_PATH}")

	# The CEF API version the wrapper was compiled at, so both halves of the link
	# read the same declarations from CEF's headers. Empty today on macOS and
	# Linux, and CEF_API_VERSION=CEF_API_VERSION_LAST on Windows. See the
	# derivation after add_subdirectory(libcef_dll_wrapper) above. On the whole
	# target, like the C++ standard, because any source may include CEF.
	target_compile_definitions(sesh_ai_extension PRIVATE ${SESH_AI_CEF_API_VERSION_DEFINITIONS})

	target_link_libraries(sesh_ai_extension PRIVATE libcef_dll_wrapper)

	if(SESH_AI_PLATFORM_WINDOWS)
		# libcef.dll delay-loaded (task 32.2), the Windows form of the macOS change
		# above. An imported libcef.dll is resolved through the host process's search
		# path, and the host is REAPER, so the loader would refuse the extension before
		# any of its code ran. Delay-loaded, nothing resolves until the first call, and
		# CefPanelBrowserHost loads it by the full path beside the binary before that
		# (src/ui/cef_library_loading.h). libcef.lib is still linked: a delay-loaded DLL
		# is linked against its import library like any other.
		#
		# The one flag, and delayimp.lib, which provides the helper /DELAYLOAD needs,
		# rather than CEF's lists. CEF_SHARED_LINKER_FLAGS on Windows is
		# CEF_DELAYLOAD_FLAGS, which also delay-loads about fifty system libraries,
		# user32 and gdi32 among them. Chromium does that so its sandboxed processes
		# do not load them; inside REAPER they are loaded already, and delay-loading
		# them would only turn a missing export into a structured exception at the
		# first call. CEF_STANDARD_LIBS is what cefsimple links; the extension linked on
		# Windows without it before this, and needs only delayimp.lib from it now. The
		# helper takes CEF's lists, and gets the same /DELAYLOAD:libcef.dll from them;
		# see "The CEF subprocess helper" below.
		target_link_options(sesh_ai_extension PRIVATE "/DELAYLOAD:libcef.dll")
		target_link_libraries(sesh_ai_extension PRIVATE
			"${SESH_AI_CEF_IMPORT_LIBRARY}"
			delayimp.lib
		)
	elseif(SESH_AI_PLATFORM_LINUX)
		target_link_libraries(sesh_ai_extension PRIVATE "${SESH_AI_CEF_RUNTIME_ARTIFACT}")

		# libcef.so beside the extension, as it is beside the helper. Without a
		# run-path the dynamic loader searches REAPER's own run-path and the system
		# directories, and finds no libcef.so in either. "The CEF runtime beside the
		# binary" below copies it into bin/, so the library in bin/ is already the
		# one that ships.
		set_target_properties(sesh_ai_extension PROPERTIES
			INSTALL_RPATH "$ORIGIN"
			BUILD_WITH_INSTALL_RPATH TRUE
		)
	endif()

	if(SESH_AI_PLATFORM_MACOS)
		message(STATUS "Sesh AI extension: the shared library links CEF's wrapper and loads the framework at run time")
	elseif(SESH_AI_PLATFORM_WINDOWS)
		message(STATUS "Sesh AI extension: the shared library delay-loads libcef.dll and loads it by full path at run time")
	else()
		message(STATUS "Sesh AI extension: the shared library links CEF")
	endif()

	# The library embeds the protocolVersion and validates against the bundle, so
	# it has no business being produced from files that failed verification. The
	# verification target is already in ALL; this orders it before the compile.
	add_dependencies(sesh_ai_extension sesh_ai_verify_schema_bundle)

	message(STATUS "Sesh AI extension: shared library target sesh_ai_extension configured")
endif()

# ---------------------------------------------------------------------------
# The CEF subprocess helper (task 26.1, requirements 15.7 and 21.1)
#
# CEF runs the renderer, GPU, and utility processes by launching an executable, and its
# default is the host process's own executable. The host process here is REAPER. So the
# extension always sets `browser_subprocess_path`, and this target builds what it points
# at: one source file, src/ui/cef_subprocess_helper_main.cpp, whose jobs are
# `CefExecuteProcess` and, in the renderer, installing `seshAiBridgeSend` (task 31.1).
#
# What CEF's own sample does, and what is followed from it. The minimal distribution has
# no samples in it, so this was read from tests/cefsimple/CMakeLists.txt.in in the CEF
# repository at the pinned commit (g564dd6c), against cmake/cef_macros.cmake and
# cmake/cef_variables.cmake in the distribution itself:
#
#   - Every cefsimple target, the helper included, gets SET_EXECUTABLE_TARGET_PROPERTIES,
#     which applies all of CEF_COMPILER_FLAGS, CEF_CXX_COMPILER_FLAGS,
#     CEF_COMPILER_DEFINES, CEF_LINKER_FLAGS and CEF_EXE_LINKER_FLAGS. Followed, with the
#     exceptions below.
#   - macOS: the helper links libcef_dll_wrapper and CEF_STANDARD_LIBS, and not the
#     framework, which its main() loads at run time with
#     CefScopedLibraryLoader::LoadInHelper(). Followed.
#   - macOS: five MACOSX_BUNDLE targets, one per entry in CEF_HELPER_APP_SUFFIXES, each
#     with its own Info.plist. Followed by task 26.2; see "The macOS helper bundles"
#     below.
#   - Windows: the executable is built for the GUI subsystem (WIN32) and links libcef.lib
#     and CEF_STANDARD_LIBS. Followed. cefsimple then embeds its manifests with an mt.exe
#     post-build step, because CEF_EXE_LINKER_FLAGS turns the linker's manifest off. Not
#     followed; see below.
#   - Linux: links libcef.so and the wrapper, with INSTALL_RPATH "$ORIGIN" and
#     BUILD_WITH_INSTALL_RPATH, so the binary finds libcef.so beside itself. Followed.
#
# Why this target takes CEF's flags when the extension does not. The extension stays off
# CEF's flag lists because they carry -fno-exceptions, -fno-rtti, _HAS_EXCEPTIONS=0 and
# -Werror, and the extension is built on libraries that report errors by throwing:
# nlohmann/json and json-schema-validator. None of that applies here. The helper is one
# translation unit that calls CEF and links nothing else of ours, so it is the kind of
# program those flags were written for. Taking them also gives the helper exactly the compile definitions
# libcef_dll_wrapper was built with. That matters most on Windows, where USE_SANDBOX adds
# CEF_API_VERSION=CEF_API_VERSION_LAST to the wrapper. The extension gets only that one
# definition, read off the wrapper target (task 32.1); the derivation after
# add_subdirectory(libcef_dll_wrapper) has the reasoning.
#
# Removed from CEF's lists, for this target only:
#
#   -Werror, /WX              This build does not make warnings fatal yet; see "Compiler
#   -Wl,--fatal-warnings      warnings" near the top of this file. The helper is code this
#                             repository writes, so the same rule applies.
#   -mmacosx-version-min=12.0 CEF's stale target SDK (see the restore after
#                             find_package(CEF)). CMake already passes
#                             -mmacosx-version-min=13.0 from CMAKE_OSX_DEPLOYMENT_TARGET, so
#                             removing CEF's later copy is what leaves the floor at 13.0
#                             (ADR 0023).
#   -Wl,-rpath,.              A run-path relative to the working directory, so libcef.so
#                             would be loaded from whatever directory the process was
#                             started in. $ORIGIN, as cefsimple sets it, is relative to the
#                             executable instead.
#   /MANIFEST:NO              Turns off the manifest the linker generates, which cefsimple
#                             replaces with its own mt.exe step. Here the manifest is a
#                             source file, src/ui/cef_subprocess_helper.manifest, which
#                             CMake hands to the linker to merge into the generated one,
#                             under both the Visual Studio and the command-line generators.
#                             That only works with the linker's manifest left on.
#
# Not linked to sesh_ai_compiler_warnings or sesh_ai_platform_floor. CEF_COMPILER_FLAGS
# already carries -Wall -Wextra on macOS, -Wall on Linux, and /W4 on Windows, and
# CEF_COMPILER_DEFINES already carries _WIN32_WINNT=0x0A00 and NOMINMAX on Windows.
# Linking both would pass each flag twice.
#
# A function rather than an inline target, so the five macOS helper bundles are built
# from it: MACOSX_BUNDLE as the extra argument and a suffixed output name per bundle.
# `list(REMOVE_ITEM)` inside a function edits local copies of CEF's lists, so
# libcef_dll_wrapper and anything else that reads them are unaffected.
#
# A plain executable on Windows and Linux, which is what ships there. On macOS a bare
# executable cannot run: LoadInHelper() looks for the framework three directories above
# the executable, which only works from inside `Sesh AI Helper.app/Contents/MacOS/`.
# ---------------------------------------------------------------------------

function(sesh_ai_add_cef_subprocess_helper target_name output_name)
	list(REMOVE_ITEM CEF_COMPILER_FLAGS -Werror /WX "-mmacosx-version-min=${CEF_TARGET_SDK}")
	list(REMOVE_ITEM CEF_LINKER_FLAGS -Wl,--fatal-warnings -Wl,-rpath,.)
	list(REMOVE_ITEM CEF_EXE_LINKER_FLAGS /MANIFEST:NO)

	if(SESH_AI_PLATFORM_WINDOWS)
		add_executable(${target_name} WIN32 ${ARGN}
			"${SESH_AI_CEF_SUBPROCESS_HELPER_SOURCE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/src/ui/cef_subprocess_helper.manifest"
		)
	else()
		add_executable(${target_name} ${ARGN} "${SESH_AI_CEF_SUBPROCESS_HELPER_SOURCE}")
	endif()

	SET_EXECUTABLE_TARGET_PROPERTIES(${target_name})

	# No CXX_STANDARD here, for the reason the wrapper has none: CEF_CXX_COMPILER_FLAGS,
	# applied by the macro above, already carries -std=c++20 or /std:c++20.
	set_target_properties(${target_name} PROPERTIES OUTPUT_NAME "${output_name}")

	target_link_libraries(${target_name} PRIVATE libcef_dll_wrapper)

	# src/ only, for ui/renderer_bridge_binding.h (task 31.1): the renderer's half of the
	# bridge, whose decisions live in a header the suite compiles. That header includes
	# nothing but the standard library and is written to compile under CEF's
	# -fno-exceptions and -fno-rtti. SESH_AI_INCLUDE_DIRECTORIES is not used, because its
	# generated directory is the extension's and nothing in the helper reads it.
	target_include_directories(${target_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

	if(SESH_AI_PLATFORM_MACOS)
		target_link_libraries(${target_name} PRIVATE ${CEF_STANDARD_LIBS})
	elseif(SESH_AI_PLATFORM_WINDOWS)
		# delayimp.lib, in CEF_STANDARD_LIBS, is what makes CEF's /DELAYLOAD:libcef.dll link.
		target_link_libraries(${target_name} PRIVATE
			"${SESH_AI_CEF_IMPORT_LIBRARY}"
			${CEF_STANDARD_LIBS}
		)
	else()
		# CEF_STANDARD_LIBS is only X11 on Linux, which cefsimple links for its own X11
		# error handlers. The helper calls nothing in X11.
		target_link_libraries(${target_name} PRIVATE "${SESH_AI_CEF_RUNTIME_ARTIFACT}")

		# In the build tree as well as when installed, so the executable in bin/ is
		# already the one that ships, and nothing has to rewrite it.
		set_target_properties(${target_name} PROPERTIES
			INSTALL_RPATH "$ORIGIN"
			BUILD_WITH_INSTALL_RPATH TRUE
		)
	endif()
endfunction()

# ---------------------------------------------------------------------------
# The CEF runtime beside the binary (task 26.2, requirements 1.1 and 15.7)
#
# The release archive is unpacked into REAPER's UserPlugins, and everything CEF needs
# sits beside the extension binary there:
#
#   macOS    reaper_sesh_ai.dylib
#            Chromium Embedded Framework.framework/
#            Sesh AI Helper.app/, and the (Alerts), (GPU), (Plugin) and (Renderer) ones
#            sesh_ai_ui/
#
#   Windows  reaper_sesh_ai.dll, sesh_ai_cef_helper.exe, libcef.dll and the rest of
#            CEF_BINARY_FILES, CEF_RESOURCE_FILES (the .pak files, icudtl.dat,
#            locales/ trimmed to SESH_AI_CEF_KEPT_LOCALES), sesh_ai_ui/
#
#   Linux    reaper_sesh_ai.so, sesh_ai_cef_helper, libcef.so and the rest of
#            CEF_BINARY_FILES including chrome-sandbox, CEF_RESOURCE_FILES with locales/
#            trimmed the same way, sesh_ai_ui/
#
# The build produces the same layout in bin/, so a development build resolves end to end
# with nothing but sesh_ai_ui/ missing. Task 27.1's install rules install from here.
#
# Why each piece is where it is:
#
#   - macOS framework: the extension loads it with `cef_load_library` from beside itself,
#     and gives CEF its path as `framework_dir_path`; CEF finds the .pak files and
#     locales inside it. The helpers load it with LoadInHelper(), three directories
#     above their executables, which is this directory.
#   - Windows libcef.dll: the helper delay-loads it from its own directory. The extension
#     delay-loads it too and loads it by full path from beside itself, so its imports,
#     chrome_elf.dll among them, are looked for here first. CEF's resources default to
#     libcef.dll's directory.
#   - Linux libcef.so: both binaries carry the run-path $ORIGIN. CEF's resources default
#     to libcef.so's directory.
#
# The macOS framework is copied rather than symlinked. Codesign requires a framework to
# have the versioned layout (Versions/A plus symlinks), which CEF's distribution does not,
# so the bundle has to be rebuilt in any case; cefsimple's COPY_MAC_FRAMEWORK does the
# same. The copy is also the bundle that ships, byte for byte, so a development build
# exercises the archive's layout rather than a link into third_party/. It costs one
# ~300 MB copy, and only when the distribution's framework executable changes.
# ---------------------------------------------------------------------------

get_property(sesh_ai_generator_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

# Where executables land. A multi-config generator appends the configuration itself, and
# a custom command's OUTPUT may name it only through $<CONFIG>.
if(sesh_ai_generator_is_multi_config)
	set(SESH_AI_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>")
else()
	set(SESH_AI_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

unset(sesh_ai_generator_is_multi_config)

set(SESH_AI_CEF_SUBPROCESS_HELPER_TARGETS "")

# What the Windows and Linux branch below copies into the output directory, kept for the
# install rules further down so they install exactly what the build placed. The locale
# files are a list of their own because they install into locales/ rather than beside
# the binary.
set(SESH_AI_CEF_RUNTIME_FILES_BESIDE_BINARY "")
set(SESH_AI_CEF_LOCALE_FILES_BESIDE_BINARY "")

# ---------------------------------------------------------------------------
# The Chromium locales that ship (task 27.2)
#
# CEF's distribution carries 55 locales, about 49 MB uncompressed on macOS. The chat
# panel's own text comes from the seven react-i18next bundles in ui/src/i18n, and
# Chromium's locale pack only supplies the strings Chromium draws itself: context menus,
# form validation messages, error pages. Shipping Chromium strings for a language the
# panel does not speak would give a producer a panel in one language with menus in
# another, so the packs are trimmed to the panel's seven.
#
# The names are Chromium's, which are not the UI's BCP 47 tags. They are the entries of
# `platform_pak_locales` in Chromium's build/config/locales.gni at 154.0.8037.58, and
# the Windows and Linux file is locales/<name>.pak. macOS spells the same locale as a
# <name>.lproj directory inside the framework's Resources, with "_" for "-" and "en" for
# "en-US" (`locales_as_apple_outputs` in the same file); the macOS names were checked
# against the pinned distribution's framework.
#
#   UI tag   Chromium locale   Windows and Linux        macOS
#   en       en-US             locales/en-US.pak        en.lproj
#   de       de                locales/de.pak           de.lproj
#   ja       ja                locales/ja.pak           ja.lproj
#   pt-BR    pt-BR             locales/pt-BR.pak        pt_BR.lproj
#   es       es, es-419        locales/es.pak, es-419   es.lproj, es_419.lproj
#   fr       fr                locales/fr.pak           fr.lproj
#   it       it                locales/it.pak           it.lproj
#
# en-US is required, not chosen. The extension sets no CefSettings.locale, so Chromium
# picks the locale from the system: the outer bundle's preferred localization on macOS,
# the ICU default locale on Windows, the LANGUAGE, LC_ALL, LC_MESSAGES and LANG variables
# on Linux. A locale with no pack on disk falls back to en-US, and with no en-US pack
# Chromium loads no locale at all (`GetApplicationLocaleInternalNonMac` in
# ui/base/l10n/l10n_util.cc, and `OverrideLocaleWithCocoaLocale` in l10n_util_mac.mm, at
# 154.0.8037.58). The framework's Info.plist also names "en" as CFBundleDevelopmentRegion.
#
# es-419 as well as es because the UI's one Spanish bundle serves every Spanish-speaking
# producer. `CheckAndResolveLocale` matches a system locale with no pack of its own, such
# as es-MX, against Chromium's locale list (`GetPlatformLanguageMatcher`, in
# ui/base/l10n/chromium_language_matcher.cc), which offers es-419 for the Latin American
# regions, and then requires that one pack to exist; it does not try es next. Without
# es-419 most Spanish speakers would get English menus around a Spanish panel. en-GB is
# not kept: the English regions it serves fall back to en-US, which is English.
#
# The distribution's _FEMININE, _MASCULINE and _NEUTER variants are not kept either.
# Chromium 154 loads them on Android only; `ResourceBundle::LocaleDataPakExists` in
# ui/base/resource/resource_bundle.cc carries "TODO: Support gender translations on
# non-Android platforms" and ignores the gender.
#
# A name here that the distribution does not have stops configuration (Windows and Linux)
# or the framework placement (macOS) rather than shipping without it. Revisit the list
# when the UI gains a locale, and re-check the names when the CEF pin moves.
# ---------------------------------------------------------------------------
set(SESH_AI_CEF_KEPT_LOCALES en-US de ja pt-BR es es-419 fr it)

# The helpers read CEF's variables, and a configure that does not create the library
# never sets them: one without vcpkg that builds only the Catch2 suite, for instance.
# SESH_AI_CEF_FOUND is ON exactly when the library is created, so it keeps the helpers
# out of that configure, which would otherwise fail here. It no longer stands for a
# library built without CEF, because there is none.
if(SESH_AI_CEF_FOUND AND SESH_AI_PLATFORM_MACOS)
	# -----------------------------------------------------------------------
	# Signing (requirement 15.7)
	#
	# Every bundle is signed as it is built, ad hoc by default, so a development build is
	# well formed and `codesign --verify --deep --strict` passes on it. A release
	# substitutes a Developer ID identity here and changes nothing else: the order, the
	# Hardened Runtime and the entitlements are already the release ones. Notarization
	# is a step on the finished archive, not a build step, and belongs to the release
	# workflow.
	#
	# What is signed, and how, follows CEF's code signing guide
	# (https://chromiumembedded.github.io/cef/code_signing) and Chromium's own signing
	# definitions at the pinned revision (chrome/installer/mac/signing/parts.py at
	# 154.0.8037.58):
	#
	#   - Inside out. The Mach-O libraries in the framework's Libraries/ directory, then
	#     the framework, then each helper. No --deep, which cannot give each helper its
	#     own entitlements.
	#   - Each helper with the Hardened Runtime (--options runtime), which notarization
	#     requires.
	#   - Entitlements on the helpers only, never on the framework. The Renderer and GPU
	#     helpers get com.apple.security.cs.allow-jit, which V8 and SwiftShader need under
	#     the Hardened Runtime; the plain, Alerts and Plugin helpers get none. The files
	#     are under cmake/cef_helper_entitlements/, and the two allow-jit ones are
	#     Chromium's chrome/app/helper-renderer-entitlements.plist and
	#     helper-gpu-entitlements.plist at 154.0.8037.58, byte for byte; replace them
	#     from the matching Chromium revision when the CEF pin moves. The provenance is
	#     recorded here rather than in the files because codesign's entitlements parser
	#     rejects an XML comment. helper-entitlements.plist is the empty dictionary
	#     CEF's guide gives the other three. CEF's own repository at the
	#     pinned commit (g564dd6c) carries no entitlements files; cefsimple and cefclient
	#     sign nothing.
	#
	# A secure timestamp needs a real identity and a network, so an ad hoc signature
	# carries none and any other identity asks for one.
	# -----------------------------------------------------------------------
	set(SESH_AI_MACOS_CODE_SIGN_IDENTITY "-" CACHE STRING
		"codesign identity for the macOS framework and helper bundles; '-' signs ad hoc"
	)

	if(SESH_AI_MACOS_CODE_SIGN_IDENTITY STREQUAL "-")
		set(sesh_ai_code_sign_timestamp_argument "--timestamp=none")
	else()
		set(sesh_ai_code_sign_timestamp_argument "--timestamp")
	endif()

	set(SESH_AI_CEF_HELPER_ENTITLEMENTS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cef_helper_entitlements")

	# -----------------------------------------------------------------------
	# The framework, placed and signed
	#
	# cmake/PlaceMacosCefFramework.cmake rebuilds the distribution's flat bundle in the
	# versioned layout, trims its locales to SESH_AI_CEF_KEPT_LOCALES, and signs it. Its
	# output is the framework executable, so the copy reruns only when the distribution's
	# executable, the script, or the kept locales change.
	#
	# The locales are trimmed there because they are .lproj directories inside the
	# framework. Removing one after signing breaks the seal, so the trim has to happen
	# between the copy and the signature, and that is inside the script.
	# -----------------------------------------------------------------------
	set(SESH_AI_CEF_PLACED_FRAMEWORK "${SESH_AI_RUNTIME_OUTPUT_DIRECTORY}/Chromium Embedded Framework.framework")

	set(sesh_ai_cef_kept_locale_directories "")

	foreach(sesh_ai_cef_kept_locale IN LISTS SESH_AI_CEF_KEPT_LOCALES)
		if(sesh_ai_cef_kept_locale STREQUAL "en-US")
			list(APPEND sesh_ai_cef_kept_locale_directories "en.lproj")
		else()
			string(REPLACE "-" "_" sesh_ai_cef_kept_locale_directory "${sesh_ai_cef_kept_locale}")
			list(APPEND sesh_ai_cef_kept_locale_directories "${sesh_ai_cef_kept_locale_directory}.lproj")
		endif()
	endforeach()

	# Comma-separated, because a semicolon in a custom command's argument would split it
	# into several arguments.
	string(JOIN "," sesh_ai_cef_kept_locale_directories_argument ${sesh_ai_cef_kept_locale_directories})

	# A file holding the list, rewritten only when the list changes, so that changing it
	# reruns the placement. The Makefile generators do not rerun a custom command whose
	# command line alone changed.
	set(sesh_ai_cef_kept_locale_directories_record
		"${CMAKE_CURRENT_BINARY_DIR}/cef_kept_locale_directories.txt"
	)
	file(CONFIGURE
		OUTPUT "${sesh_ai_cef_kept_locale_directories_record}"
		CONTENT "${sesh_ai_cef_kept_locale_directories_argument}\n"
	)

	add_custom_command(
		OUTPUT "${SESH_AI_CEF_PLACED_FRAMEWORK}/Versions/A/Chromium Embedded Framework"
		COMMAND "${CMAKE_COMMAND}"
			"-DSOURCE_FRAMEWORK=${SESH_AI_CEF_RUNTIME_ARTIFACT}"
			"-DDESTINATION_FRAMEWORK=${SESH_AI_CEF_PLACED_FRAMEWORK}"
			"-DKEPT_LOCALE_DIRECTORIES=${sesh_ai_cef_kept_locale_directories_argument}"
			"-DCODE_SIGN_IDENTITY=${SESH_AI_MACOS_CODE_SIGN_IDENTITY}"
			"-DCODE_SIGN_TIMESTAMP_ARGUMENT=${sesh_ai_code_sign_timestamp_argument}"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PlaceMacosCefFramework.cmake"
		DEPENDS
			"${SESH_AI_CEF_RUNTIME_ARTIFACT}/Chromium Embedded Framework"
			"${CMAKE_CURRENT_SOURCE_DIR}/cmake/PlaceMacosCefFramework.cmake"
			"${sesh_ai_cef_kept_locale_directories_record}"
		COMMENT "Placing and signing Chromium Embedded Framework.framework beside the extension"
		VERBATIM
	)

	unset(sesh_ai_cef_kept_locale)
	unset(sesh_ai_cef_kept_locale_directory)
	unset(sesh_ai_cef_kept_locale_directories)
	unset(sesh_ai_cef_kept_locale_directories_argument)
	unset(sesh_ai_cef_kept_locale_directories_record)

	add_custom_target(sesh_ai_cef_framework ALL
		DEPENDS "${SESH_AI_CEF_PLACED_FRAMEWORK}/Versions/A/Chromium Embedded Framework"
	)

	# -----------------------------------------------------------------------
	# The macOS helper bundles
	#
	# One bundle per entry of CEF_HELPER_APP_SUFFIXES, as cefsimple builds them: the plain
	# helper, then (Alerts), (GPU), (Plugin) and (Renderer). Each entry is
	# "<name suffix>:<target suffix>:<plist suffix>".
	#
	# The extension names only the plain one, as `browser_subprocess_path`. Chromium
	# derives the others from it, which was confirmed in the source rather than assumed:
	# `ChildProcessHost::GetChildPath` (content/browser/child_process_host_impl.cc at
	# 154.0.8037.58) goes four directories up from that path and appends
	# "<base name><suffix>.app/Contents/MacOS/<base name><suffix>", with " (Renderer)"
	# and " (GPU)" from content/public/app/mac_helpers.gni and " (Alerts)" from
	# chrome::kMacHelperSuffixAlerts. It only does so when Chromium believes it is
	# bundled, which CEF decides from `main_bundle_path`
	# (src/entry/cef_framework_location.h). Chromium 154 has no process type that asks for
	# " (Plugin)"; it is built because CEF's list names it.
	#
	# Each bundle gets its own Info.plist, from cmake/cef_helper_Info.plist.in, which is
	# cefsimple's tests/cefsimple/mac/helper-Info.plist.in at g564dd6c with this project's
	# identifier, version, and macOS floor. Configured per bundle at configure time rather
	# than through MACOSX_BUNDLE_INFO_PLIST's own substitution, which reads directory-scope
	# variables when the project is generated and would give all five the last bundle's
	# values; cefsimple works around the same thing.
	#
	# Under the Unix Makefiles generator the four suffixed helpers relink and re-sign on
	# every build, about a second in all. Their executable names end in ")", which GNU
	# make reads as archive-member syntax, so it never finds the file and always remakes
	# it (`make -d` reports the existing executable as "does not exist"). The names are
	# Chromium's to choose, so this is accepted rather than worked around.
	# -----------------------------------------------------------------------
	set(SESH_AI_CEF_HELPER_BUNDLE_IDENTIFIER_BASE "app.sesh-ai.reaper-extension.helper")

	foreach(sesh_ai_helper_suffix_entry IN LISTS CEF_HELPER_APP_SUFFIXES)
		string(REPLACE ":" ";" sesh_ai_helper_suffixes "${sesh_ai_helper_suffix_entry}")
		list(GET sesh_ai_helper_suffixes 0 sesh_ai_helper_name_suffix)
		list(GET sesh_ai_helper_suffixes 1 sesh_ai_helper_target_suffix)
		list(GET sesh_ai_helper_suffixes 2 sesh_ai_helper_bundle_identifier_suffix)

		set(sesh_ai_helper_target "sesh_ai_cef_subprocess_helper${sesh_ai_helper_target_suffix}")

		# Read by cmake/cef_helper_Info.plist.in.
		set(SESH_AI_CEF_HELPER_EXECUTABLE_NAME
			"${SESH_AI_CEF_SUBPROCESS_HELPER_OUTPUT_NAME}${sesh_ai_helper_name_suffix}"
		)
		set(SESH_AI_CEF_HELPER_BUNDLE_IDENTIFIER
			"${SESH_AI_CEF_HELPER_BUNDLE_IDENTIFIER_BASE}${sesh_ai_helper_bundle_identifier_suffix}"
		)

		set(sesh_ai_helper_information_property_list
			"${CMAKE_CURRENT_BINARY_DIR}/cef_helper_information_property_lists/${sesh_ai_helper_target}.plist"
		)

		configure_file(
			"${CMAKE_CURRENT_SOURCE_DIR}/cmake/cef_helper_Info.plist.in"
			"${sesh_ai_helper_information_property_list}"
			@ONLY
		)

		if(sesh_ai_helper_bundle_identifier_suffix STREQUAL ".renderer")
			set(sesh_ai_helper_entitlements "${SESH_AI_CEF_HELPER_ENTITLEMENTS_DIRECTORY}/helper-renderer-entitlements.plist")
		elseif(sesh_ai_helper_bundle_identifier_suffix STREQUAL ".gpu")
			set(sesh_ai_helper_entitlements "${SESH_AI_CEF_HELPER_ENTITLEMENTS_DIRECTORY}/helper-gpu-entitlements.plist")
		else()
			set(sesh_ai_helper_entitlements "${SESH_AI_CEF_HELPER_ENTITLEMENTS_DIRECTORY}/helper-entitlements.plist")
		endif()

		sesh_ai_add_cef_subprocess_helper(${sesh_ai_helper_target}
			"${SESH_AI_CEF_HELPER_EXECUTABLE_NAME}"
			MACOSX_BUNDLE
		)

		# LINK_DEPENDS so that a changed Info.plist or entitlements file relinks the
		# helper, and the relink re-signs it. Otherwise the bundle would keep a signature
		# over the old plist and fail verification.
		set_target_properties(${sesh_ai_helper_target} PROPERTIES
			MACOSX_BUNDLE_INFO_PLIST "${sesh_ai_helper_information_property_list}"
			LINK_DEPENDS "${sesh_ai_helper_information_property_list};${sesh_ai_helper_entitlements}"
		)

		add_custom_command(TARGET ${sesh_ai_helper_target} POST_BUILD
			COMMAND codesign --force
				--sign "${SESH_AI_MACOS_CODE_SIGN_IDENTITY}"
				--options runtime
				"${sesh_ai_code_sign_timestamp_argument}"
				--entitlements "${sesh_ai_helper_entitlements}"
				"$<TARGET_BUNDLE_DIR:${sesh_ai_helper_target}>"
			COMMENT "Signing ${SESH_AI_CEF_HELPER_EXECUTABLE_NAME}.app"
			VERBATIM
		)

		list(APPEND SESH_AI_CEF_SUBPROCESS_HELPER_TARGETS ${sesh_ai_helper_target})
	endforeach()

	unset(sesh_ai_helper_suffix_entry)
	unset(sesh_ai_helper_suffixes)
	unset(sesh_ai_helper_name_suffix)
	unset(sesh_ai_helper_target_suffix)
	unset(sesh_ai_helper_bundle_identifier_suffix)
	unset(sesh_ai_helper_target)
	unset(sesh_ai_helper_information_property_list)
	unset(sesh_ai_helper_entitlements)
	unset(sesh_ai_code_sign_timestamp_argument)

	set(SESH_AI_CEF_RUNTIME_TARGET sesh_ai_cef_framework)

	message(STATUS
		"Sesh AI extension: CEF helper bundles ${SESH_AI_CEF_SUBPROCESS_HELPER_TARGETS} configured, "
		"signed with identity '${SESH_AI_MACOS_CODE_SIGN_IDENTITY}'"
	)
elseif(SESH_AI_CEF_FOUND)
	sesh_ai_add_cef_subprocess_helper(sesh_ai_cef_subprocess_helper
		"${SESH_AI_CEF_SUBPROCESS_HELPER_OUTPUT_NAME}"
	)

	list(APPEND SESH_AI_CEF_SUBPROCESS_HELPER_TARGETS sesh_ai_cef_subprocess_helper)

	# -----------------------------------------------------------------------
	# libcef and its companions, copied beside the helper
	#
	# What cefsimple's COPY_FILES does for its own executable: every file in
	# CEF_BINARY_FILES and CEF_RESOURCE_FILES, which cef_variables.cmake lists per
	# platform, except that the locales directory gets only SESH_AI_CEF_KEPT_LOCALES. One
	# custom command per file, so each is copied only when the distribution's copy
	# changes.
	# -----------------------------------------------------------------------
	set(sesh_ai_cef_runtime_file_sources "")
	set(sesh_ai_cef_runtime_file_outputs "")
	set(sesh_ai_cef_runtime_outputs "")

	foreach(sesh_ai_cef_runtime_file IN LISTS CEF_BINARY_FILES)
		list(APPEND sesh_ai_cef_runtime_file_sources "${CEF_BINARY_DIR}/${sesh_ai_cef_runtime_file}")
		list(APPEND sesh_ai_cef_runtime_file_outputs "${SESH_AI_RUNTIME_OUTPUT_DIRECTORY}/${sesh_ai_cef_runtime_file}")
	endforeach()

	# cefsimple copies it when the distribution has it, and so does this.
	if(SESH_AI_PLATFORM_LINUX AND EXISTS "${CEF_BINARY_DIR}/libminigbm.so")
		list(APPEND sesh_ai_cef_runtime_file_sources "${CEF_BINARY_DIR}/libminigbm.so")
		list(APPEND sesh_ai_cef_runtime_file_outputs "${SESH_AI_RUNTIME_OUTPUT_DIRECTORY}/libminigbm.so")
	endif()

	foreach(sesh_ai_cef_runtime_file IN LISTS CEF_RESOURCE_FILES)
		if(sesh_ai_cef_runtime_file STREQUAL "locales")
			# File by file, and only the kept ones. A build directory that an earlier
			# configuration filled with every locale keeps the others in its locales/, but
			# the install rules install this list and not the directory, so the tree that
			# ships is trimmed regardless.
			foreach(sesh_ai_cef_kept_locale IN LISTS SESH_AI_CEF_KEPT_LOCALES)
				set(sesh_ai_cef_locale_file_source
					"${CEF_RESOURCE_DIR}/locales/${sesh_ai_cef_kept_locale}.pak"
				)
				set(sesh_ai_cef_locale_file_output
					"${SESH_AI_RUNTIME_OUTPUT_DIRECTORY}/locales/${sesh_ai_cef_kept_locale}.pak"
				)

				# At configure time, so that CI's Windows and Linux legs check the names
				# on every push rather than the first release finding one wrong.
				if(NOT EXISTS "${sesh_ai_cef_locale_file_source}")
					message(FATAL_ERROR
						"Sesh AI extension: SESH_AI_CEF_KEPT_LOCALES names '${sesh_ai_cef_kept_locale}', "
						"but the CEF distribution has no '${sesh_ai_cef_locale_file_source}'. "
						"Correct the name to the one in '${CEF_RESOURCE_DIR}/locales', or remove it "
						"from the list; the list's comment in CMakeLists.txt says where the names come from."
					)
				endif()

				add_custom_command(
					OUTPUT "${sesh_ai_cef_locale_file_output}"
					COMMAND "${CMAKE_COMMAND}" -E make_directory "${SESH_AI_RUNTIME_OUTPUT_DIRECTORY}/locales"
					COMMAND "${CMAKE_COMMAND}" -E copy
						"${sesh_ai_cef_locale_file_source}"
						"${sesh_ai_cef_locale_file_output}"
					DEPENDS "${sesh_ai_cef_locale_file_source}"
					VERBATIM
				)

				list(APPEND sesh_ai_cef_runtime_outputs "${sesh_ai_cef_locale_file_output}")
				list(APPEND SESH_AI_CEF_LOCALE_FILES_BESIDE_BINARY "${sesh_ai_cef_locale_file_output}")
			endforeach()
		elseif(IS_DIRECTORY "${CEF_RESOURCE_DIR}/${sesh_ai_cef_runtime_file}")
			# locales is the only directory CEF has ever listed here. A new one needs a
			# decision about what of it ships, so it stops configuration rather than
			# being copied, or skipped, without one.
			message(FATAL_ERROR
				"Sesh AI extension: CEF_RESOURCE_FILES names the directory "
				"'${sesh_ai_cef_runtime_file}', which this build does not know how to place "
				"beside the extension. Add it to the CEF runtime section of CMakeLists.txt."
			)
		else()
			list(APPEND sesh_ai_cef_runtime_file_sources "${CEF_RESOURCE_DIR}/${sesh_ai_cef_runtime_file}")
			list(APPEND sesh_ai_cef_runtime_file_outputs "${SESH_AI_RUNTIME_OUTPUT_DIRECTORY}/${sesh_ai_cef_runtime_file}")
		endif()
	endforeach()

	foreach(sesh_ai_cef_runtime_file_source sesh_ai_cef_runtime_file_output
		IN ZIP_LISTS sesh_ai_cef_runtime_file_sources sesh_ai_cef_runtime_file_outputs)
		add_custom_command(
			OUTPUT "${sesh_ai_cef_runtime_file_output}"
			COMMAND "${CMAKE_COMMAND}" -E copy
				"${sesh_ai_cef_runtime_file_source}"
				"${sesh_ai_cef_runtime_file_output}"
			DEPENDS "${sesh_ai_cef_runtime_file_source}"
			VERBATIM
		)

		list(APPEND sesh_ai_cef_runtime_outputs "${sesh_ai_cef_runtime_file_output}")
	endforeach()

	add_custom_target(sesh_ai_cef_runtime ALL DEPENDS ${sesh_ai_cef_runtime_outputs})

	set(SESH_AI_CEF_RUNTIME_FILES_BESIDE_BINARY ${sesh_ai_cef_runtime_file_outputs})

	# The helper in bin/ is only runnable with libcef beside it.
	add_dependencies(sesh_ai_cef_subprocess_helper sesh_ai_cef_runtime)

	set(SESH_AI_CEF_RUNTIME_TARGET sesh_ai_cef_runtime)

	if(SESH_AI_PLATFORM_LINUX)
		# -------------------------------------------------------------------
		# chrome-sandbox (requirement 15.7's "process sandboxing on Linux")
		#
		# Copied above with the rest of CEF_BINARY_FILES, beside the helper. Chromium
		# only uses it when it is owned by root with mode 4755, which no build step can
		# arrange, so this prints the command CEF's own SET_LINUX_SUID_PERMISSIONS prints
		# for cefsimple, and the permissions are installation's job. The install rules
		# below install it and print the same command for the installed copy.
		#
		# Placing it is not enough on its own, and nothing in this build changes that.
		# Chromium looks for chrome-sandbox beside the host executable, which is REAPER's,
		# or at CHROME_DEVEL_SANDBOX when the host executable is owned by the user running
		# it (`SetuidSandboxHost::GetSandboxBinaryPath` in
		# sandbox/linux/suid/client/setuid_sandbox_host.cc at 154.0.8037.58). CEF has no
		# setting for it. And Chromium only reaches for it at all when unprivileged user
		# namespaces are unavailable; where they are, the namespace sandbox is used and
		# chrome-sandbox is never run. Without either sandbox Chromium stops rather than
		# run the renderer unsandboxed, unless CEF is told `no_sandbox`, which is the
		# only development mode that needs neither. The extension does not do that:
		# `process_sandbox_enabled` is true on Linux (src/ui/ui_host.h). Whether REAPER
		# on the supported distributions gets the namespace sandbox is task 28's to find.
		# -------------------------------------------------------------------
		SET_LINUX_SUID_PERMISSIONS(sesh_ai_cef_subprocess_helper
			"${SESH_AI_RUNTIME_OUTPUT_DIRECTORY}/chrome-sandbox"
		)

		message(STATUS
			"Sesh AI extension: chrome-sandbox is copied beside the helper without the root "
			"ownership and setuid bit Chromium requires of it; the build prints the command "
			"that sets them"
		)
	endif()

	unset(sesh_ai_cef_runtime_file)
	unset(sesh_ai_cef_kept_locale)
	unset(sesh_ai_cef_locale_file_source)
	unset(sesh_ai_cef_locale_file_output)
	unset(sesh_ai_cef_runtime_file_source)
	unset(sesh_ai_cef_runtime_file_output)
	unset(sesh_ai_cef_runtime_file_sources)
	unset(sesh_ai_cef_runtime_file_outputs)
	unset(sesh_ai_cef_runtime_outputs)

	message(STATUS
		"Sesh AI extension: CEF subprocess helper target sesh_ai_cef_subprocess_helper configured "
		"(${SESH_AI_CEF_SUBPROCESS_HELPER_OUTPUT_NAME})"
	)
endif()

# Building the extension builds the helpers it needs and puts the CEF runtime beside it,
# so a development build's bin/ is the layout the extension looks for. The helpers and
# the runtime target exist whenever the library does, because both depend on CEF and the
# library is never created without it.
if(TARGET sesh_ai_extension)
	add_dependencies(sesh_ai_extension ${SESH_AI_CEF_SUBPROCESS_HELPER_TARGETS} ${SESH_AI_CEF_RUNTIME_TARGET})
endif()

# ---------------------------------------------------------------------------
# Install rules (task 27.1, requirements 15.1, 21.4 and 27.1)
#
# `cmake --install <build directory> --prefix <directory>` produces the tree a producer
# unpacks into REAPER's UserPlugins. It is one flat directory, laid out the way the
# build already lays out bin/ (see "The CEF runtime beside the binary" above), plus the
# chat panel's files and the notices. The extension resolves everything from the
# directory its own binary was loaded from (src/entry/panel_asset_directory.h,
# cef_subprocess_helper_location.h and cef_framework_location.h), so the prefix stands
# for that directory and every DESTINATION is relative to it:
#
#   macOS    reaper_sesh_ai.dylib
#            Chromium Embedded Framework.framework/, its locales trimmed to
#            SESH_AI_CEF_KEPT_LOCALES
#            Sesh AI Helper.app/, and the (Alerts), (GPU), (Plugin) and (Renderer) ones
#            sesh_ai_ui/
#            sesh_ai_licenses/
#
#   Windows  reaper_sesh_ai.dll, sesh_ai_cef_helper.exe, CEF_BINARY_FILES,
#            CEF_RESOURCE_FILES with locales/ trimmed to SESH_AI_CEF_KEPT_LOCALES,
#            sesh_ai_ui/, sesh_ai_licenses/
#
#   Linux    reaper_sesh_ai.so, sesh_ai_cef_helper, CEF_BINARY_FILES including
#            chrome-sandbox, CEF_RESOURCE_FILES with locales/ trimmed to
#            SESH_AI_CEF_KEPT_LOCALES, sesh_ai_ui/, sesh_ai_licenses/
#
# What does not ship: the Windows import library (below, with the extension binary),
# the Vite source map (with the chat panel's files), and this repository's SECURITY.md
# and DEPENDENCIES.md. Both documents are about the repository rather than the
# installed extension: SECURITY.md tells a reader how to report a vulnerability to the
# repository, where it already is, and DEPENDENCIES.md is build instructions. In
# UserPlugins, which every extension shares, they would also be two more generic names
# for another extension's files to collide with.
#
# Not GNUInstallDirs. Its bin/, lib/ and share/ describe a system prefix, and the
# resolvers look beside the binary and nowhere else, so a split tree would leave the
# extension unable to find its helper, its framework or its files.
#
# The default prefix is inside the build directory rather than CMake's /usr/local. The
# tree belongs in one REAPER's UserPlugins and nowhere system-wide, and a bare
# `cmake --install` into /usr/local would scatter libcef and locales/ there. Which REAPER
# to install into is the producer's choice, so it is always an explicit --prefix.
#
# Installed from the build's output rather than from the CEF distribution, so what is
# installed is what the build placed and signed.
#
# Only with the library. A configure that builds just the Catch2 suite has nothing to
# install, and the library always comes with CEF.
# ---------------------------------------------------------------------------

if(TARGET sesh_ai_extension)
	if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
		set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH
			"Where `cmake --install` puts the tree a producer unpacks into REAPER's UserPlugins"
			FORCE
		)
	endif()

	# -----------------------------------------------------------------------
	# The chat panel's files (requirement 21.4)
	#
	# Vite's output, which npm builds and CMake does not: CI builds it once and hands it
	# to every build leg as the `ui-static-assets` artifact, downloaded into ui/dist, and
	# the release workflow runs `npm run build` in ui/ before configuring. Both leave it
	# at the path below, so the rule reads it from there.
	#
	# A missing ui/dist stops the install rather than producing a tree whose panel has
	# nothing to load. It is checked at install time, not configure time, because
	# building the UI after configuring is a normal order to do things in. The check is
	# the first install rule, so it fails before any of CEF is copied.
	#
	# The source map is not installed. Vite writes it for developers working in ui/,
	# where the sources it maps to are; the installed panel has no use for it, and it is
	# about 2 MB, nearly five times the script it describes. The script keeps its
	# sourceMappingURL comment, which matters only if DevTools is opened on the panel and
	# then costs a missing-file warning.
	#
	# The same build writes the notices of the npm packages compiled into the bundle,
	# which the minified script keeps none of, to ui/dist/third-party-licenses.md. The
	# name is BUNDLED_PACKAGE_NOTICES_FILE_NAME in ui/vite.config.ts, and the two change
	# together. It is installed with the other notices below rather than into
	# sesh_ai_ui/, which holds only what the panel loads. A ui/dist without it is from a
	# build that predates it, so it stops the install the way a missing index.html does,
	# and an archive can never carry the bundle without its notices.
	# -----------------------------------------------------------------------
	set(SESH_AI_UI_BUILD_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ui/dist")
	set(SESH_AI_UI_BUNDLED_PACKAGE_NOTICES_FILE_NAME "third-party-licenses.md")

	install(CODE "
		if(NOT EXISTS \"${SESH_AI_UI_BUILD_OUTPUT_DIRECTORY}/index.html\")
			message(FATAL_ERROR
				\"Sesh AI extension: cannot install the chat panel's files, because \"
				\"'${SESH_AI_UI_BUILD_OUTPUT_DIRECTORY}/index.html' does not exist. ui/dist is \"
				\"built by npm, not by CMake: run `npm ci && npm run build` in \"
				\"'${CMAKE_CURRENT_SOURCE_DIR}/ui' (Node as pinned in .nvmrc), then install again.\"
			)
		endif()

		if(NOT EXISTS \"${SESH_AI_UI_BUILD_OUTPUT_DIRECTORY}/${SESH_AI_UI_BUNDLED_PACKAGE_NOTICES_FILE_NAME}\")
			message(FATAL_ERROR
				\"Sesh AI extension: cannot install the notices of the npm packages in the chat \"
				\"panel's bundle, because '${SESH_AI_UI_BUILD_OUTPUT_DIRECTORY}/${SESH_AI_UI_BUNDLED_PACKAGE_NOTICES_FILE_NAME}' \"
				\"does not exist, so ui/dist was built by an older ui/vite.config.ts. Run \"
				\"`npm ci && npm run build` in '${CMAKE_CURRENT_SOURCE_DIR}/ui', then install again.\"
			)
		endif()
	")

	# The trailing slash installs the directory's contents as sesh_ai_ui/ rather than a
	# dist/ inside it. An install into a prefix that already holds an older build keeps
	# that build's hashed asset files beside the new ones, so a release installs into an
	# empty directory.
	install(DIRECTORY "${SESH_AI_UI_BUILD_OUTPUT_DIRECTORY}/"
		DESTINATION "${SESH_AI_APPLICATION_ASSET_DIRECTORY_NAME}"
		PATTERN "*.map" EXCLUDE
		PATTERN "${SESH_AI_UI_BUNDLED_PACKAGE_NOTICES_FILE_NAME}" EXCLUDE
	)

	# -----------------------------------------------------------------------
	# The notices of the libraries linked into the binaries
	#
	# The two vcpkg ports compiled into the extension are MIT licensed, and MIT asks for
	# its notice in every copy, which a static link is. The notice is the `copyright`
	# file vcpkg installs for each port, taken from there rather than restated here so it
	# is always the text of the version linked. DEPENDENCIES.md lists what else is linked
	# and why it needs no notice of its own.
	#
	# Without vcpkg the libraries come from the system and there is no such file, so
	# the install stops rather than shipping the binary without their notices. Checked
	# at install time, beside the ui/dist check, so a vcpkg-less build still builds.
	# -----------------------------------------------------------------------
	set(SESH_AI_LINKED_LIBRARY_NOTICE_FILES "")
	set(sesh_ai_missing_linked_library_notice_files "")

	foreach(sesh_ai_linked_vcpkg_port IN ITEMS nlohmann-json json-schema-validator)
		set(sesh_ai_linked_library_notice_file
			"${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${sesh_ai_linked_vcpkg_port}/copyright"
		)

		if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET
			AND EXISTS "${sesh_ai_linked_library_notice_file}")
			list(APPEND SESH_AI_LINKED_LIBRARY_NOTICE_FILES "${sesh_ai_linked_library_notice_file}")
		else()
			list(APPEND sesh_ai_missing_linked_library_notice_files "${sesh_ai_linked_vcpkg_port}")
		endif()
	endforeach()

	if(sesh_ai_missing_linked_library_notice_files)
		string(JOIN ", " sesh_ai_missing_linked_library_notice_ports ${sesh_ai_missing_linked_library_notice_files})

		install(CODE "
			message(FATAL_ERROR
				\"Sesh AI extension: cannot install the notices for ${sesh_ai_missing_linked_library_notice_ports}, \"
				\"which are linked into the extension. They are read from the copyright file vcpkg \"
				\"installs for each port, and this build was not configured with vcpkg. Configure \"
				\"with the vcpkg toolchain (DEPENDENCIES.md) and install again.\"
			)
		")
	endif()

	unset(sesh_ai_linked_vcpkg_port)
	unset(sesh_ai_linked_library_notice_file)
	unset(sesh_ai_missing_linked_library_notice_files)
	unset(sesh_ai_missing_linked_library_notice_ports)

	# -----------------------------------------------------------------------
	# The extension binary
	#
	# The Windows import library is the ARCHIVE artifact of a DLL, and it is link-time
	# only. Without an ARCHIVE destination CMake would install it to lib/, so it is given
	# one in a component nothing installs by default.
	#
	# The macOS dylib is installed as the linker left it, which on x86_64 is unsigned and
	# on arm64 is the linker's ad hoc signature. REAPER loads either. The build signs the
	# framework and the helpers with SESH_AI_MACOS_CODE_SIGN_IDENTITY and not the dylib,
	# so notarization, which needs every Mach-O signed with a Developer ID, is the
	# release workflow's step on the installed tree, alongside the rest of notarization.
	# -----------------------------------------------------------------------
	install(TARGETS sesh_ai_extension
		LIBRARY DESTINATION .
		RUNTIME DESTINATION .
		ARCHIVE DESTINATION . COMPONENT sesh_ai_link_time_only EXCLUDE_FROM_ALL
	)

	if(SESH_AI_PLATFORM_MACOS)
		# -------------------------------------------------------------------
		# The framework and the five helper bundles, signatures intact
		#
		# All six are signed in the build tree, so installing must not change a byte
		# inside them. install(DIRECTORY) copies each file's contents as they are and
		# recreates a symbolic link as a link rather than following it, which keeps the
		# framework's Versions/Current and its three top-level links; codesign refuses
		# a framework without them. USE_SOURCE_PERMISSIONS keeps the executables
		# executable, which install(DIRECTORY)'s default file permissions would not.
		#
		# Not install(TARGETS) for the helpers. It installs a bundle and then treats the
		# executable inside it as its own: it rewrites install names and run-paths when
		# they differ from the build tree's, and `cmake --install --strip` strips it.
		# Either one changes a signed file after it was signed.
		# -------------------------------------------------------------------
		install(DIRECTORY "${SESH_AI_CEF_PLACED_FRAMEWORK}"
			DESTINATION .
			USE_SOURCE_PERMISSIONS
		)

		foreach(sesh_ai_helper_target IN LISTS SESH_AI_CEF_SUBPROCESS_HELPER_TARGETS)
			install(DIRECTORY "$<TARGET_BUNDLE_DIR:${sesh_ai_helper_target}>"
				DESTINATION .
				USE_SOURCE_PERMISSIONS
			)
		endforeach()

		unset(sesh_ai_helper_target)
	else()
		# -------------------------------------------------------------------
		# The helper, and CEF beside it
		#
		# Both binaries already carry the run-path they ship with on Linux
		# (BUILD_WITH_INSTALL_RPATH), so installing rewrites nothing, and Windows has
		# no run-paths. The CEF files are the copies in the output directory, split
		# into what has to be executable and what is data. The locales are the kept
		# ones the build copied, file by file, so a build directory's locales/ holding
		# more from an earlier configuration does not widen what ships.
		#
		# CEF's own import library, libcef.lib, is not in CEF_BINARY_FILES and so is
		# never copied beside the binary, let alone installed.
		# -------------------------------------------------------------------
		install(TARGETS ${SESH_AI_CEF_SUBPROCESS_HELPER_TARGETS} RUNTIME DESTINATION .)

		set(sesh_ai_cef_installed_programs "")
		set(sesh_ai_cef_installed_data_files "")

		foreach(sesh_ai_cef_runtime_file IN LISTS SESH_AI_CEF_RUNTIME_FILES_BESIDE_BINARY)
			get_filename_component(sesh_ai_cef_runtime_file_name "${sesh_ai_cef_runtime_file}" NAME)

			# libvulkan.so.1 is why the version suffix is matched.
			if(sesh_ai_cef_runtime_file_name MATCHES "\\.(dll|exe|so)(\\.[0-9]+)*$"
				OR sesh_ai_cef_runtime_file_name STREQUAL "chrome-sandbox")
				list(APPEND sesh_ai_cef_installed_programs "${sesh_ai_cef_runtime_file}")
			else()
				list(APPEND sesh_ai_cef_installed_data_files "${sesh_ai_cef_runtime_file}")
			endif()
		endforeach()

		install(PROGRAMS ${sesh_ai_cef_installed_programs} DESTINATION .)
		install(FILES ${sesh_ai_cef_installed_data_files} DESTINATION .)
		install(FILES ${SESH_AI_CEF_LOCALE_FILES_BESIDE_BINARY} DESTINATION locales)

		unset(sesh_ai_cef_runtime_file)
		unset(sesh_ai_cef_runtime_file_name)
		unset(sesh_ai_cef_installed_programs)
		unset(sesh_ai_cef_installed_data_files)

		if(SESH_AI_PLATFORM_LINUX)
			# chrome-sandbox is installed mode 755 and owned by whoever installed it.
			# Chromium uses it only when it is owned by root with mode 4755, and an
			# install rule cannot make a file setuid root. Setting that belongs to the
			# producer's installation instructions or to a package's install step, as
			# 26.2 decided for the build tree, so this prints the command for the
			# installed copy the way the build prints it for bin/.
			install(CODE "
				message(STATUS
					\"Sesh AI extension: chrome-sandbox is installed without the root ownership and \"
					\"setuid bit Chromium requires of it. The producer's installation, or a package, \"
					\"sets them: sudo chown root:root '\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/chrome-sandbox' \"
					\"&& sudo chmod 4755 '\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/chrome-sandbox'\"
				)
			")
		endif()
	endif()

	# -----------------------------------------------------------------------
	# Notices
	#
	# All in sesh_ai_licenses/, never loose beside the binary. UserPlugins is shared by
	# every extension a producer has installed, and LICENSE, LICENSE.txt and
	# CREDITS.html are names another extension could ship too, so one install would
	# overwrite the other's. Inside the directory each notice keeps the name its source
	# gives it, under a directory naming the source, so provenance is readable from the
	# path:
	#
	#   sesh_ai_licenses/LICENSE                          this repository, MIT
	#   sesh_ai_licenses/cef/LICENSE.txt                  CEF, BSD; also covers
	#                                                     libcef_dll_wrapper, linked into
	#                                                     the extension and the helpers
	#   sesh_ai_licenses/cef/CREDITS.html                 Chromium and its third parties
	#   sesh_ai_licenses/nlohmann-json/copyright          MIT, statically linked
	#   sesh_ai_licenses/json-schema-validator/copyright  MIT, statically linked
	#   sesh_ai_licenses/ui/third-party-licenses.md       the npm packages compiled into
	#                                                     the chat panel's bundle
	# -----------------------------------------------------------------------
	set(SESH_AI_LICENSE_DIRECTORY_NAME "sesh_ai_licenses")

	install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
		DESTINATION "${SESH_AI_LICENSE_DIRECTORY_NAME}"
	)

	install(FILES "${SESH_AI_UI_BUILD_OUTPUT_DIRECTORY}/${SESH_AI_UI_BUNDLED_PACKAGE_NOTICES_FILE_NAME}"
		DESTINATION "${SESH_AI_LICENSE_DIRECTORY_NAME}/ui"
	)

	install(FILES
		"${CEF_ROOT}/LICENSE.txt"
		"${CEF_ROOT}/CREDITS.html"
		DESTINATION "${SESH_AI_LICENSE_DIRECTORY_NAME}/cef"
	)

	foreach(sesh_ai_linked_library_notice_file IN LISTS SESH_AI_LINKED_LIBRARY_NOTICE_FILES)
		# The port's share/ directory is named after the port.
		get_filename_component(sesh_ai_linked_library_notice_directory "${sesh_ai_linked_library_notice_file}" DIRECTORY)
		get_filename_component(sesh_ai_linked_vcpkg_port "${sesh_ai_linked_library_notice_directory}" NAME)

		install(FILES "${sesh_ai_linked_library_notice_file}"
			DESTINATION "${SESH_AI_LICENSE_DIRECTORY_NAME}/${sesh_ai_linked_vcpkg_port}"
		)
	endforeach()

	unset(sesh_ai_linked_library_notice_file)
	unset(sesh_ai_linked_library_notice_directory)
	unset(sesh_ai_linked_vcpkg_port)
endif()

# ---------------------------------------------------------------------------
# Tests
#
# The Catch2 suite covers the REAPER-independent logic: codec, validator,
# resolver, context builder.
#
# Catch2 v3 comes from vcpkg when a toolchain is configured. Without one it is
# fetched from source at the same pinned version, verified against the digest of
# the release archive. The suite is the part of this build a contributor is most
# likely to want to run on a machine with nothing installed, so it is worth not
# making that conditional on a package manager.
# ---------------------------------------------------------------------------

enable_testing()

option(SESH_AI_FETCH_CATCH2
	"Fetch Catch2 from source when it is not provided by the dependency toolchain" ON
)

set(SESH_AI_CATCH2_VERSION "3.16.0")
set(SESH_AI_CATCH2_ARCHIVE_SHA256 "0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34")

find_package(Catch2 3 CONFIG QUIET)

if(NOT Catch2_FOUND AND SESH_AI_FETCH_CATCH2)
	message(STATUS "Sesh AI extension: fetching Catch2 ${SESH_AI_CATCH2_VERSION} from source")

	FetchContent_Declare(Catch2
		URL "https://github.com/catchorg/Catch2/archive/refs/tags/v${SESH_AI_CATCH2_VERSION}.tar.gz"
		URL_HASH "SHA256=${SESH_AI_CATCH2_ARCHIVE_SHA256}"
		# Timestamp extracted files at extraction time rather than carrying the
		# archive's own timestamps, so a version bump actually triggers a rebuild.
		DOWNLOAD_EXTRACT_TIMESTAMP FALSE
	)

	FetchContent_MakeAvailable(Catch2)

	# catch_discover_tests lives in the extras directory of the source tree, which
	# a config-mode find_package would have put on the module path for us.
	list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
elseif(Catch2_FOUND)
	message(STATUS "Sesh AI extension: Catch2 ${Catch2_VERSION}")
endif()

add_subdirectory(tests)
