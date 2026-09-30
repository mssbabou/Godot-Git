// Stashes: setting changes aside (what's staged, from its section's header; everything, from the
// ⋮ menu or when changes are in a branch switch's way) and the Stashes section, which only shows
// while there are stashes. Each stash is a collapsed row (what's in it, its branch, its age) with
// Restore and Delete on hover; expanding it lists its files, each showing its diff when clicked.

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include "editor/git_dock_util.h"
#include "editor/ui_text.h"

using namespace godot_git;

void GitDock::_build_stashes(Control *p_parent) {
	stashes_pane = memnew(FoldableContainer);
	// Nothing else in its header: this keeps it as tall as the others (see _update_icons).
	stashes_header_strut = memnew(Control);
	stashes_header_strut->set_mouse_filter(MOUSE_FILTER_IGNORE);
	stashes_pane->add_title_bar_control(stashes_header_strut);
	stashes_pane->set_title("Stashes");
	stashes_pane->hide();
	p_parent->add_child(stashes_pane);

	stashes_tree = memnew(Tree);
	stashes_tree->set_hide_root(true);
	stashes_tree->set_select_mode(Tree::SELECT_ROW);
	stashes_tree->set_allow_rmb_select(true);
	stashes_tree->set_v_scroll_enabled(false); // Grow to fit; the outer ScrollContainer scrolls.
	stashes_tree->set_h_scroll_enabled(false);
	stashes_tree->set_columns(2);
	stashes_tree->set_column_expand(0, true);
	stashes_tree->set_column_clip_content(0, true);
	stashes_tree->set_column_expand(1, false);
	stashes_tree->connect("item_mouse_selected", callable_mp(this, &GitDock::_on_tree_mouse_selected).bind(stashes_tree));
	stashes_tree->connect("item_selected", callable_mp(this, &GitDock::_on_stash_item_selected));
	stashes_tree->connect("item_collapsed", callable_mp(this, &GitDock::_on_stash_item_collapsed));
	stashes_tree->connect("button_clicked", callable_mp(this, &GitDock::_on_stash_button_clicked));
	stashes_tree->connect("gui_input", callable_mp(this, &GitDock::_on_stashes_gui_input));
	stashes_tree->connect("mouse_exited", callable_mp(this, &GitDock::_set_stash_hovered).bind((TreeItem *)nullptr));
	_make_body(stashes_pane, stashes_tree);

	// Stashing asks once: it shows exactly what goes, and takes an optional name. Enter stashes.
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	stash_dialog = memnew(ConfirmationDialog);
	stash_dialog->set_title("Stash Changes");
	stash_dialog->set_ok_button_text("Stash");
	stash_dialog->connect("confirmed", callable_mp(this, &GitDock::_on_stash_dialog_confirmed));
	add_child(stash_dialog);
	VBoxContainer *stash_vb = memnew(VBoxContainer);
	stash_dialog->add_child(stash_vb);
	stash_files_label = memnew(Label);
	stash_files_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	// The label's own width, not its box's: a wrapping label measures its height at its minimum
	// width, and at 0 the dialog opened as tall as the screen (gotcha 28).
	stash_files_label->set_custom_minimum_size(Vector2(400 * scale, 0));
	stash_vb->add_child(stash_files_label);
	Label *name_label = memnew(Label);
	name_label->set_text("Name (optional)");
	stash_vb->add_child(name_label);
	stash_name_edit = memnew(LineEdit);
	stash_vb->add_child(stash_name_edit);
	stash_dialog->register_text_enter(stash_name_edit);

	stash_delete_confirm = _make_confirm("Delete Stash", "Delete", callable_mp(this, &GitDock::_on_stash_delete_confirmed));
	stash_switch_confirm = _make_confirm("Changes in the Way", "Stash and Switch", callable_mp(this, &GitDock::_stash_and_switch));
}

// The section, from get_stashes. Rebuilt only when the stashes changed, so expanded stashes and
// the selection survive the refresh every save triggers.
void GitDock::_fill_stashes() {
	const Array stashes = repo->get_stashes();
	stashes_pane->set_visible(!stashes.is_empty());
	stashes_pane->set_title(stashes.is_empty() ? String("Stashes") : vformat("Stashes (%d)", stashes.size()));
	TreeItem *root = stashes_tree->get_root();
	if (root && stashes == stashes_shown) {
		for (TreeItem *item = root->get_first_child(); item; item = item->get_next()) {
			const Dictionary stash = item->get_metadata(0);
			item->set_text(1, vformat(String::utf8("%s · %s"), stash["branch"], relative_time(stash["time"])));
		}
		return;
	}
	stashes_shown = stashes;
	stash_hovered = 0;
	stashes_tree->clear();
	root = stashes_tree->create_item();
	const Ref<Texture2D> icon = _icon("GitStash");
	const Color dim = _dim_color();
	for (int i = 0; i < stashes.size(); i++) {
		const Dictionary stash = stashes[i];
		TreeItem *item = stashes_tree->create_item(root);
		item->set_meta("git_row", "stash");
		item->set_metadata(0, stash);
		item->set_icon(0, icon);
		item->set_text(0, stash["message"]);
		const String when = local_date_time(stash["time"]);
		item->set_tooltip_text(0, vformat("%s\n\nStashed on %s, %s. Restore puts these changes back and removes the stash.", stash["message"], stash["branch"], when));
		item->set_text(1, vformat(String::utf8("%s · %s"), stash["branch"], relative_time(stash["time"])));
		item->set_text_overrun_behavior(1, TextServer::OVERRUN_NO_TRIMMING);
		item->set_custom_color(1, dim);
		item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
		item->set_tooltip_text(1, when);

		// Collapsed with a placeholder child, so it shows the arrow; the files load on expand.
		TreeItem *placeholder = stashes_tree->create_item(item);
		placeholder->set_meta("git_row", "placeholder");
		placeholder->set_selectable(0, false);
		placeholder->set_selectable(1, false);
		// Read before collapsing: collapsing emits item_collapsed, which forgets it was expanded.
		const bool expanded = stashes_expanded.has(stash["hash"]);
		item->set_collapsed(true);
		if (expanded) {
			item->set_collapsed(false); // Loads it (item_collapsed).
		}
	}
}

// An expanded stash's files.
void GitDock::_fill_stash(TreeItem *p_item) {
	while (p_item->get_first_child()) {
		memdelete(p_item->get_first_child());
	}
	const String hash = Dictionary(p_item->get_metadata(0))["hash"];
	if (!stash_files.has(hash)) {
		stash_files[hash] = repo->get_stash_files(hash);
	}
	_add_commit_file_rows(stashes_tree, p_item, stash_files[hash], hash, INT32_MAX);
}

void GitDock::_on_stash_item_collapsed(TreeItem *p_item) {
	if (row_kind(p_item) != "stash") {
		return;
	}
	const String hash = Dictionary(p_item->get_metadata(0))["hash"];
	if (p_item->is_collapsed()) {
		stashes_expanded.erase(hash);
		return;
	}
	stashes_expanded[hash] = true;
	// Deferred for the same reason as History's commits: no new rows during the Tree's click (gotcha 41).
	callable_mp(this, &GitDock::_fill_stash_later).call_deferred(p_item->get_instance_id());
}

void GitDock::_fill_stash_later(uint64_t p_item) {
	TreeItem *item = Object::cast_to<TreeItem>(ObjectDB::get_instance(ObjectID(p_item)));
	TreeItem *first = item ? item->get_first_child() : nullptr;
	if (!item || item->is_collapsed() || !first || row_kind(first) != "placeholder") {
		return; // Gone (rebuilt), collapsed again, or already filled.
	}
	_fill_stash(item);
	_select_diff_row();
}

// Selecting a stash's file shows its change in the Diff panel; one list keeps a selection.
void GitDock::_on_stash_item_selected() {
	TreeItem *item = stashes_tree->get_selected();
	if (row_kind(item) != "file") {
		return;
	}
	staged_pane.tree->deselect_all();
	changes_pane.tree->deselect_all();
	history_tree->deselect_all();
	_show_commit_diff(item->get_meta("git_hash"), item->get_meta("git_path"), false, true);
}

// Restore and Delete only on the stash row under the mouse, like the file rows' buttons.
void GitDock::_on_stashes_gui_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		TreeItem *item = stashes_tree->get_item_at_position(motion->get_position());
		_set_stash_hovered(row_kind(item) == "stash" ? item : nullptr);
	}
}

void GitDock::_set_stash_hovered(TreeItem *p_item) {
	const uint64_t id = p_item ? (uint64_t)p_item->get_instance_id() : 0;
	if (id == stash_hovered) {
		return;
	}
	if (TreeItem *old = Object::cast_to<TreeItem>(ObjectDB::get_instance(ObjectID(stash_hovered)))) {
		while (old->get_button_count(1) > 0) {
			old->erase_button(1, 0);
		}
	}
	stash_hovered = id;
	if (p_item) {
		p_item->add_button(1, _icon("GitRestore"), STASH_RESTORE, false, "Restore: put these changes back and remove the stash");
		p_item->add_button(1, get_theme_icon("Remove", "EditorIcons"), STASH_DELETE, false, "Delete this stash");
	}
}

void GitDock::_on_stash_button_clicked(TreeItem *p_item, int p_column, int p_id, int p_mouse_button) {
	const String hash = Dictionary(p_item->get_metadata(0))["hash"];
	if (p_id == STASH_RESTORE) {
		_restore_stash(hash);
	} else if (p_id == STASH_DELETE) {
		_confirm_delete_stash(hash);
	}
}

// The right-click menu on a stash row. False for its files (nothing to offer there yet).
bool GitDock::_build_stash_menu() {
	if (row_kind(stashes_tree->get_selected()) != "stash") {
		return false;
	}
	context_menu->add_icon_item(_icon("GitRestore"), "Restore", MENU_RESTORE_STASH);
	context_menu->add_icon_item(get_theme_icon("Remove", "EditorIcons"), "Delete...", MENU_DELETE_STASH);
	return true;
}

// Stash what's staged (p_staged) or everything: first the dialog, showing what goes and asking
// for an optional name (named after its files otherwise, the name field's placeholder).
void GitDock::_stash(bool p_staged) {
	PackedStringArray paths;
	for (const Variant &entry : repo->get_status()) {
		const Dictionary item = entry;
		if (!String(item["index"]).is_empty() || (!p_staged && !String(item["worktree"]).is_empty())) {
			paths.push_back(item["path"]);
		}
	}
	if (paths.is_empty()) {
		_set_status(STATUS_NEUTRAL, p_staged ? "Nothing is staged, so there's nothing to stash." : "There are no changes to stash.");
		return;
	}
	PackedStringArray lines;
	PackedStringArray names;
	for (int i = 0; i < paths.size(); i++) {
		names.push_back(paths[i].get_file());
		if (i < 10) {
			lines.push_back(String::utf8("• ") + paths[i]);
		}
	}
	if (paths.size() > 10) {
		lines.push_back(vformat("...and %d more", paths.size() - 10));
	}
	stash_dialog_staged = p_staged;
	stash_files_label->set_text(vformat("%s, to restore later from Stashes:\n%s", p_staged ? "Set aside what's staged" : "Set aside every change, new files included", String("\n").join(lines)));
	stash_name_edit->clear();
	stash_name_edit->set_placeholder(join_list(names, 3));
	stash_dialog->popup_centered();
	stash_name_edit->grab_focus();
}

// After offering to save open scenes and scripts: a stash takes what's on disk, and saving an
// unsaved scene afterwards would bring its changes back.
void GitDock::_on_stash_dialog_confirmed() {
	if (_ask_to_save("Stash", callable_mp(this, &GitDock::_on_stash_dialog_confirmed))) {
		return;
	}
	// Left empty: the placeholder it showed, so the stash gets the name the dialog promised.
	const String name = stash_name_edit->get_text().strip_edges();
	_run_stash(stash_dialog_staged, name.is_empty() ? stash_name_edit->get_placeholder() : name);
}

Error GitDock::_run_stash(bool p_staged, const String &p_name) {
	_remember_open_scenes();
	const Error err = repo->stash(p_staged, p_name);
	_report(err, "Stash");
	if (err == OK) {
		EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	}
	refresh();
	if (err == OK) {
		const Array stashes = repo->get_stashes();
		_set_status(STATUS_SUCCESS, stashes.is_empty() ? String("Stashed") : vformat("Stashed %s", Dictionary(stashes[0])["message"]));
		_reload_changed_scenes();
	}
	return err;
}

// From the switch's "changes in the way" question: stash everything, then switch.
void GitDock::_stash_and_switch() {
	const String target = pending_switch;
	if (_ask_to_save("Switch", callable_mp(this, &GitDock::_stash_and_switch))) {
		return;
	}
	if (target.is_empty() || _run_stash(false) != OK) {
		pending_switch = String();
		return;
	}
	unsaved_checked = true; // Already asked about unsaved files, just now.
	_switch_branch(target);
}

void GitDock::_restore_stash(const String &p_hash) {
	if (_ask_to_save("Restore", callable_mp(this, &GitDock::_restore_stash).bind(p_hash))) {
		return;
	}
	String message;
	for (const Variant &stash : stashes_shown) {
		if (String(Dictionary(stash)["hash"]) == p_hash) {
			message = Dictionary(stash)["message"];
		}
	}
	_remember_open_scenes();
	const Error err = repo->restore_stash(p_hash);
	_report(err, "Restore");
	if (err == OK) {
		EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	}
	refresh();
	if (err == OK) {
		const String notice = repo->get_notice();
		if (notice.is_empty()) {
			_set_status(STATUS_SUCCESS, vformat("Restored %s", message));
		} else {
			_set_status(STATUS_WARNING, notice);
		}
		_reload_changed_scenes();
	}
}

void GitDock::_confirm_delete_stash(const String &p_hash) {
	pending_stash_delete = p_hash;
	String message;
	for (const Variant &stash : stashes_shown) {
		if (String(Dictionary(stash)["hash"]) == p_hash) {
			message = Dictionary(stash)["message"];
		}
	}
	stash_delete_confirm->set_text(vformat("Delete the stash with your changes to %s?\nThis can't be undone.", message));
	stash_delete_confirm->popup_centered();
}

void GitDock::_on_stash_delete_confirmed() {
	const String hash = pending_stash_delete;
	pending_stash_delete = String();
	const Error err = repo->delete_stash(hash);
	_report(err, "Delete stash");
	refresh();
	if (err == OK) {
		_set_status(STATUS_NEUTRAL, "Deleted the stash.");
	}
}
