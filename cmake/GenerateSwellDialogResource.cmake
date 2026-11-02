# Turns the panel's Win32 resource script into SWELL's form, run as a build step on
# macOS and Linux.
#
# Invoked through `cmake -P` by the custom command the top-level CMakeLists.txt
# attaches to the extension's library:
#
#   cmake -DSESH_AI_PERL_EXECUTABLE=<perl>
#         -DSESH_AI_SWELL_RESOURCE_GENERATOR=<WDL/swell/swell_resgen.pl>
#         -DSESH_AI_RESOURCE_SCRIPT=<src/entry/panel_window.rc>
#         -DSESH_AI_GENERATED_RESOURCE_DIRECTORY=<build/generated/entry>
#         -DSESH_AI_EXPECTED_DIALOG_NAME=<the DIALOGEX line's resource name>
#         -P cmake/GenerateSwellDialogResource.cmake
#
# A script rather than the generator called straight from the custom command,
# because three things about the generator are wrong for a build step. All three
# were read off `WDL/swell/swell_resgen.pl` at the pinned WDL commit:
#
#   1. It writes beside its input — `<input>.rc_mac_dlg` and `<input>.rc_mac_menu` —
#      so it cannot be pointed at src/ without generating into the source tree. The
#      script is copied into the build tree first and the generator runs on the copy.
#   2. It exits 0 when it fails to write an output. Write errors are printed and
#      counted, and the count only decides whether a summary line is printed. So
#      success here is decided by what it printed and what it wrote, not by its
#      exit status alone. With `--quiet` a clean run prints nothing at all.
#   3. It skips a dialog it cannot parse without saying so. A `DIALOGEX` line with
#      too few fields simply does not match, the output is a valid empty file, the
#      C++ compiles, and `CreateDialogParam` returns null in a producer's session —
#      on macOS and Linux only, since Windows never runs the generator. So the
#      expected dialog name is checked in the output.
#
# The outputs are deleted before the generator runs, so a failed run cannot leave a
# stale file that a later build mistakes for a fresh one. `--force` makes it rewrite
# outputs whose contents did not change: without it the generator leaves them
# untouched, their timestamps stay older than the resource script, and the custom
# command re-runs on every build.

cmake_minimum_required(VERSION 3.21)

foreach(required_variable IN ITEMS
	SESH_AI_PERL_EXECUTABLE
	SESH_AI_SWELL_RESOURCE_GENERATOR
	SESH_AI_RESOURCE_SCRIPT
	SESH_AI_GENERATED_RESOURCE_DIRECTORY
	SESH_AI_EXPECTED_DIALOG_NAME
)
	if(NOT ${required_variable})
		message(FATAL_ERROR
			"Sesh AI extension: GenerateSwellDialogResource.cmake needs -D${required_variable}=<value>."
		)
	endif()
endforeach()

get_filename_component(resource_script_name "${SESH_AI_RESOURCE_SCRIPT}" NAME)
set(copied_resource_script "${SESH_AI_GENERATED_RESOURCE_DIRECTORY}/${resource_script_name}")
set(generated_dialog_file "${copied_resource_script}_mac_dlg")
set(generated_menu_file "${copied_resource_script}_mac_menu")

file(MAKE_DIRECTORY "${SESH_AI_GENERATED_RESOURCE_DIRECTORY}")
file(REMOVE "${generated_dialog_file}" "${generated_menu_file}")
file(COPY_FILE "${SESH_AI_RESOURCE_SCRIPT}" "${copied_resource_script}")

# The interpreter is named rather than relying on the generator's `#!/usr/bin/perl`
# line or its executable bit, neither of which a configured Perl has to agree with.
execute_process(
	COMMAND "${SESH_AI_PERL_EXECUTABLE}" "${SESH_AI_SWELL_RESOURCE_GENERATOR}"
		--force --quiet "${copied_resource_script}"
	RESULT_VARIABLE generator_result
	OUTPUT_VARIABLE generator_output
	ERROR_VARIABLE generator_error
)

string(STRIP "${generator_output}" generator_output)
string(STRIP "${generator_error}" generator_error)

if(NOT generator_result EQUAL 0 OR NOT generator_output STREQUAL "")
	message(FATAL_ERROR
		"Sesh AI extension: swell_resgen.pl did not convert ${SESH_AI_RESOURCE_SCRIPT} "
		"(exit status ${generator_result}).\n"
		"Output: ${generator_output}\n"
		"Errors: ${generator_error}"
	)
endif()

if(NOT EXISTS "${generated_dialog_file}")
	message(FATAL_ERROR
		"Sesh AI extension: swell_resgen.pl reported success but wrote no ${generated_dialog_file}."
	)
endif()

file(READ "${generated_dialog_file}" generated_dialog_contents)

# The generator writes `SWELL_DEFINE_DIALOG_RESOURCE_BEGIN2(<name>,` once per dialog
# it recognised. A plain substring search, because the name is a C identifier and
# has no regular-expression meaning to escape.
string(FIND "${generated_dialog_contents}"
	"SWELL_DEFINE_DIALOG_RESOURCE_BEGIN2(${SESH_AI_EXPECTED_DIALOG_NAME},"
	expected_dialog_position
)

if(expected_dialog_position EQUAL -1)
	message(FATAL_ERROR
		"Sesh AI extension: swell_resgen.pl produced no dialog named "
		"${SESH_AI_EXPECTED_DIALOG_NAME} from ${SESH_AI_RESOURCE_SCRIPT}. The generator skips a "
		"DIALOGEX line it cannot parse without reporting it, so check that line's fields first. "
		"The output was ${generated_dialog_file}."
	)
endif()
