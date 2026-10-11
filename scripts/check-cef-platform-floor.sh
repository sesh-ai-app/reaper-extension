#!/usr/bin/env bash
#
# Does the pinned CEF distribution actually run on the platforms we claim?
#
# This is the step that stops the supported-platform floors in CMakeLists.txt
# being aspirational. CEF sets the real minimum on macOS and Linux — it is a
# Chromium build, it is far larger than anything here, and it is not ours to
# recompile. If CEF demands more than we declare, the extension builds, links,
# and then fails to load on exactly the machines the floor existed for. Nothing
# about that failure shows up in a build log; it shows up in a support thread.
#
# So the two numbers are read off the downloaded distribution and compared
# against what CMakeLists.txt declares:
#
#   macOS — LC_BUILD_VERSION's `minos` on the framework binary, via
#     `vtool -show-build`, plus LSMinimumSystemVersion from the framework's
#     Info.plist when it carries one. `minos` is the authoritative number: it is
#     what dyld refuses to load against.
#
#   Linux — the highest versioned glibc symbol libcef.so requires. A shared
#     object asking for GLIBC_2.38 will not resolve on a 2.35 system, whatever
#     the extension beside it was compiled for.
#
# Windows is not checked. CEF's Windows minimum is documented rather than stated
# in the PE headers in any form worth parsing, and `_WIN32_WINNT` on the library
# target is where that floor lives instead.
#
# ---------------------------------------------------------------------------
# What a failure means
#
# That the CEF pin and the declared floor disagree. The fix is one of two things:
# raise the floor in CMakeLists.txt and in the spec, or change
# SESH_AI_CEF_VERSION. It is never to delete this check — the whole point of it
# is that the alternative is finding out from a producer whose REAPER logged a
# load failure.
#
# ---------------------------------------------------------------------------
# Unverified
#
# None of this has run. There is no CEF distribution on any development machine
# here, which is the same reason the CEF arm of CMakeLists.txt is unexercised, so
# the first CEF leg in CI is the first time these commands meet a real framework.
# Written defensively for that reason: a missing distribution, a missing binary,
# or a missing tool is a failure with an explanation, never a pass. A check that
# quietly succeeds is worse than no check, because it is also evidence.
#
#   ./scripts/check-cef-platform-floor.sh

set -o errexit
set -o nounset
set -o pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repository_root}"

# Written for bash 3.2, which is what macOS ships — no `mapfile`, no associative
# arrays, same constraint as the house-style lint.

fail() {
	echo "check-cef-platform-floor: $1" >&2
	shift

	while [ "$#" -gt 0 ]; do
		echo "check-cef-platform-floor:   $1" >&2
		shift
	done

	exit 1
}

# Dotted versions as one comparable integer. Two components are enough for
# everything here — macOS deployment targets are `13.0`, glibc symbols are
# `2.35` — and a third is tolerated rather than relied on.
version_as_number() {
	echo "$1" | awk -F. '{
		major = ($1 == "") ? 0 : $1
		minor = ($2 == "") ? 0 : $2
		patch = ($3 == "") ? 0 : $3
		printf "%d\n", (major * 1000000) + (minor * 1000) + patch
	}'
}

require_tool() {
	if ! command -v "$1" > /dev/null 2>&1; then
		fail \
			"$1 is not on PATH, so CEF's $2 cannot be read." \
			"This check cannot be skipped by being unable to run — if the tool is genuinely" \
			"unavailable on this image, the step needs a different tool, not a bypass."
	fi
}

# ---------------------------------------------------------------------------
# The declared floors, parsed out of CMakeLists.txt rather than repeated here.
#
# Same approach release.yml already takes for `project(VERSION)`. It means the
# `set()` lines in the build system are the single declaration, and this script
# cannot drift from them — if the pattern stops matching, that is a failure, not
# a default.
# ---------------------------------------------------------------------------

declared_macos_floor="$(
	sed -n 's/^set(CMAKE_OSX_DEPLOYMENT_TARGET "\([0-9][0-9.]*\)".*$/\1/p' CMakeLists.txt \
		| head -n 1
)"

declared_glibc_floor="$(
	sed -n 's/^set(SESH_AI_LINUX_GLIBC_FLOOR "\([0-9][0-9.]*\)".*$/\1/p' CMakeLists.txt \
		| head -n 1
)"

# ---------------------------------------------------------------------------
# Where the distribution is.
#
# Globbed rather than constructed, exactly as release.yml stages it: CMakeLists.txt
# owns the CEF version string, it carries '+' characters, and it is pinned in one
# place. Asking the filesystem is harder to get quietly wrong than duplicating the
# rule.
# ---------------------------------------------------------------------------

cef_vendor_directory="third_party/cef"

if [ ! -d "${cef_vendor_directory}" ]; then
	fail \
		"no ${cef_vendor_directory} directory, so no CEF distribution to inspect." \
		"This script belongs on a build leg configured with -DSESH_AI_DOWNLOAD_CEF=ON." \
		"Running it on a CEF-less leg is a workflow mistake rather than a passing check."
fi

cef_root_match_count="$(
	find "${cef_vendor_directory}" -maxdepth 1 -type d -name 'cef_binary_*_minimal' \
		| wc -l \
		| tr -d '[:space:]'
)"

if [ "${cef_root_match_count}" -ne 1 ]; then
	fail \
		"expected exactly one extracted CEF distribution under ${cef_vendor_directory}, found ${cef_root_match_count}." \
		"More than one means two versions are present and the wrong one could be measured;" \
		"none means the download or extraction did not happen."
fi

cef_root="$(
	find "${cef_vendor_directory}" -maxdepth 1 -type d -name 'cef_binary_*_minimal' \
		| head -n 1
)"

echo "check-cef-platform-floor: inspecting ${cef_root}"

case "$(uname -s)" in
	Darwin)
		# -----------------------------------------------------------------
		# macOS: the framework's own minimum, two ways.
		# -----------------------------------------------------------------

		if [ -z "${declared_macos_floor}" ]; then
			fail \
				"could not read CMAKE_OSX_DEPLOYMENT_TARGET out of CMakeLists.txt." \
				"The declaration moved or was reformatted. Fix the pattern in this script —" \
				"do not hard-code the number, which is the drift this avoids."
		fi

		framework_name="Chromium Embedded Framework"
		framework_directory="${cef_root}/Release/${framework_name}.framework"

		if [ ! -d "${framework_directory}" ]; then
			fail \
				"no framework at ${framework_directory}." \
				"CEF's macOS layout changed, or the distribution is for another platform." \
				"Check what Release/ actually contains before changing anything here."
		fi

		framework_binary="${framework_directory}/${framework_name}"

		if [ ! -f "${framework_binary}" ]; then
			fail "no framework binary at ${framework_binary}."
		fi

		require_tool vtool "LC_BUILD_VERSION minos"

		# `vtool -show-build` prints the load commands; minos is one field of
		# LC_BUILD_VERSION. The last field of that line is the version.
		cef_minos="$(
			vtool -show-build "${framework_binary}" \
				| awk '/^[[:space:]]*minos/ { print $2; exit }'
		)"

		if [ -z "${cef_minos}" ]; then
			fail \
				"vtool reported no minos for ${framework_binary}." \
				"Either the binary carries no LC_BUILD_VERSION, or vtool's output format moved." \
				"Run 'vtool -show-build' on it by hand and adjust the parse."
		fi

		echo "check-cef-platform-floor: CEF framework minos ${cef_minos}, declared floor ${declared_macos_floor}"

		if [ "$(version_as_number "${cef_minos}")" -gt "$(version_as_number "${declared_macos_floor}")" ]; then
			fail \
				"CEF requires macOS ${cef_minos}, but this build declares ${declared_macos_floor}." \
				"The extension would link and then fail to load on every macOS below ${cef_minos}." \
				"Fix it by raising CMAKE_OSX_DEPLOYMENT_TARGET in CMakeLists.txt (and the floor in" \
				"the spec and docs), or by changing SESH_AI_CEF_VERSION to a distribution that" \
				"supports ${declared_macos_floor}. Not by removing this check."
		fi

		# LSMinimumSystemVersion, when the framework declares one.
		#
		# Deliberately softer than the minos check, and the distinction matters:
		# a missing Info.plist is a broken distribution and fails, but an absent
		# key is normal metadata variation in a framework bundle — dyld does not
		# read it, and failing on it would be this check breaking for a reason
		# that is not about compatibility at all. Reported either way, so the
		# log says which happened.
		framework_plist="${framework_directory}/Resources/Info.plist"

		if [ ! -f "${framework_plist}" ]; then
			framework_plist="${framework_directory}/Info.plist"
		fi

		if [ ! -f "${framework_plist}" ]; then
			fail \
				"no Info.plist in ${framework_directory}, at Resources/Info.plist or at the root." \
				"A framework bundle without one is not a distribution this build understands."
		fi

		require_tool plutil "LSMinimumSystemVersion"

		# `plutil -extract` rather than `defaults read`, which needs an absolute
		# path and reports a relative one as a nonexistent preference domain —
		# and rather than PlistBuddy, which lives inside /usr/libexec rather than
		# on PATH. `-extract` exits non-zero when the key is absent, which is the
		# distinction the branch below depends on.
		if cef_minimum_system_version="$(
			plutil -extract LSMinimumSystemVersion raw -o - "${framework_plist}" 2> /dev/null
		)"; then
			echo "check-cef-platform-floor: CEF framework LSMinimumSystemVersion ${cef_minimum_system_version}"

			if [ "$(version_as_number "${cef_minimum_system_version}")" -gt "$(version_as_number "${declared_macos_floor}")" ]; then
				fail \
					"CEF's framework declares LSMinimumSystemVersion ${cef_minimum_system_version}, but this build declares ${declared_macos_floor}." \
					"Raise CMAKE_OSX_DEPLOYMENT_TARGET in CMakeLists.txt, or change SESH_AI_CEF_VERSION." \
					"Not by removing this check."
			fi
		else
			echo "check-cef-platform-floor: the framework Info.plist declares no LSMinimumSystemVersion."
			echo "check-cef-platform-floor: not a failure — minos above is the number dyld enforces."
		fi

		echo "check-cef-platform-floor: CEF runs on macOS ${declared_macos_floor} and later. Floor holds."
		;;

	Linux)
		# -----------------------------------------------------------------
		# Linux: the highest versioned glibc symbol libcef.so asks for.
		# -----------------------------------------------------------------

		if [ -z "${declared_glibc_floor}" ]; then
			fail \
				"could not read SESH_AI_LINUX_GLIBC_FLOOR out of CMakeLists.txt." \
				"The declaration moved or was reformatted. Fix the pattern in this script —" \
				"do not hard-code the number, which is the drift this avoids."
		fi

		cef_library="${cef_root}/Release/libcef.so"

		if [ ! -f "${cef_library}" ]; then
			fail \
				"no libcef.so at ${cef_library}." \
				"CEF's Linux layout changed, or the distribution is for another platform." \
				"Check what Release/ actually contains before changing anything here."
		fi

		require_tool objdump "glibc symbol versions"

		# `objdump -T` lists the dynamic symbol table, where an imported
		# versioned symbol carries its GLIBC_x.y tag. The highest one is the
		# effective requirement: a system older than that cannot resolve it.
		# `sort -uV` is a version sort, so GLIBC_2.9 does not outrank
		# GLIBC_2.35 the way a lexical sort would.
		cef_glibc_requirement="$(
			objdump -T "${cef_library}" \
				| grep -o 'GLIBC_[0-9][0-9.]*' \
				| sed 's/^GLIBC_//' \
				| sed 's/\.$//' \
				| sort -uV \
				| tail -n 1
		)"

		if [ -z "${cef_glibc_requirement}" ]; then
			fail \
				"objdump found no versioned GLIBC symbols in ${cef_library}." \
				"A Chromium build links glibc, so finding none means the parse is wrong rather" \
				"than the requirement being absent. Run 'objdump -T' on it by hand."
		fi

		echo "check-cef-platform-floor: CEF requires glibc ${cef_glibc_requirement}, declared floor ${declared_glibc_floor}"

		if [ "$(version_as_number "${cef_glibc_requirement}")" -gt "$(version_as_number "${declared_glibc_floor}")" ]; then
			fail \
				"CEF requires glibc ${cef_glibc_requirement}, but this build declares ${declared_glibc_floor}." \
				"The extension would link here and then fail to load on Ubuntu 22.04 and Debian 12." \
				"Fix it by raising SESH_AI_LINUX_GLIBC_FLOOR in CMakeLists.txt — and the build image" \
				"with it, since a glibc floor is whatever the linking machine provides — or by" \
				"changing SESH_AI_CEF_VERSION. Not by removing this check."
		fi

		# The binary this leg produced, measured the same way. Worth having
		# because the two can diverge: CEF could be within the floor while the
		# runner's own toolchain linked the extension against something newer,
		# which is precisely the "compatibility tracks the image" failure the
		# floors exist to stop. Skipped rather than failed when the library was
		# not built, since the CEF-less legs do not produce one and this script
		# does not run on them anyway.
		extension_library="$(
			find build/bin -type f -name 'reaper_sesh_ai.so' 2> /dev/null \
				| head -n 1
		)"

		if [ -n "${extension_library}" ]; then
			extension_glibc_requirement="$(
				objdump -T "${extension_library}" \
					| grep -o 'GLIBC_[0-9][0-9.]*' \
					| sed 's/^GLIBC_//' \
					| sed 's/\.$//' \
					| sort -uV \
					| tail -n 1
			)"

			echo "check-cef-platform-floor: the extension itself requires glibc ${extension_glibc_requirement:-none}"

			if [ -n "${extension_glibc_requirement}" ] \
				&& [ "$(version_as_number "${extension_glibc_requirement}")" -gt "$(version_as_number "${declared_glibc_floor}")" ]; then
				fail \
					"${extension_library} requires glibc ${extension_glibc_requirement}, above the declared floor of ${declared_glibc_floor}." \
					"CEF is not the problem here — the build machine is. This is the runner image" \
					"determining the compatibility floor, which is the thing the pinned labels in" \
					".github/workflows/ are supposed to prevent. Check which image this leg ran on."
			fi
		else
			echo "check-cef-platform-floor: no reaper_sesh_ai.so under build/bin, so only CEF was measured."
		fi

		echo "check-cef-platform-floor: CEF runs on glibc ${declared_glibc_floor} and later. Floor holds."
		;;

	*)
		fail \
			"no floor check for $(uname -s)." \
			"macOS and Linux are the platforms where CEF states a minimum this script can read." \
			"Windows' floor is _WIN32_WINNT on the library target. Reaching this case means the" \
			"workflow ran the step on a platform it was not meant for."
		;;
esac
