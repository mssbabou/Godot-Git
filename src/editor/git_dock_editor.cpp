// Keeping the rest of the editor in step with git: offering to save before an operation rewrites
// files, reloading open scenes it rewrote, and the status colors in the FileSystem dock.

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/script_editor.hpp>

#include "editor/filesystem_colors.h"
#include "editor/git_colors.h"
#include "editor/git_dock_util.h"
#include "editor/script_marks.h"
#include "editor/ui_text.h"

using namespace godot_git;

// Scenes and scripts open in the editor with unsaved edits. Built-in scripts ("scene.tscn::..")
// are saved with their scene, so they're left out.
static PackedStringArray unsaved_files() {
	PackedStringArray files;
	PackedStringArray candidates = EditorInterface::get_singleton()->get_unsaved_scenes();
	if (ScriptEditor *script_editor = EditorInterface::get_singleton()->get_script_editor()) {
		candidates.append_array(script_editor->get_unsaved_files());
	}
	for (const String &path : candidates) {
		if (!path.is_empty() && !path.contains("::") && !files.has(path)) {
			files.push_back(path);
		}
	}
	return files;
}

// Before a pull or a branch switch: files open with unsaved edits would be out of step with what's
// on disk afterwards, and saving them later would quietly undo what the operation changed. So it
// offers to save them first. Returns true if it asked; p_then runs once answered (not on Cancel).
namespace {

// "Pulling", for the save offer's explanation of p_verb (its button says "Save and Pull").
String doing(const String &p_verb) {
	if (p_verb == "Pull") {
		return "Pulling";
	}
	if (p_verb == "Switch") {
		return "Switching branches";
	}
	if (p_verb == "Stash") {
		return "Stashing";
	}
	if (p_verb == "Restore") {
		return "Restoring a stash";
	}
	if (p_verb == "Abort") {
		return "Aborting";
	}
	return "Continuing";
}

} // namespace

bool GitDock::_ask_to_save(const String &p_verb, const Callable &p_then) {
	if (unsaved_checked) {
		unsaved_checked = false;
		return false;
	}
	const PackedStringArray files = unsaved_files();
	if (files.is_empty()) {
		return false;
	}
	PackedStringArray names;
	for (int i = 0; i < MIN(files.size(), 8); i++) {
		names.push_back(String::utf8("• ") + files[i].trim_prefix("res://"));
	}
	if (files.size() > 8) {
		names.push_back(vformat("...and %d more", files.size() - 8));
	}
	unsaved_then = p_then;
	unsaved_confirm->set_text(vformat("%s unsaved changes in the editor:\n%s\n\n%s can change files on disk. Save first, so the editor doesn't hold an older version: saving it afterwards would undo the change.",
			files.size() == 1 ? String("This file has") : String("These files have"), String("\n").join(names), doing(p_verb)));
	unsaved_confirm->set_ok_button_text(vformat("Save and %s", p_verb));
	unsaved_skip->set_text(vformat("%s Without Saving", p_verb));
	unsaved_confirm->popup_centered();
	return true;
}

void GitDock::_on_unsaved_confirmed() {
	if (ScriptEditor *script_editor = EditorInterface::get_singleton()->get_script_editor()) {
		script_editor->save_all_scripts();
	}
	EditorInterface::get_singleton()->save_all_scenes();
	unsaved_checked = true;
	unsaved_then.call();
}

void GitDock::_on_unsaved_custom_action(const StringName &p_action) {
	if (p_action != StringName("skip")) {
		return;
	}
	unsaved_confirm->hide();
	unsaved_checked = true;
	unsaved_then.call();
}

// Before an operation that may rewrite files (pull, switch, discard): what the open scenes' files
// hold, so _reload_changed_scenes can tell which ones it changed. By content, not modified time,
// which only counts whole seconds.
void GitDock::_remember_open_scenes() {
	open_scene_hashes.clear();
	for (const String &path : EditorInterface::get_singleton()->get_open_scenes()) {
		if (!path.is_empty()) {
			open_scene_hashes[path] = FileAccess::get_md5(path);
		}
	}
}

// After the operation. Godot notices files changed on disk only when the editor gets focus back,
// and an operation started from the panel happens while it has focus: without this, an open scene
// kept showing the old version, and saving it undid what was just pulled. Scripts go through the
// script editor's own check, which reloads them or asks, as for any change made outside Godot.
void GitDock::_reload_changed_scenes() {
	const PackedStringArray unsaved = EditorInterface::get_singleton()->get_unsaved_scenes();
	PackedStringArray kept;
	const Array paths = open_scene_hashes.keys();
	for (int i = 0; i < paths.size(); i++) {
		const String path = paths[i];
		if (!FileAccess::file_exists(path) || FileAccess::get_md5(path) == String(open_scene_hashes[path])) {
			continue;
		}
		if (unsaved.has(path)) {
			kept.push_back(path.get_file());
		} else {
			EditorInterface::get_singleton()->reload_scene_from_path(path);
		}
	}
	open_scene_hashes.clear();
	if (ScriptEditor *script_editor = EditorInterface::get_singleton()->get_script_editor()) {
		script_editor->reload_open_files();
	}
	if (!kept.is_empty()) {
		_set_status(STATUS_WARNING, vformat("%s changed on disk, but %s unsaved edits in the editor, so %s not reloaded. Saving would undo the change; use Scene > Reload Saved Scene to take the new version.", String(", ").join(kept), kept.size() == 1 ? "it has" : "they have", kept.size() == 1 ? "it was" : "they were"));
	}
}

bool GitDock::_is_filesystem_colors_enabled() const {
	return EditorInterface::get_singleton()->get_editor_settings()->get_project_metadata("godot_git", "filesystem_colors", true);
}

// Changed files in the FileSystem dock get their status letter's color; folders holding them get
// the strongest of those colors, softened, so the tree shows where the changes are.
void GitDock::_update_filesystem_colors(const Array &p_status) {
	Dictionary colors;
	Dictionary badges; // The status letter at the row's right edge; a dot for folders.
	if (_is_filesystem_colors_enabled()) {
		Dictionary folder_rank; // Folder -> 1 new, 2 modified, 3 deleted or conflicted.
		for (int i = 0; i < p_status.size(); i++) {
			const Dictionary entry = p_status[i];
			const String worktree = entry["worktree"];
			const String state = worktree.is_empty() ? String(entry["index"]) : worktree;
			const String path = _to_res_path(entry["path"]);
			if (state.is_empty() || !path.begins_with("res://")) {
				continue;
			}
			// Name and letter in the same color, the panel's one color for that status (git_colors.h).
			colors[path] = status_color(state);
			badges[path] = Array::make(status_letter(state), status_color(state));
			const int rank = (state == "new" || state == "untracked") ? 1 : (state == "deleted" || state == "conflicted") ? 3
																														  : 2;
			for (String folder = path.get_base_dir(); folder != "res://" && folder.begins_with("res://"); folder = folder.get_base_dir()) {
				const String key = folder + "/";
				if ((int)folder_rank.get(key, 0) < rank) {
					folder_rank[key] = rank;
				}
			}
		}
		// Folders keep their normal name; a dot in the strongest of their changes' colors says
		// something inside changed. Colored folder names in a second, softer shade made too many
		// shades of each color (maintainer, 2026-09-30).
		const Array folders = folder_rank.keys();
		for (int i = 0; i < folders.size(); i++) {
			const int rank = folder_rank[folders[i]];
			badges[folders[i]] = Array::make(String::utf8("•"), change_color(rank == 1 ? CHANGE_ADDED : rank == 3 ? CHANGE_REMOVED
																												  : CHANGE_MODIFIED));
		}
	}
	filesystem_colors->set_colors(colors, badges);
}

// The files (repository paths) with changes in Staged Changes (p_staged) or Changes at or under
// the FileSystem dock's p_res_paths (folders end in "/"). A file brings its companions along,
// which the FileSystem dock doesn't list (player.gd.uid), like its row in the list does.
PackedStringArray GitDock::get_changed_paths(const PackedStringArray &p_res_paths, bool p_staged) const {
	PackedStringArray result;
	if (repo.is_null() || !repo->is_open()) {
		return result;
	}
	const PackedStringArray &listed = p_staged ? staged_paths : unstaged_paths;
	const String workdir = repo->get_workdir().simplify_path().trim_suffix("/") + "/";
	for (const String &res_path : p_res_paths) {
		const bool folder = res_path.ends_with("/");
		const String absolute = ProjectSettings::get_singleton()->globalize_path(res_path).simplify_path();
		if (!absolute.begins_with(workdir) && absolute != workdir.trim_suffix("/")) {
			continue; // Outside the repository.
		}
		const String path = absolute.substr(workdir.length()).trim_suffix("/");
		for (const String &changed : listed) {
			const bool match = folder ? (path.is_empty() || changed.begins_with(vformat("%s/", path))) : (changed == path || companion_owner(changed) == path);
			if (match && !result.has(changed)) {
				result.push_back(changed);
			}
		}
	}
	return result;
}

// Shows a file's changes in the Diff panel, the unstaged ones if it has both.
void GitDock::show_change(const String &p_path) {
	_show_diff(p_path, !unstaged_paths.has(p_path), true);
}

void GitDock::discard_changes(const PackedStringArray &p_paths) {
	_confirm_discard(p_paths);
}

// Our settings live in Editor Settings (under godot_git/), so they follow you to every project and
// get Godot's revert button. Godot only lists a plugin's settings with "Advanced Settings" ticked
// (gotcha 49), so the ⋮ menu offers each one too.
void GitDock::_register_settings() {
	const Ref<EditorSettings> settings = EditorInterface::get_singleton()->get_editor_settings();
	if (!settings->has_setting(CHANGE_MARKS_SETTING)) {
		settings->set_setting(CHANGE_MARKS_SETTING, true);
	}
	settings->set_initial_value(CHANGE_MARKS_SETTING, true, false);
	Dictionary info;
	info["name"] = CHANGE_MARKS_SETTING;
	info["type"] = Variant::BOOL;
	settings->add_property_info(info);
}

bool GitDock::_is_change_marks_enabled() const {
	return EditorInterface::get_singleton()->get_editor_settings()->get_setting(CHANGE_MARKS_SETTING);
}

// A setting changed in Editor Settings rather than through the ⋮ menu.
void GitDock::_on_editor_settings_changed() {
	script_marks->set_enabled(_is_change_marks_enabled());
}
