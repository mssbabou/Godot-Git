#include "editor/filesystem_menu.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/theme.hpp>

#include "editor/git_dock.h"

void GitFileSystemMenu::set_dock(GitDock *p_dock) {
	dock = p_dock ? p_dock->get_instance_id() : ObjectID();
}

GitDock *GitFileSystemMenu::_get_dock() const {
	return Object::cast_to<GitDock>(ObjectDB::get_instance(dock));
}

// p_paths: the res:// paths selected in the FileSystem dock (folders end in "/").
void GitFileSystemMenu::_popup_menu(const PackedStringArray &p_paths) {
	GitDock *git_dock = _get_dock();
	if (!git_dock) {
		return;
	}
	const PackedStringArray unstaged = git_dock->get_changed_paths(p_paths, false);
	const PackedStringArray staged = git_dock->get_changed_paths(p_paths, true);
	const Ref<Theme> theme = EditorInterface::get_singleton()->get_editor_theme();

	// One file: the commits that changed it.
	if (p_paths.size() == 1 && !p_paths[0].ends_with("/") && !git_dock->get_repo_path(p_paths[0]).is_empty()) {
		add_context_menu_item("Show History", callable_mp(this, &GitFileSystemMenu::_show_history), theme->get_icon("History", "EditorIcons"));
	}
	// One file: its diff. A folder's changes are too many to show as one.
	if (p_paths.size() == 1 && !p_paths[0].ends_with("/") && (!unstaged.is_empty() || !staged.is_empty())) {
		add_context_menu_item("Show Uncommitted Changes", callable_mp(this, &GitFileSystemMenu::_show_change), theme->get_icon("VCSCommit", "EditorIcons"));
	}
	// One new file: keep it out of git.
	if (p_paths.size() == 1 && !p_paths[0].ends_with("/") && git_dock->can_ignore(git_dock->get_repo_path(p_paths[0]))) {
		add_context_menu_item("Ignore...", callable_mp(this, &GitFileSystemMenu::_ignore), theme->get_icon("Hide", "EditorIcons"));
	}
	// Discard only touches what isn't staged (like the Changes list), so it says so when that's
	// not everything.
	if (!unstaged.is_empty()) {
		add_context_menu_item(staged.is_empty() ? "Discard Uncommitted Changes..." : "Discard Unstaged Changes...", callable_mp(this, &GitFileSystemMenu::_discard), theme->get_icon("UndoRedo", "EditorIcons"));
	}
}

void GitFileSystemMenu::_show_change(const PackedStringArray &p_paths) {
	GitDock *git_dock = _get_dock();
	if (!git_dock) {
		return;
	}
	PackedStringArray paths = git_dock->get_changed_paths(p_paths, false);
	if (paths.is_empty()) {
		paths = git_dock->get_changed_paths(p_paths, true);
	}
	if (!paths.is_empty()) {
		git_dock->show_change(paths[0]);
	}
}

void GitFileSystemMenu::_show_history(const PackedStringArray &p_paths) {
	GitDock *git_dock = _get_dock();
	if (git_dock && !p_paths.is_empty()) {
		git_dock->show_file_history(git_dock->get_repo_path(p_paths[0]));
	}
}

void GitFileSystemMenu::_discard(const PackedStringArray &p_paths) {
	GitDock *git_dock = _get_dock();
	if (git_dock) {
		git_dock->discard_changes(git_dock->get_changed_paths(p_paths, false));
	}
}

void GitFileSystemMenu::_ignore(const PackedStringArray &p_paths) {
	GitDock *git_dock = _get_dock();
	if (git_dock && !p_paths.is_empty()) {
		git_dock->show_ignore(git_dock->get_repo_path(p_paths[0]));
	}
}
