#include "addon_files.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <system_error>

namespace godot_git {

namespace {

std::filesystem::path own_library_path() {
#ifdef _WIN32
	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&own_library_path), &module);
	wchar_t path[MAX_PATH * 4];
	DWORD length = GetModuleFileNameW(module, path, sizeof(path) / sizeof(path[0]));
	return std::filesystem::path(std::wstring(path, length));
#else
	Dl_info info = {};
	dladdr(reinterpret_cast<void *>(&own_library_path), &info);
	return info.dli_fname ? std::filesystem::path(info.dli_fname) : std::filesystem::path();
#endif
}

} // namespace

std::filesystem::path addon_manifest_path() {
	std::error_code ec;
	for (std::filesystem::path dir = own_library_path().parent_path(); !dir.empty() && dir != dir.parent_path(); dir = dir.parent_path()) {
		const std::filesystem::path manifest = dir / "godot_git.gdextension";
		if (std::filesystem::exists(manifest, ec)) {
			return manifest;
		}
		if (dir.filename() == "addons") {
			break;
		}
	}
	return std::filesystem::path();
}

void keep_library_loaded() {
#ifdef _WIN32
	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&own_library_path), &module);
#else
	dlopen(own_library_path().c_str(), RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
#endif
}

} // namespace godot_git
