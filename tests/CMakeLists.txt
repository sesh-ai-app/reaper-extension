# Catch2 suite over the REAPER-independent logic.
#
# Nothing here links against the extension's shared library. The library is a
# REAPER plugin and loading it outside REAPER is not meaningful, so the logic
# under test is compiled into the test binary directly as the source domains grow
# — envelope codec, schema validator, track resolver, context builder,
# folder-depth arithmetic, unit conversion.

if(NOT TARGET Catch2::Catch2WithMain)
	message(STATUS
		"Sesh AI extension: Catch2 is unavailable, so the test target is not created. Configure with a "
		"vcpkg toolchain, or leave SESH_AI_FETCH_CATCH2 on and allow the source fetch."
	)
	return()
endif()

# Test sources are discovered rather than listed, for the same reason and with the
# same tradeoff as src/ in the top-level CMakeLists.txt: CONFIGURE_DEPENDS makes
# the generator re-check the glob each build, and in exchange the independent
# pieces of work that each add a test file stop colliding over one shared list.
# A file named *_test.cpp anywhere under tests/ is part of the suite.
file(GLOB_RECURSE SESH_AI_TEST_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_executable(sesh_ai_tests ${SESH_AI_TEST_SOURCES})

target_include_directories(sesh_ai_tests PRIVATE ${SESH_AI_INCLUDE_DIRECTORIES})

target_link_libraries(sesh_ai_tests PRIVATE Catch2::Catch2WithMain)

# The queue pair is the one thing in this codebase whose contract is about threads,
# so the suite actually runs threads against it rather than reasoning about the
# mutex. That needs pthreads linked explicitly on Linux.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(sesh_ai_tests PRIVATE Threads::Threads)

# The suite validates envelopes against the vendored schemas, which is also the
# practical check that the digests hold — so it must not be built, let alone run,
# against a bundle that failed verification.
add_dependencies(sesh_ai_tests sesh_ai_verify_schema_bundle)

# The JSON dependencies are optional at this point for the same reason they are
# optional for the library: there is nothing using them yet, and a contributor
# with no vcpkg should still be able to run the suite.
if(TARGET nlohmann_json::nlohmann_json)
	target_link_libraries(sesh_ai_tests PRIVATE nlohmann_json::nlohmann_json)
endif()

if(TARGET nlohmann_json_schema_validator::validator)
	target_link_libraries(sesh_ai_tests PRIVATE nlohmann_json_schema_validator::validator)
endif()

# Registers each Catch2 test case as its own ctest test, so a failure names the
# case rather than the whole binary.
include(Catch)
catch_discover_tests(sesh_ai_tests)
