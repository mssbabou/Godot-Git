#include "addon_files.h"

#include <godot_cpp/classes/file_access.hpp>

// After godot-cpp: windows.h defines macros (CONNECT_DEFERRED, ...) that break its headers.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace godot;

namespace godot_git {

namespace {

// Not std::filesystem: macOS only has it from 10.15, and we support 10.13.
String own_library_path() {
#ifdef _WIN32
	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&own_library_path), &module);
	wchar_t path[MAX_PATH * 4];
	const DWORD length = GetModuleFileNameW(module, path, sizeof(path) / sizeof(path[0]));
	return String::utf16(reinterpret_cast<const char16_t *>(path), length).replace("\\", "/");
#else
	Dl_info info = {};
	dladdr(reinterpret_cast<void *>(&own_library_path), &info);
	return info.dli_fname ? String::utf8(info.dli_fname) : String();
#endif
}

} // namespace

String addon_manifest_path() {
	String dir = own_library_path().get_base_dir();
	while (!dir.is_empty() && dir != dir.get_base_dir()) {
		const String manifest = dir.path_join("godot_git.gdextension");
		if (FileAccess::file_exists(manifest)) {
			return manifest;
		}
		if (dir.get_file() == "addons") {
			break;
		}
		dir = dir.get_base_dir();
	}
	return String();
}

void keep_library_loaded() {
#ifdef _WIN32
	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&own_library_path), &module);
#else
	dlopen(own_library_path().utf8().get_data(), RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
#endif
}

} // namespace godot_git
