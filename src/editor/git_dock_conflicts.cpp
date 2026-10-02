// GitDock: the Conflicts section (files a merge, rebase, cherry-pick or stash left conflicted)
// and resolving them, in the Git Diff panel's resolver (GitConflictView).

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_interface.hpp>

#include "editor/git_colors.h"
#include "editor/git_diff_dock.h"
#include "editor/git_dock_util.h"
#include "editor/ui_text.h"

using namespace godot_git;

// Above Staged Changes, and only while there are conflicts: it's the one thing to do first, and
// it goes away with them.
void GitDock::_build_conflicts(Control *p_parent) {
	conflicts_pane = memnew(FoldableContainer);
	conflicts_header_strut = memnew(Control); // Keeps its header as tall as the others.
	conflicts_header_strut->set_mouse_filter(MOUSE_FILTER_IGNORE);
	conflicts_pane->add_title_bar_control(conflicts_header_strut);
	conflicts_pane->set_title("Conflicts");
	conflicts_pane->hide();
	p_parent->add_child(conflicts_pane);

	conflicts_tree = memnew(Tree);
	conflicts_tree->set_hide_root(true);
	conflicts_tree->set_select_mode(Tree::SELECT_ROW);
	conflicts_tree->set_v_scroll_enabled(false); // Grow to fit; the outer ScrollContainer scrolls.
	conflicts_tree->set_h_scroll_enabled(false);
	conflicts_tree->set_columns(2);
	conflicts_tree->set_column_expand(0, true);
	conflicts_tree->set_column_clip_content(0, true);
	conflicts_tree->set_column_expand(1, false);
	conflicts_tree->add_theme_constant_override("item_margin", 0);
	conflicts_tree->connect("item_selected", callable_mp(this, &GitDock::_on_conflict_selected));
	_make_body(conflicts_pane, conflicts_tree);
}

void GitDock::_fill_conflicts(const Array &p_status) {
	PackedStringArray paths;
	for (int i = 0; i < p_status.size(); i++) {
		const Dictionary entry = p_status[i];
		if (String(entry["worktree"]) == "conflicted" || String(entry["index"]) == "conflicted") {
			paths.push_back(entry["path"]);
		}
	}
	conflicted_paths = paths;
	conflicts_pane->set_visible(!paths.is_empty());
	conflicts_pane->set_title(paths.is_empty() ? String("Conflicts") : vformat("Conflicts (%d)", paths.size()));
	conflicts_tree->clear();
	TreeItem *root = conflicts_tree->create_item();
	for (const String &path : paths) {
		TreeItem *item = conflicts_tree->create_item(root);
		item->set_meta("git_path", path);
		item->set_meta("git_state", "conflicted");
		item->set_meta("git_icon", _file_icon(path));
		item->set_cell_mode(0, TreeItem::CELL_MODE_CUSTOM);
		item->set_custom_draw_callback(0, callable_mp(this, &GitDock::_draw_file_row));
		item->set_text(0, path.get_file());
		item->set_custom_color(0, Color(0, 0, 0, 0));
		item->set_tooltip_text(0, vformat("%s\nBoth sides changed it. Click to resolve it in the Git Diff panel.", path));
		// The letter in the second column, like History's file rows.
		item->set_text(1, status_letter("conflicted"));
		item->set_text_overrun_behavior(1, TextServer::OVERRUN_NO_TRIMMING); // Gotcha 4.
		item->set_custom_color(1, _status_color("conflicted"));
		item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
		item->set_selectable(1, false);
		if (diff_conflict && path == diff_path) {
			item->select(0);
		}
	}
}

void GitDock::_on_conflict_selected() {
	TreeItem *item = conflicts_tree->get_selected();
	if (item) {
		staged_pane.tree->deselect_all();
		changes_pane.tree->deselect_all();
		history_tree->deselect_all();
		stashes_tree->deselect_all();
		_show_conflict(item->get_meta("git_path"));
	}
}

// Opens a conflicted file in the Git Diff panel's resolver.
void GitDock::_show_conflict(const String &p_path) {
	diff_path = p_path;
	diff_conflict = true;
	diff_commit = String();
	diff_commit_shown = String();
	diff_stash = false;
	_update_diff();
	if (diff_dock) {
		diff_dock->make_visible();
	}
}

// From the resolver: its result (p_text) or a whole side (p_side), written and staged. Then the
// next conflicted file, if any, or this file's staged change.
void GitDock::_resolve_conflict(const String &p_path, const String &p_text, const String &p_side) {
	_remember_open_scenes();
	const Error err = p_side.is_empty() ? repo->resolve_conflict(p_path, p_text) : repo->resolve_conflict_with(p_path, p_side);
	_report(err, "Resolve");
	if (err != OK) {
		return;
	}
	diff_conflict = false;
	refresh();
	_reload_changed_scenes();
	const String how = p_side.is_empty() ? String() : (p_side == "mine" ? String(" with your version") : String(" with their version"));
	if (conflicted_paths.is_empty()) {
		const String finish = _in_operation() ? vformat(": finish the %s when you're ready", _operation_name()) : String();
		_set_status(STATUS_SUCCESS, vformat("Resolved %s%s. No conflicts left%s", p_path.get_file(), how, finish));
		// What the resolution changed, if anything (keeping your version of a file often changes
		// nothing: then the panel is cleared rather than saying there's nothing there).
		if (String(repo->get_diff(p_path, true).get("kind", String())) != "unchanged") {
			_show_diff(p_path, true, false);
		} else if (diff_dock) {
			diff_path = String();
			diff_dock->set_diff(Dictionary(), String(), Ref<Texture2D>());
		}
	} else {
		_set_status(STATUS_SUCCESS, vformat("Resolved %s%s. %s left", p_path.get_file(), how, plural(conflicted_paths.size(), "conflicted file", "conflicted files")));
		_show_conflict(conflicted_paths[0]);
	}
}

void GitDock::_on_conflict_text(const String &p_path, const String &p_text) {
	_resolve_conflict(p_path, p_text, String());
}

void GitDock::_on_conflict_side(const String &p_path, const String &p_side) {
	_resolve_conflict(p_path, String(), p_side);
}
