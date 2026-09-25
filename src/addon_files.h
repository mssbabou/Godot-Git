#pragma once

#include <godot_cpp/variant/string.hpp>

namespace godot_git {

// The addon's godot_git.gdextension (absolute), found next to this library; empty when it's gone.
godot::String addon_manifest_path();

// Keeps this library mapped after Godot unloads it (see register_types.cpp).
void keep_library_loaded();

} // namespace godot_git
