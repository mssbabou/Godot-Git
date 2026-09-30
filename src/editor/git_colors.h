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
// editor's marks). From the editor theme's success / warning / error colors: green a quarter of
// the way to the text color (full strength reads too loud as whole file names, maintainer
// 2026-09-29), red a tenth, and yellow made more saturated than the theme's (maintainer,
// 2026-09-30: muddy). One shade everywhere is the point (2026-09-30).
// Warnings and errors in the status strip keep the theme's own colors: they're alerts, not changes.
// The +/− line counts (section headers, the Diff panel) and the Diff panel's line tints use the
// theme's full colors (see GitDiffDock::row_tint).
inline Color change_color(ChangeColor p_kind) {
	const Ref<Theme> theme = EditorInterface::get_singleton()->get_editor_theme();
	const char *name = p_kind == CHANGE_ADDED ? "success_color" : (p_kind == CHANGE_REMOVED ? "error_color" : "warning_color");
	Color color = theme->get_color(name, "Editor");
	// Opaque: the Tree's text color is 75% opaque, and blending toward it made every status color
	// a little see-through, duller still over the dark background.
	Color text = theme->get_color("font_color", "Tree");
	text.a = 1;
	if (p_kind == CHANGE_MODIFIED) {
		// The theme's warning color is khaki (#d4c79e in the 4.7 dark theme, 25% saturation):
		// "muddy" (maintainer, 2026-09-30). Its hue, with a real yellow's saturation, and bright
		// enough on a dark theme.
		const bool dark = text.get_luminance() > 0.5;
		color.set_hsv(color.get_h(), MAX(color.get_s(), 0.62f), dark ? MAX(color.get_v(), 0.93f) : color.get_v());
		return color;
	}
	return color.lerp(text, p_kind == CHANGE_ADDED ? 0.25f : 0.1f);
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
