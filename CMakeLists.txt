cmake_minimum_required(VERSION 3.21)

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

project(sesh-ai-extension
	VERSION 0.1.0
	DESCRIPTION "Sesh AI — REAPER extension"
	LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Language standard
#
# C++17 across every platform. No compiler extensions, so the same source
# compiles under AppleClang, MSVC, and GCC without dialect drift.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

message(STATUS "Sesh AI extension: configuring for ${SESH_AI_PLATFORM_NAME}")
message(STATUS "Sesh AI extension: output name ${SESH_AI_EXTENSION_OUTPUT_NAME}${SESH_AI_EXTENSION_SUFFIX}")

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
# module CEF ships for exactly this purpose. With -DSESH_AI_DOWNLOAD_CEF=ON the
# pinned archive is downloaded into (3) and checked against its published hash.
# That is opt-in because the archive is hundreds of megabytes.
#
# When no distribution is found, configuration reports how to get one and
# continues; the UI host is the only thing that needs CEF and it does not exist
# yet. CI sets -DSESH_AI_REQUIRE_CEF=ON so a missing distribution there is a
# configuration failure, not a quietly CEF-less build.
# ---------------------------------------------------------------------------

set(SESH_AI_CEF_VERSION "154.0.28+g564dd6c+chromium-154.0.8037.58" CACHE STRING
	"CEF binary distribution version to locate or download"
)

set(SESH_AI_CEF_ROOT "" CACHE PATH
	"Path to an extracted CEF binary distribution"
)

option(SESH_AI_DOWNLOAD_CEF
	"Download the pinned CEF binary distribution when it is not already present" OFF
)

option(SESH_AI_REQUIRE_CEF
	"Fail configuration when no CEF binary distribution can be located" OFF
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

if(NOT SESH_AI_CEF_LOCATED_ROOT AND SESH_AI_DOWNLOAD_CEF)
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
		message(FATAL_ERROR "Sesh AI extension: CEF download failed — ${sesh_ai_cef_download_message}")
	endif()

	file(ARCHIVE_EXTRACT
		INPUT "${sesh_ai_cef_archive_path}"
		DESTINATION "${SESH_AI_CEF_VENDOR_DIRECTORY}"
	)

	file(REMOVE "${sesh_ai_cef_archive_path}")

	sesh_ai_locate_cef_distribution(SESH_AI_CEF_LOCATED_ROOT)

	if(NOT SESH_AI_CEF_LOCATED_ROOT)
		message(FATAL_ERROR
			"Sesh AI extension: the downloaded CEF archive did not extract to "
			"${SESH_AI_CEF_DEFAULT_ROOT}. Extract it by hand and pass -DSESH_AI_CEF_ROOT=<path>."
		)
	endif()
endif()

set(SESH_AI_CEF_FOUND OFF)

if(SESH_AI_CEF_LOCATED_ROOT)
	set(SESH_AI_CEF_FOUND ON)

	# FindCEF and cef_variables come from the distribution itself. CEF_ROOT is the
	# name they expect, and find_package(CEF) fills in CEF_BINARY_DIR,
	# CEF_RESOURCE_DIR, CEF_LIBCEF_DLL_WRAPPER_PATH, and the per-configuration
	# library variables.
	set(CEF_ROOT "${SESH_AI_CEF_LOCATED_ROOT}")
	list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")

	find_package(CEF REQUIRED)

	# The wrapper is distributed as source and has to be compiled with the same
	# settings as the code that consumes it, so it is built here rather than
	# linked from a prebuilt archive.
	add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)

	# The runtime artifact differs per platform, and so does what packaging has to
	# do with it. Recording it once keeps the packaging and UI host work from
	# re-deriving these paths.
	if(SESH_AI_PLATFORM_MACOS)
		# A framework bundle that is embedded in the extension's bundle and loaded
		# through @rpath. Helper processes are separate .app bundles and each one
		# needs its own signature (requirement 15.7).
		set(SESH_AI_CEF_RUNTIME_ARTIFACT "${CEF_BINARY_DIR}/Chromium Embedded Framework.framework")
		set(SESH_AI_CEF_RUNTIME_ARTIFACT_KIND "framework")
	elseif(SESH_AI_PLATFORM_WINDOWS)
		# libcef.dll plus the import library. The loader has to sit next to the
		# extension binary in REAPER's UserPlugins directory.
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
elseif(SESH_AI_REQUIRE_CEF)
	message(FATAL_ERROR
		"Sesh AI extension: no CEF binary distribution found for ${SESH_AI_CEF_PLATFORM}, and "
		"SESH_AI_REQUIRE_CEF is ON. Configure with -DSESH_AI_DOWNLOAD_CEF=ON, or pass "
		"-DSESH_AI_CEF_ROOT=<path> pointing at an extracted "
		"${SESH_AI_CEF_DISTRIBUTION_NAME}. See DEPENDENCIES.md."
	)
else()
	message(STATUS
		"Sesh AI extension: no CEF binary distribution found for ${SESH_AI_CEF_PLATFORM}. The UI host "
		"cannot be built. Configure with -DSESH_AI_DOWNLOAD_CEF=ON to fetch the pinned distribution "
		"into third_party/cef, or pass -DSESH_AI_CEF_ROOT=<path>. See DEPENDENCIES.md."
	)
endif()

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

# The explicit exclusions the glob comment above anticipates. There are two, and they
# are two different kinds of thing.
#
# src/ui/cef_browser_host.cpp is the only translation unit that includes CEF, and CEF is
# located rather than resolved — so on a machine without a distribution it is not a file
# that fails to link, it is a file that cannot be compiled at all. Dropping it keeps the
# rest of the library buildable, which is the same tradeoff the CEF section above makes
# when it reports a missing distribution and continues. CI sets -DSESH_AI_REQUIRE_CEF=ON,
# so a build that is meant to ship cannot lose the UI host this way.
#
# Everything the UI host decides is in src/ui/ui_host.h and is unaffected: it includes no
# CEF, and src/ui/ui_host.cpp compiles either way.
#
# The second exclusion is a choice between two definitions of one function rather than a
# file being dropped. src/entry/panel_browser_creator.h declares
# `make_panel_browser_creator`, and exactly one of these two defines it:
#
#   src/entry/cef_panel_browser_creator.cpp         names CefPanelBrowserHost
#   src/entry/panel_browser_creator_without_cef.cpp returns an empty creator and a reason
#
# Compiling both is a duplicate symbol and compiling neither is an undefined one, so the
# `else` branch is as load-bearing as the `if`. The CEF-naming one compiles without a
# distribution — ui/cef_browser_host.h holds every CEF type behind an incomplete
# `cef_state` — and only fails to *link*, which is why this is a CMake decision and not a
# preprocessor one: a `#ifdef` would produce a library with an undefined symbol in it.
set(SESH_AI_CEF_PANEL_BROWSER_CREATOR
	"${CMAKE_CURRENT_SOURCE_DIR}/src/entry/cef_panel_browser_creator.cpp"
)
set(SESH_AI_PANEL_BROWSER_CREATOR_WITHOUT_CEF
	"${CMAKE_CURRENT_SOURCE_DIR}/src/entry/panel_browser_creator_without_cef.cpp"
)

if(NOT SESH_AI_CEF_FOUND)
	list(REMOVE_ITEM SESH_AI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/cef_browser_host.cpp")
	list(REMOVE_ITEM SESH_AI_SOURCES "${SESH_AI_CEF_PANEL_BROWSER_CREATOR}")

	message(STATUS
		"Sesh AI extension: src/ui/cef_browser_host.cpp and src/entry/cef_panel_browser_creator.cpp "
		"are excluded from the build — they are the only sources that need CEF, and no distribution "
		"was located. make_panel_browser_creator comes from "
		"src/entry/panel_browser_creator_without_cef.cpp instead, so the panel reports that it has "
		"no browser rather than the library failing to link."
	)
else()
	list(REMOVE_ITEM SESH_AI_SOURCES "${SESH_AI_PANEL_BROWSER_CREATOR_WITHOUT_CEF}")

	message(STATUS
		"Sesh AI extension: make_panel_browser_creator comes from "
		"src/entry/cef_panel_browser_creator.cpp"
	)
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
# So this follows CEF's pattern instead: report what is missing, skip the target
# that needs it, and let CI turn the same situation into a failure with
# -DSESH_AI_REQUIRE_EXTENSION_LIBRARY=ON. A build with no library is then
# impossible to ship by accident but perfectly possible to work in.
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

	set_target_properties(sesh_ai_extension PROPERTIES
		OUTPUT_NAME "${SESH_AI_EXTENSION_OUTPUT_NAME}"
		PREFIX ""
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
		nlohmann_json::nlohmann_json
		nlohmann_json_schema_validator::validator
	)

	# CEF, when a distribution was located. The `if` is what lets a contributor with no
	# distribution still build the library — see the exclusion of
	# src/ui/cef_browser_host.cpp above.
	#
	# Unverified: there is no distribution on the machine this was written on, so none of
	# this has been configured, let alone compiled. It follows CEF's documented usage —
	# CEF_INCLUDE_PATH on the include path, the libcef_dll_wrapper target built from
	# source above, and the per-platform link arrangement CEF's own cef_variables.cmake
	# sets up. The framework on macOS is linked by directory rather than through a
	# logical target because that is how CEF distributes it; on Windows and Linux the
	# import library and the shared object are named directly. The packaging work is the
	# first thing that will exercise any of it.
	if(SESH_AI_CEF_FOUND)
		target_include_directories(sesh_ai_extension SYSTEM PRIVATE "${CEF_INCLUDE_PATH}")

		target_link_libraries(sesh_ai_extension PRIVATE libcef_dll_wrapper)

		if(SESH_AI_PLATFORM_MACOS)
			target_link_options(sesh_ai_extension PRIVATE
				"-F${CEF_BINARY_DIR}"
				"-framework" "Chromium Embedded Framework"
			)
		elseif(SESH_AI_PLATFORM_WINDOWS)
			target_link_libraries(sesh_ai_extension PRIVATE "${SESH_AI_CEF_IMPORT_LIBRARY}")
		else()
			target_link_libraries(sesh_ai_extension PRIVATE "${SESH_AI_CEF_RUNTIME_ARTIFACT}")
		endif()

		message(STATUS "Sesh AI extension: the shared library links CEF")
	endif()

	# The library embeds the protocolVersion and validates against the bundle, so
	# it has no business being produced from files that failed verification. The
	# verification target is already in ALL; this orders it before the compile.
	add_dependencies(sesh_ai_extension sesh_ai_verify_schema_bundle)

	message(STATUS "Sesh AI extension: shared library target sesh_ai_extension configured")
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
