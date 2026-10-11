#!/usr/bin/env bash
#
# House-style check over the C++ sources under src/ and tests/.
#
# This is the C++ half of requirement 21.5. It is deliberately *not* clang-format,
# and that is the most important thing to know about this file.
#
# clang-format was tried first. A configuration matching the house style — tabs,
# indented namespaces, block-indented call arguments — still disagreed with 142 of
# the 146 files in this tree over roughly 37,000 lines. The disagreement is not
# fixable by tuning: this codebase hand-wraps parameter lists one per line and
# breaks template argument lists where they read best, and clang-format re-flows
# both from scratch rather than preserving an author's break. Gating CI on
# `clang-format --dry-run -Werror` would therefore demand a wholesale reformat of
# every file, which is a decision about how the code should look rather than
# something a CI task gets to do as a side effect. No `.clang-format` is committed
# either, because a config nobody enforces is worse than none — the first
# format-on-save would rewrite a file for no reason.
#
# So what is checked here is the part of the style that is mechanical, that the
# whole tree already satisfies, and that a reviewer should never have to spend
# attention on:
#
#   1. Indentation starts with a tab. The code-style steering says tabs for every
#      language here. Spaces *after* the tabs are alignment rather than
#      indentation and are left alone — see the note at the check itself.
#   2. No trailing whitespace. Also a code-style steering rule.
#   3. Exactly one newline at end of file, and no blank line before it.
#   4. No carriage returns. The tree is LF throughout, and a CRLF file committed
#      from Windows would show up as a whole-file diff.
#
# Every violation is reported before the script exits, so one run names everything
# that is wrong rather than revealing one problem per run.
#
# Written for bash 3.2, which is what macOS ships. No `mapfile`, no associative
# arrays — the Linux CI runner has bash 5 but a contributor on macOS should be able
# to run exactly what CI runs.
#
#   ./scripts/lint-cpp-house-style.sh

set -o errexit
set -o nounset
set -o pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repository_root}"

# tests/ as well as src/. The suite is two thirds of the C++ in this repository and
# there is no reason it should be held to a looser standard than the sources.
cpp_source_file_list="$(find src tests -type f \( -name '*.cpp' -o -name '*.h' \) | sort)"

if [ -z "${cpp_source_file_list}" ]; then
	echo "lint-cpp-house-style: no C++ sources found under src/ or tests/." >&2
	echo "lint-cpp-house-style: this runs from the repository root — check the layout." >&2
	exit 1
fi

inspected_file_count=0
violation_count=0
violation_report=""

record_violation() {
	violation_count=$((violation_count + 1))
	violation_report="${violation_report}  $1
"
}

while IFS= read -r cpp_source_file; do
	inspected_file_count=$((inspected_file_count + 1))

	# Indentation must start with a tab. A space anywhere later in the leading
	# whitespace is fine, and the rule is deliberately that narrow: this codebase
	# indents with tabs and then *aligns* with spaces, which is a different job. Five
	# files align continued string literals under the preceding `+` that way —
	# src/daw/tools/item_tools.h and its neighbours — and that is readability, not a
	# violation. What is forbidden is a space before the first tab, or indentation made
	# of spaces alone, because either one indents differently in every editor.
	space_indented_lines="$(grep -n '^ ' "${cpp_source_file}" | cut -d: -f1 | tr '\n' ' ' || true)"
	if [ -n "${space_indented_lines}" ]; then
		record_violation "${cpp_source_file}: indentation starts with a space on line(s) ${space_indented_lines% } — it must start with a tab"
	fi

	trailing_whitespace_lines="$(grep -n '[[:blank:]]$' "${cpp_source_file}" | cut -d: -f1 | tr '\n' ' ' || true)"
	if [ -n "${trailing_whitespace_lines}" ]; then
		record_violation "${cpp_source_file}: trailing whitespace on line(s) ${trailing_whitespace_lines% }"
	fi

	if grep -q "$(printf '\r')" "${cpp_source_file}"; then
		record_violation "${cpp_source_file}: contains carriage returns — this tree is LF throughout"
	fi

	# A missing final newline and a blank line at end of file are the same two bytes
	# looked at two ways, so they are read as hex rather than through a command
	# substitution, which strips the very newlines being measured.
	final_two_bytes="$(tail -c 2 "${cpp_source_file}" | od -An -tx1 | tr -d ' \n')"
	case "${final_two_bytes}" in
		*0a0a)
			record_violation "${cpp_source_file}: blank line at end of file"
			;;
		*0a)
			;;
		*)
			record_violation "${cpp_source_file}: no newline at end of file"
			;;
	esac
done <<EOF
${cpp_source_file_list}
EOF

if [ "${violation_count}" -gt 0 ]; then
	echo "lint-cpp-house-style: ${violation_count} violation(s) across ${inspected_file_count} file(s)." >&2
	printf '%s' "${violation_report}" >&2
	exit 1
fi

echo "lint-cpp-house-style: ${inspected_file_count} C++ file(s) conform."
