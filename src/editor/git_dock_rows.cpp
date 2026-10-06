// What the list rows do: hover buttons (drawn by _draw_file_row, clicked here), clicks,
// double-clicks, selection and the right-click menus.

#include "editor/git_dock.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include "editor/git_diff_dock.h"
#include "editor/git_dock_util.h"
#include "editor/ui_text.h"

using namespace godot_git;

GitDock::FilePane *GitDock::_pane_for_tree(Object *p_tree) {
	if (p_tree == staged_pane.tree) {
		return &staged_pane;
	}
	if (p_tree == changes_pane.tree) {
		return &changes_pane;
	}
	return nullptr;
}

// p_companions: with the companion files shown on the rows (for stage, unstage and discard,
// which act on a file together with them), not for opening or copying paths.
PackedStringArray GitDock::_selected_paths(Tree *p_tree, bool p_companions) const {
	PackedStringArray paths;
	for (TreeItem *item = p_tree->get_next_selected(nullptr); item; item = p_tree->get_next_selected(item)) {
		if (item->get_metadata(COLUMN_NAME).get_type() == Variant::STRING) {
			paths.append_array(p_companions ? _row_paths(item) : PackedStringArray({ item->get_metadata(COLUMN_NAME) }));
		}
	}
	return paths;
}

// A file row's path and its companions' (see _fill_file_pane).
PackedStringArray GitDock::_row_paths(TreeItem *p_item) const {
	PackedStringArray paths;
	paths.push_back(p_item->get_metadata(COLUMN_NAME));
	paths.append_array(p_item->get_meta("git_companions", PackedStringArray()));
	return paths;
}

// Which of a row's files Pull waits for (see GitRepository::get_pull_blockers).
PackedStringArray GitDock::_blocking_paths(TreeItem *p_item) const {
	PackedStringArray blocking;
	for (const String &path : _row_paths(p_item)) {
		if (pull_blockers.has(path) || pull_conflict_paths.has(path)) {
			blocking.push_back(path);
		}
	}
	return blocking;
}

// A list's row buttons, left to right: [{id, icon, tooltip}].
Array GitDock::_row_button_list(const FilePane &p_pane) const {
	auto button = [&](int p_id, const char *p_icon, const char *p_tooltip) {
		Dictionary entry;
		entry["id"] = p_id;
		entry["icon"] = _icon(p_icon);
		entry["tooltip"] = p_tooltip;
		return entry;
	};
	if (p_pane.staged) {
		return Array::make(button(BUTTON_UNSTAGE, "GitMinus", "Unstage"));
	}
	return Array::make(button(BUTTON_DISCARD, "UndoRedo", "Discard changes"), button(BUTTON_STAGE, "Add", "Stage"));
}

// Row buttons only show on the row under the mouse, to keep the list calm (drawn by _draw_file_row).
void GitDock::_set_hovered(FilePane &p_pane, TreeItem *p_item) {
	TreeItem *previous = Object::cast_to<TreeItem>(ObjectDB::get_instance(p_pane.hovered_item));
	if (previous == p_item) {
		return;
	}
	if (previous) {
		previous->remove_meta("git_button_rects");
		previous->set_tooltip_text(COLUMN_NAME, previous->get_meta("git_tooltip", String()));
	}
	p_pane.hovered_item = 0;
	p_pane.hovered_button = -1;
	if (p_item && p_item->get_metadata(COLUMN_NAME).get_type() == Variant::STRING) {
		p_pane.hovered_item = p_item->get_instance_id();
	}
	p_pane.tree->queue_redraw();
}

// Which of the hovered row's buttons is at p_position: its {id, icon, tooltip, rect}, or empty.
Dictionary GitDock::_row_button_at(const FilePane &p_pane, const Vector2 &p_position) const {
	TreeItem *item = Object::cast_to<TreeItem>(ObjectDB::get_instance(p_pane.hovered_item));
	if (!item) {
		return Dictionary();
	}
	const Array rects = item->get_meta("git_button_rects", Array());
	for (int i = 0; i < rects.size(); i++) {
		const Dictionary button = rects[i];
		if (Rect2(button["rect"]).has_point(p_position)) {
			return button;
		}
	}
	return Dictionary();
}

void GitDock::_on_tree_gui_input(const Ref<InputEvent> &p_event, Object *p_tree) {
	FilePane *pane = _pane_for_tree(p_tree);
	if (!pane) {
		return;
	}
	Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		_set_hovered(*pane, pane->tree->get_item_at_position(motion->get_position()));
		const Dictionary button = _row_button_at(*pane, motion->get_position());
		const int id = button.get("id", -1);
		if (id != pane->hovered_button) {
			pane->hovered_button = id;
			pane->tree->queue_redraw();
			// The Tree shows the row's tooltip; over a button, that's the button's.
			TreeItem *item = Object::cast_to<TreeItem>(ObjectDB::get_instance(pane->hovered_item));
			if (item) {
				item->set_tooltip_text(COLUMN_NAME, id >= 0 ? String(button["tooltip"]) : String(item->get_meta("git_tooltip", String())));
			}
		}
		return;
	}
	// A click on a row button: taken before the Tree sees it (Godot emits gui_input first for
	// this), so it doesn't also select the row. The action runs deferred, outside this event.
	Ref<InputEventMouseButton> click = p_event;
	if (click.is_valid() && click->get_button_index() == MOUSE_BUTTON_LEFT) {
		const Dictionary button = _row_button_at(*pane, click->get_position());
		if (!button.is_empty()) {
			pane->tree->accept_event();
			if (click->is_pressed()) {
				callable_mp(this, &GitDock::_click_row_button).call_deferred(pane->hovered_item, (int)button["id"]);
			}
		}
	}
}

void GitDock::_on_tree_mouse_exited(Object *p_tree) {
	FilePane *pane = _pane_for_tree(p_tree);
	if (pane) {
		_set_hovered(*pane, nullptr);
	}
}

// A click on one of the buttons _draw_file_row draws (by the row's instance id: it runs deferred).
void GitDock::_click_row_button(uint64_t p_item, int p_id) {
	TreeItem *item = Object::cast_to<TreeItem>(ObjectDB::get_instance(p_item));
	if (!item) {
		return;
	}
	const PackedStringArray paths = _row_paths(item);
	switch (p_id) {
		case BUTTON_STAGE: {
			_stage_paths(paths, true);
		} break;
		case BUTTON_UNSTAGE: {
			_stage_paths(paths, false);
		} break;
		case BUTTON_DISCARD: {
			_confirm_discard(paths);
		} break;
	}
}

void GitDock::_on_file_activated(Object *p_tree) {
	Tree *tree = Object::cast_to<Tree>(p_tree);
	TreeItem *item = tree ? tree->get_selected() : nullptr;
	if (item) {
		_open_path(item->get_metadata(COLUMN_NAME));
	}
}

// A click on a commit or stash opens or closes it (_on_tree_mouse_selected). A second click soon
// after is a double-click to the Tree, which then reports item_activated instead, so without
// this, closing a commit right after opening it (or clicking again when nothing seemed to happen)
// took another click.
void GitDock::_on_row_activated(Object *p_tree) {
	Tree *tree = Object::cast_to<Tree>(p_tree);
	TreeItem *item = tree ? tree->get_selected() : nullptr;
	const String row = row_kind(item);
	if (row == "commit" || row == "stash") {
		item->set_collapsed(!item->is_collapsed());
	}
}

void GitDock::_on_tree_mouse_selected(const Vector2 &p_position, int p_mouse_button, Object *p_tree) {
	if (p_mouse_button == MOUSE_BUTTON_LEFT && p_tree == stashes_tree) {
		TreeItem *item = stashes_tree->get_item_at_position(p_position);
		const String row = row_kind(item);
		if (row == "stash" && stashes_tree->get_button_id_at_position(p_position) < 0) {
			item->set_collapsed(!item->is_collapsed()); // Like a commit; not when a button was hit.
		} else if (row == "file") {
			_show_commit_diff(item->get_meta("git_hash"), item->get_meta("git_path"), true, true);
		}
		return;
	}
	if (p_mouse_button == MOUSE_BUTTON_LEFT && p_tree == history_tree) {
		TreeItem *item = history_tree->get_item_at_position(p_position);
		const String row = row_kind(item);
		if (row == "commit") {
			item->set_collapsed(!item->is_collapsed()); // Like VS Code: a click opens or closes it.
		} else if (row == "file") {
			_show_commit_diff(item->get_meta("git_hash"), item->get_meta("git_path"), true);
		} else if (row == "more") {
			// Deferred for the same reason as filling a commit: no clearing the tree mid-click.
			callable_mp(this, &GitDock::_load_more_commits).call_deferred();
		}
		return;
	}
	if (p_mouse_button == MOUSE_BUTTON_LEFT) {
		// A click on a file brings up the Diff panel (the selection already put the file in it).
		const FilePane *pane = _pane_for_tree(p_tree);
		TreeItem *item = pane ? pane->tree->get_item_at_position(p_position) : nullptr;
		if (item && item->get_metadata(COLUMN_NAME).get_type() == Variant::STRING) {
			_show_diff(item->get_metadata(COLUMN_NAME), pane->staged, true);
		}
		return;
	}
	if (p_mouse_button != MOUSE_BUTTON_RIGHT) {
		return;
	}
	Tree *tree = Object::cast_to<Tree>(p_tree);
	context_tree = tree;
	context_menu->clear();
	const bool built = tree == history_tree ? _build_commit_menu() : (tree == stashes_tree ? _build_stash_menu() : _build_file_menu(tree));
	if (!built) {
		return; // Nothing to offer for this row.
	}
	context_menu->set_position(Vector2i(tree->get_screen_position() + p_position));
	context_menu->reset_size();
	context_menu->popup();
}

// The right-click menu for History's selected row (a commit, or one of its files). False when
// the row has none.
bool GitDock::_build_commit_menu() {
	const String row = row_kind(history_tree->get_selected());
	if (row == "commit") {
		const Dictionary commit = history_tree->get_selected()->get_metadata(0);
		const bool busy = _in_operation() || network_op != NETWORK_NONE;
		// The last commit can be taken back while it's only here (like Amend).
		if (String(commit["hash"]) == String(sync_status.get("head", String()))) {
			context_menu->add_icon_item(get_theme_icon("UndoRedo", "EditorIcons"), "Undo Last Commit", MENU_UNDO_COMMIT);
			String why;
			if (busy) {
				why = _in_operation() ? vformat("Not while a %s is in progress.", _operation_name()) : String("Not while another operation runs.");
			} else if (!bool(commit["unpushed"]) && bool(sync_status.get("has_remotes", false))) {
				why = "It's already pushed, so undoing it here would leave your teammates with a commit you no longer have. Revert it instead.";
			} else if (bool(commit.get("merge", false))) {
				why = "It's a merge; undoing merges isn't supported here.";
			} else if (repo->get_commits(2).size() < 2) {
				why = "It's the first commit, so there's nothing to go back to.";
			}
			context_menu->set_item_disabled(-1, !why.is_empty());
			context_menu->set_item_tooltip(-1, why.is_empty() ? String("Take this commit back: the branch moves back one commit, and its changes wait in Staged Changes (with its message in the box) to be committed again.") : why);
		}
		context_menu->add_icon_item(get_theme_icon("Reload", "EditorIcons"), "Revert Commit...", MENU_REVERT_COMMIT);
		context_menu->set_item_disabled(-1, busy);
		context_menu->set_item_tooltip(-1, busy ? String("Not while another operation is in progress.") : String("Add a new commit that undoes this one's changes. Safe for pushed commits: nothing is rewritten."));
		context_menu->add_icon_item(get_theme_icon("VcsBranches", "EditorIcons"), "Create Branch Here...", MENU_BRANCH_HERE);
		context_menu->set_item_tooltip(-1, "Create a branch at this commit, to go back to it or try something from there. You stay on the current branch.");
		context_menu->add_separator();
		context_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), "Copy Commit Hash", MENU_COPY_HASH);
		context_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), "Copy Commit Message", MENU_COPY_MESSAGE);
		const String url = _web_commit_url(commit["hash"]);
		if (!url.is_empty()) {
			// Only offered for GitHub, GitLab and Bitbucket; before it's pushed there's no page yet.
			context_menu->add_separator();
			context_menu->add_icon_item(get_theme_icon("ExternalLink", "EditorIcons"), vformat("Open on %s", web_host_name(url)), MENU_OPEN_ON_WEB);
			if (commit["unpushed"]) {
				context_menu->set_item_disabled(-1, true);
				context_menu->set_item_tooltip(-1, "This commit isn't pushed yet, so it isn't there.");
			}
		}
		return true;
	}
	if (row == "file") {
		TreeItem *file = history_tree->get_selected();
		const String state = file->get_meta("git_state", String());
		const String hash = file->get_meta("git_hash", String());
		const String short_hash = hash.left(7);
		context_menu->add_icon_item(get_theme_icon("History", "EditorIcons"), "Show History of This File", MENU_SHOW_HISTORY);
		context_menu->add_separator();
		// Restoring puts a version back as an uncommitted change. A deleted file has no version in
		// the commit that deleted it; a renamed one's earlier version has another name.
		if (state != "deleted") {
			context_menu->add_icon_item(get_theme_icon("UndoRedo", "EditorIcons"), "Restore This Version", MENU_RESTORE_VERSION);
			context_menu->set_item_tooltip(-1, vformat("Put the file back as it was in %s, as an uncommitted change you can look at, commit or discard.", short_hash));
		}
		if (state != "renamed") {
			context_menu->add_icon_item(get_theme_icon("UndoRedo", "EditorIcons"), "Restore Version Before This Commit", MENU_RESTORE_BEFORE);
			context_menu->set_item_tooltip(-1, state == "new" ? vformat("The file didn't exist before %s, so this deletes it (as an uncommitted change).", short_hash) : vformat("Put the file back as it was before %s, as an uncommitted change.", short_hash));
		}
		context_menu->add_separator();
		context_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), "Copy Path", MENU_COPY_PATH);
		context_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), "Copy Relative Path", MENU_COPY_RELATIVE_PATH);
		return true;
	}
	return false;
}

// The right-click menu for the selected files of Staged Changes or Changes. False when nothing is
// selected.
bool GitDock::_build_file_menu(Tree *p_tree) {
	const FilePane *pane = _pane_for_tree(p_tree);
	const PackedStringArray paths = _selected_paths(p_tree);
	if (!pane || paths.is_empty()) {
		return false;
	}
	const bool single = paths.size() == 1;
	const String count = single ? String() : vformat(" (%d)", paths.size());

	if (single) {
		context_menu->add_icon_item(get_theme_icon("Load", "EditorIcons"), "Open", MENU_OPEN);
		context_menu->add_icon_item(get_theme_icon("History", "EditorIcons"), "Show History", MENU_SHOW_HISTORY);
		context_menu->set_item_tooltip(-1, "Show the commits that changed this file, in History.");
		context_menu->add_separator();
	}
	if (pane->staged) {
		context_menu->add_icon_item(_icon("GitMinus"), "Unstage" + count, MENU_UNSTAGE);
	} else {
		context_menu->add_icon_item(_icon("Add"), "Stage" + count, MENU_STAGE);
		context_menu->add_icon_item(get_theme_icon("UndoRedo", "EditorIcons"), "Discard Changes..." + count, MENU_DISCARD);
		if (single && can_ignore(paths[0])) {
			context_menu->add_icon_item(get_theme_icon("Hide", "EditorIcons"), "Ignore...", MENU_IGNORE);
			context_menu->set_item_tooltip(-1, "Keep this file (or every file of its type, or its folder) out of git, through .gitignore.");
		}
	}
	if (single) {
		const String lock = get_lock_action(paths[0]);
		if (lock == "lock") {
			context_menu->add_icon_item(get_theme_icon("Lock", "EditorIcons"), "Lock (Git LFS)", MENU_LOCK);
			context_menu->set_item_tooltip(-1, "Lock it on the LFS server, so nobody else can push changes to it until you unlock it.");
		} else if (lock == "unlock") {
			context_menu->add_icon_item(get_theme_icon("Unlock", "EditorIcons"), "Unlock (Git LFS)", MENU_UNLOCK);
		}
	}
	if (single) {
		context_menu->add_separator();
		if (_to_res_path(paths[0]).begins_with("res://")) {
			context_menu->add_icon_item(get_theme_icon("Filesystem", "EditorIcons"), "Show in FileSystem", MENU_SHOW_IN_FILESYSTEM);
		}
		context_menu->add_icon_item(get_theme_icon("Folder", "EditorIcons"), "Show in File Manager", MENU_SHOW_IN_FILE_MANAGER);
	}
	context_menu->add_separator();
	context_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), single ? "Copy Path" : "Copy Paths", MENU_COPY_PATH);
	context_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), single ? "Copy Relative Path" : "Copy Relative Paths", MENU_COPY_RELATIVE_PATH);
	return true;
}

void GitDock::_on_context_menu_id(int p_id) {
	if (!context_tree) {
		return;
	}
	PackedStringArray paths;
	if (context_tree != history_tree) {
		paths = _selected_paths(context_tree);
	} else if (history_tree->get_selected() && history_tree->get_selected()->has_meta("git_path")) {
		paths.push_back(history_tree->get_selected()->get_meta("git_path"));
	}

	if (p_id == MENU_RESTORE_STASH || p_id == MENU_DELETE_STASH) {
		TreeItem *stash = stashes_tree->get_selected();
		if (row_kind(stash) == "stash") {
			const String hash = Dictionary(stash->get_metadata(0))["hash"];
			if (p_id == MENU_RESTORE_STASH) {
				_restore_stash(hash);
			} else {
				_confirm_delete_stash(hash);
			}
		}
		return;
	}
	switch (p_id) {
		case MENU_LOCK:
		case MENU_UNLOCK: {
			if (paths.size() == 1) {
				lock_file(paths[0], p_id == MENU_LOCK);
			}
		} break;
		case MENU_IGNORE: {
			if (!paths.is_empty()) {
				show_ignore(paths[0]);
			}
		} break;
		case MENU_OPEN: {
			if (!paths.is_empty()) {
				_open_path(paths[0]);
			}
		} break;
		case MENU_STAGE: {
			_stage_paths(_selected_paths(context_tree, true), true);
		} break;
		case MENU_UNSTAGE: {
			_stage_paths(_selected_paths(context_tree, true), false);
		} break;
		case MENU_DISCARD: {
			_confirm_discard(_selected_paths(context_tree, true));
		} break;
		case MENU_SHOW_IN_FILESYSTEM: {
			EditorInterface::get_singleton()->select_file(_to_res_path(paths[0]));
		} break;
		case MENU_SHOW_IN_FILE_MANAGER: {
			OS::get_singleton()->shell_show_in_file_manager(repo->get_workdir().path_join(paths[0]), false);
		} break;
		case MENU_COPY_PATH:
		case MENU_COPY_RELATIVE_PATH: {
			PackedStringArray lines;
			for (const String &path : paths) {
				lines.push_back(p_id == MENU_COPY_PATH ? repo->get_workdir().path_join(path) : path);
			}
			DisplayServer::get_singleton()->clipboard_set(String("\n").join(lines));
		} break;
		case MENU_COPY_HASH:
		case MENU_COPY_MESSAGE: {
			const Dictionary commit = history_tree->get_selected()->get_metadata(0);
			DisplayServer::get_singleton()->clipboard_set(commit[p_id == MENU_COPY_HASH ? "hash" : "message"]);
		} break;
		case MENU_SHOW_HISTORY: {
			if (!paths.is_empty()) {
				show_file_history(paths[0]);
			}
		} break;
		case MENU_RESTORE_VERSION:
		case MENU_RESTORE_BEFORE: {
			TreeItem *file = history_tree->get_selected();
			if (row_kind(file) == "file") {
				const String hash = file->get_meta("git_hash");
				const bool before = p_id == MENU_RESTORE_BEFORE;
				_restore_version(before ? hash + String("^1") : hash, file->get_meta("git_path"), vformat(before ? "as it was before %s" : "as it was in %s", hash.left(7)));
			}
		} break;
		case MENU_UNDO_COMMIT: {
			_undo_last_commit();
		} break;
		case MENU_REVERT_COMMIT: {
			const Dictionary commit = history_tree->get_selected()->get_metadata(0);
			_confirm_revert(commit["hash"], commit["summary"]);
		} break;
		case MENU_BRANCH_HERE: {
			const Dictionary commit = history_tree->get_selected()->get_metadata(0);
			_show_branch_here(commit["hash"]);
		} break;
		case MENU_OPEN_ON_WEB: {
			const Dictionary commit = history_tree->get_selected()->get_metadata(0);
			OS::get_singleton()->shell_open(_web_commit_url(commit["hash"]));
		} break;
	}
}

// Selecting a file (by mouse or keyboard) shows its changes in the Diff panel.
void GitDock::_on_file_multi_selected(TreeItem *p_item, int p_column, bool p_selected, Object *p_tree) {
	const FilePane *pane = _pane_for_tree(p_tree);
	if (!p_selected || !pane || !p_item || p_item->get_metadata(COLUMN_NAME).get_type() != Variant::STRING) {
		return;
	}
	// One file at a time is shown, so only one list keeps a selection.
	(pane->staged ? changes_pane : staged_pane).tree->deselect_all();
	history_tree->deselect_all();
	stashes_tree->deselect_all();
	_show_diff(p_item->get_metadata(COLUMN_NAME), pane->staged, false);
}
