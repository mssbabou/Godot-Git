#pragma once

#include <filesystem>

namespace godot_git {

// The addon's godot_git.gdextension, found next to this library; empty when it's gone.
std::filesystem::path addon_manifest_path();

// Keeps this library mapped after Godot unloads it (see register_types.cpp).
void keep_library_loaded();

} // namespace godot_git
