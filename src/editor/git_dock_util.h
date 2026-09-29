#pragma once

// Small helpers shared by GitDock's source files (not part of its interface).

#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace godot_git {

// File rows are a single cell (icon, name, folder, status letter, hover buttons) so they highlight as one unit.
enum FileColumn {
	COLUMN_NAME,
	FILE_COLUMN_COUNT,
};

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

} // namespace godot_git
