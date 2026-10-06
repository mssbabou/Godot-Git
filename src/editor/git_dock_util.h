#pragma once

// Small helpers shared by GitDock's source files (not part of its interface).

#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

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
inline constexpr const char *ASK_PULL_MERGE_SETTING = "godot_git/settings/ask_before_a_pull_stops_at_conflicts";
inline constexpr const char *AVATARS_SETTING = "godot_git/settings/show_profile_pictures_from_github";
// How many settings there are (SETTINGS in git_dock_editor.cpp, settings_checks).
inline constexpr int SETTING_COUNT = 5;

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

// What a companion-only change means, for the row's tooltip. "" if p_path isn't a companion.
inline String companion_note(const String &p_path, const String &p_state) {
	const String file = companion_owner(p_path).get_file();
	if (file.is_empty()) {
		return String();
	}
	const bool added = p_state == "new" || p_state == "untracked";
	if (p_path.ends_with(".import")) {
		if (added) {
			return vformat("The import settings of %s are new; the file itself didn't change.", file);
		}
		if (p_state == "deleted") {
			return vformat("The import settings of %s were deleted; the file itself didn't change. Godot writes new ones with the default settings.", file);
		}
		return vformat("Only the import settings of %s changed, not the file itself.", file);
	}
	if (added) {
		return vformat("The uid file of %s is new. Commit it, so %s has the same uid on every machine.", file, file);
	}
	if (p_state == "deleted") {
		return vformat("The uid file of %s was deleted. Godot gives it a new uid, and scenes that refer to it by the old one fall back to its path, with a warning, until they're saved again.", file);
	}
	return vformat("The uid of %s changed. Scenes that refer to it by the old uid fall back to its path, with a warning, until they're saved again. If Godot made a new uid because the .uid file went missing, discard this change to keep the old one.", file);
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
