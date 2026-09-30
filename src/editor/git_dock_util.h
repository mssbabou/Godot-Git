#pragma once

// Small helpers shared by GitDock's source files (not part of its interface).

#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace godot_git {

// File rows are a single cell (icon, name, folder, status letter, hover buttons) so they highlight as one unit.
enum FileColumn {
	COLUMN_NAME,
	FILE_COLUMN_COUNT,
};

// Our settings in Editor Settings (see _register_settings): all on one page, Godot Git > Settings,
// named like their ⋮ menu items (one page per section would be three pages of one checkbox each).
// Plain strings: no global String objects (gotcha 3).
inline constexpr const char *CHANGE_MARKS_SETTING = "godot_git/settings/mark_changed_lines_in_scripts";
inline constexpr const char *FILESYSTEM_COLORS_SETTING = "godot_git/settings/color_changed_files_in_filesystem";
inline constexpr const char *AUTO_FETCH_SETTING = "godot_git/settings/fetch_automatically";

// What a History row is: "commit", "placeholder" (until the commit is expanded), "note", "file"
// or "more" (Load More Commits).
inline String row_kind(const TreeItem *p_item) {
	return p_item ? String(p_item->get_meta("git_row", String())) : String();
}

// The file a Godot companion file belongs to ("player.gd" for "player.gd.uid", "coin.png" for
// "coin.png.import"), or "" if p_path isn't one.
inline String companion_owner(const String &p_path) {
	for (const char *suffix : { ".uid", ".import" }) {
		if (p_path.ends_with(suffix)) {
			const String owner = p_path.trim_suffix(suffix);
			return owner.get_file().contains(".") ? owner : String();
		}
	}
	return String();
}

// Godot's companion files in p_paths whose file is in p_paths too, by file: {"player.gd":
// ["player.gd.uid"]}. Those companions get no row of their own; their file's row stands for them.
inline Dictionary grouped_companions(const PackedStringArray &p_paths) {
	HashSet<String> listed;
	for (const String &path : p_paths) {
		listed.insert(path);
	}
	Dictionary companions;
	for (const String &path : p_paths) {
		const String owner = companion_owner(path);
		if (!owner.is_empty() && listed.has(owner)) {
			PackedStringArray of = companions.get(owner, PackedStringArray());
			of.push_back(path);
			companions[owner] = of;
		}
	}
	return companions;
}

// Whether p_path is shown on its file's row (see grouped_companions).
inline bool is_grouped(const Dictionary &p_companions, const String &p_path) {
	const String owner = companion_owner(p_path);
	return !owner.is_empty() && p_companions.has(owner);
}

} // namespace godot_git
