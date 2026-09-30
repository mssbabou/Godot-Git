#pragma once

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace godot_git {

enum ChangeColor {
	CHANGE_ADDED,
	CHANGE_MODIFIED,
	CHANGE_REMOVED,
};

// The panel's change colors: one green, one yellow, one red, wherever a change is shown (status
// letters in the Git and FileSystem docks, changed file names, the dots on folders, the script
// editor's marks, +/− line counts). The editor theme's success / warning / error colors a quarter
// of the way to the text color: at full strength they read too loud as whole file names
// (maintainer, 2026-09-29), and one shade everywhere is the point (maintainer, 2026-09-30).
// Warnings and errors in the status strip keep the theme's own colors: they're alerts, not changes.
// The Diff panel's line tints are made from the full colors (see GitDiffDock::row_tint).
inline Color change_color(ChangeColor p_kind) {
	const Ref<Theme> theme = EditorInterface::get_singleton()->get_editor_theme();
	const char *name = p_kind == CHANGE_ADDED ? "success_color" : (p_kind == CHANGE_REMOVED ? "error_color" : "warning_color");
	return theme->get_color(name, "Editor").lerp(theme->get_color("font_color", "Tree"), 0.25);
}

// By status, as GitRepository::get_status names it ("new", "modified", "deleted", ...).
inline Color status_color(const String &p_state) {
	if (p_state == "new" || p_state == "untracked") {
		return change_color(CHANGE_ADDED);
	}
	if (p_state == "deleted" || p_state == "conflicted") {
		return change_color(CHANGE_REMOVED);
	}
	return change_color(CHANGE_MODIFIED);
}

} // namespace godot_git
