#pragma once

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace godot_git {

// Whether the Diff panel compares p_path setting by setting: a `.import` file (Godot's ConfigFile
// text: sections of key=value) or a `.uid` file (one uid).
bool is_settings_path(const String &p_path);

// The settings that differ between two versions of such a file, in the new file's order, then
// the removed ones: [{ "section", "key", "old", "new" }, ...], "old" or "new" missing on the
// side that doesn't have the key. Values are as written in the file (strings keep their quotes).
// A `.uid` file is one setting, section "" and key "uid".
Array settings_changes(const String &p_path, const String &p_old_text, const String &p_new_text);

// The value of one key as written in the file, or "" (the importer's name: "remap", "importer").
String settings_value(const String &p_text, const String &p_section, const String &p_key);

} // namespace godot_git
