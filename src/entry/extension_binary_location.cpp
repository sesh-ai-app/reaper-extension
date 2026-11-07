// The platform half of `entry/extension_binary_location.h` (task 25.2, requirement
// 15.1).
//
// Two questions, each answered by one platform call on each side of `_WIN32`. Neither
// is in the REAPER C API, so requirement 22.3 does not constrain when they run, and
// neither needs the SDK — this file does not include it.
//
// UTF-8 at the boundary on Windows, because the rest of the extension holds paths as
// `std::string` and CEF is handed UTF-8. The narrow `A` functions would use the
// machine's ANSI code page instead, and a producer whose user name is not ASCII would
// get a path that names nothing.
//
// Not `<filesystem>`. `std::filesystem::u8path` is the portable spelling of "this
// string is UTF-8", and it is deprecated in C++20 — which is what this target is
// compiled as, because CEF's headers need it. Two system calls cost less than a
// deprecation warning on every build.

#include <entry/extension_binary_location.h>

#include <cstddef>
#include <string>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#else
	#include <dlfcn.h>
	#include <sys/stat.h>
#endif

namespace {

#ifdef _WIN32

	std::string utf8_from_wide(const std::wstring& wide)
	{
		if (wide.empty()) {
			return {};
		}

		const int wide_length = static_cast<int>(wide.size());
		const int utf8_length =
			WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, nullptr, 0, nullptr, nullptr);

		if (utf8_length <= 0) {
			return {};
		}

		std::string utf8(static_cast<std::size_t>(utf8_length), '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, &utf8[0], utf8_length, nullptr, nullptr);

		return utf8;
	}

	std::wstring wide_from_utf8(const std::string& utf8)
	{
		if (utf8.empty()) {
			return {};
		}

		const int utf8_length = static_cast<int>(utf8.size());
		const int wide_length =
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), utf8_length, nullptr, 0);

		if (wide_length <= 0) {
			return {};
		}

		std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), utf8_length, &wide[0], wide_length);

		return wide;
	}

#endif

}

namespace sesh_ai::entry {

	std::string locate_extension_binary(void* module_instance)
	{
#ifdef _WIN32
		// A null module is REAPER's executable to `GetModuleFileNameW`, which would put
		// the panel's assets beside reaper.exe. Empty is the honest answer instead.
		if (module_instance == nullptr) {
			return {};
		}

		// `MAX_PATH` first, doubling while the result fills the buffer, which is how
		// `GetModuleFileNameW` reports truncation. Bounded, because a path that is still
		// truncated at 32768 characters is past what Windows itself accepts.
		std::wstring path(MAX_PATH, L'\0');

		while (path.size() <= 32768) {
			const DWORD length = GetModuleFileNameW(
				static_cast<HMODULE>(module_instance),
				&path[0],
				static_cast<DWORD>(path.size())
			);

			if (length == 0) {
				return {};
			}

			if (length < path.size()) {
				path.resize(length);

				return utf8_from_wide(path);
			}

			path.resize(path.size() * 2);
		}

		return {};
#else
		static_cast<void>(module_instance);

		// Any address inside this library identifies it. A data address rather than a
		// function's, because converting a function pointer to `void*` is only
		// conditionally supported, and this way there is nothing to convert.
		static const char address_inside_this_library = 0;

		Dl_info library_information{};

		if (dladdr(&address_inside_this_library, &library_information) == 0
			|| library_information.dli_fname == nullptr) {
			return {};
		}

		return library_information.dli_fname;
#endif
	}

	bool regular_file_exists(const std::string& file_path)
	{
		if (file_path.empty()) {
			return false;
		}

#ifdef _WIN32
		const std::wstring wide_path = wide_from_utf8(file_path);

		if (wide_path.empty()) {
			return false;
		}

		const DWORD attributes = GetFileAttributesW(wide_path.c_str());

		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
		struct stat file_status{};

		return stat(file_path.c_str(), &file_status) == 0 && S_ISREG(file_status.st_mode);
#endif
	}

}
