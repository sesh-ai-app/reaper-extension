# Places CEF's framework beside the extension binary and signs it (task 26.2,
# requirement 15.7). Run at build time with `cmake -P`; CMakeLists.txt's "The CEF runtime
# beside the binary" section says why the framework is there.
#
#   SOURCE_FRAMEWORK              the distribution's Chromium Embedded Framework.framework
#   DESTINATION_FRAMEWORK         where it goes, beside the extension binary
#   KEPT_LOCALE_DIRECTORIES       the Resources/*.lproj directories to keep, separated by
#                                 commas; CMakeLists.txt derives them from
#                                 SESH_AI_CEF_KEPT_LOCALES, where the choice is recorded
#   CODE_SIGN_IDENTITY            a codesign identity, or "-" for ad hoc
#   CODE_SIGN_TIMESTAMP_ARGUMENT  "--timestamp" or "--timestamp=none"
#
# The distribution ships a flat bundle: the executable, Libraries/ and Resources/ at the
# top. Codesign refuses to seal a framework in that shape, so this rebuilds it in the
# versioned layout cefsimple's COPY_MAC_FRAMEWORK produces, with the top-level entries as
# symlinks through Versions/Current. A symlink is followed by dyld, by NSBundle, and by
# CefScopedLibraryLoader's LoadInHelper(), so every path that names the flat layout still
# resolves.
#
# Then it removes every locale directory not in KEPT_LOCALE_DIRECTORIES. That has to
# happen here, before the signature: the seal covers every file in Resources, so a
# locale removed from the signed bundle, by the install rules or by the release workflow,
# would make `codesign --verify` fail on the framework.
#
# Then it signs, inside out, as CEF's code signing guide sets out: each Mach-O library in
# Libraries/, then the bundle. Neither gets entitlements or the Hardened Runtime flag,
# which are process properties and belong on the helpers. Nothing in the distribution is
# signed to begin with, and on Apple silicon unsigned code does not run at all.
#
# The destination is removed first, so files a previous CEF version shipped and this one
# does not cannot survive into the bundle and break its seal.

cmake_minimum_required(VERSION 3.21)

foreach(required_variable SOURCE_FRAMEWORK DESTINATION_FRAMEWORK KEPT_LOCALE_DIRECTORIES CODE_SIGN_IDENTITY CODE_SIGN_TIMESTAMP_ARGUMENT)
	if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
		message(FATAL_ERROR "PlaceMacosCefFramework.cmake: ${required_variable} is not set")
	endif()
endforeach()

if(NOT IS_DIRECTORY "${SOURCE_FRAMEWORK}")
	message(FATAL_ERROR "PlaceMacosCefFramework.cmake: '${SOURCE_FRAMEWORK}' is not a directory")
endif()

set(framework_executable_name "Chromium Embedded Framework")
set(version_directory "${DESTINATION_FRAMEWORK}/Versions/A")

file(REMOVE_RECURSE "${DESTINATION_FRAMEWORK}")
file(MAKE_DIRECTORY "${version_directory}")

# The trailing slash copies the bundle's contents rather than the bundle itself.
file(COPY "${SOURCE_FRAMEWORK}/" DESTINATION "${version_directory}")

file(CREATE_LINK "A" "${DESTINATION_FRAMEWORK}/Versions/Current" SYMBOLIC)

foreach(top_level_entry "${framework_executable_name}" "Libraries" "Resources")
	if(EXISTS "${version_directory}/${top_level_entry}")
		file(CREATE_LINK
			"Versions/Current/${top_level_entry}"
			"${DESTINATION_FRAMEWORK}/${top_level_entry}"
			SYMBOLIC
		)
	endif()
endforeach()

string(REPLACE "," ";" kept_locale_directories "${KEPT_LOCALE_DIRECTORIES}")

# A kept name the distribution does not have is a wrong name, not a locale to do without;
# en.lproj in particular is the fallback Chromium cannot run without.
foreach(kept_locale_directory IN LISTS kept_locale_directories)
	if(NOT IS_DIRECTORY "${version_directory}/Resources/${kept_locale_directory}")
		message(FATAL_ERROR
			"PlaceMacosCefFramework.cmake: '${kept_locale_directory}' is to be kept, but "
			"'${SOURCE_FRAMEWORK}/Resources' has no such directory. Correct "
			"SESH_AI_CEF_KEPT_LOCALES in CMakeLists.txt."
		)
	endif()
endforeach()

file(GLOB locale_directories LIST_DIRECTORIES true "${version_directory}/Resources/*.lproj")

foreach(locale_directory IN LISTS locale_directories)
	get_filename_component(locale_directory_name "${locale_directory}" NAME)

	if(NOT locale_directory_name IN_LIST kept_locale_directories)
		file(REMOVE_RECURSE "${locale_directory}")
	endif()
endforeach()

function(sign_code code_path)
	execute_process(
		COMMAND codesign --force --sign "${CODE_SIGN_IDENTITY}" "${CODE_SIGN_TIMESTAMP_ARGUMENT}" "${code_path}"
		RESULT_VARIABLE sign_result
		ERROR_VARIABLE sign_error
	)

	if(NOT sign_result EQUAL 0)
		message(FATAL_ERROR "PlaceMacosCefFramework.cmake: codesign failed for '${code_path}': ${sign_error}")
	endif()
endfunction()

# Only the dylibs. Libraries/ also holds vk_swiftshader_icd.json, which is data and is
# sealed with the bundle.
file(GLOB framework_libraries "${version_directory}/Libraries/*.dylib")

foreach(framework_library IN LISTS framework_libraries)
	sign_code("${framework_library}")
endforeach()

sign_code("${DESTINATION_FRAMEWORK}")
