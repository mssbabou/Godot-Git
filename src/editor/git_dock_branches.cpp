// The branch picker: a button naming the current branch that opens a panel with a search field
// and every branch (ahead/behind, age, Delete on hover); switching (in the background when LFS
// files may need downloading), creating and deleting.

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/core/math.hpp>

#include "addon_files.h"
#include "build_info.h"
#include "editor/file_opener.h"
#include "editor/filesystem_colors.h"
#include "editor/git_dock_util.h"
#include "editor/ui_text.h"

using namespace godot_git;

namespace {

// A branch row's buttons (shown on hover).
enum BranchButton {
	BRANCH_DELETE,
};

// What a row in the branch panel is (meta "git_branch_row").
const char *ROW_CREATE = "create";
const char *ROW_LOCAL = "local";
const char *ROW_REMOTE = "remote";
const char *ROW_HEADING = "heading";

} // namespace

// The button looks like Godot's own OptionButton (its styles and arrow, copied from the editor
// theme: "OptionButton" as a theme type variation does nothing, it isn't declared as one), but
// opens a panel of our own: an OptionButton's list can't search, mark the current branch, show
// how branches stand, or put actions on the branch they act on.
void GitDock::_build_branch_picker(Control *p_parent) {
	branch_button = memnew(Button);
	branch_button->set_name("BranchButton");
	branch_button->set_h_size_flags(SIZE_EXPAND_FILL);
	branch_button->set_clip_text(true);
	// "feature/inventory-system-re…", not cut off mid-letter.
	branch_button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	branch_button->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	branch_button->connect("pressed", callable_mp(this, &GitDock::_show_branch_popup));
	p_parent->add_child(branch_button);
	branch_style_source = memnew(OptionButton);
	branch_style_source->hide();
	p_parent->add_child(branch_style_source);
	branch_arrow = memnew(TextureRect);
	branch_arrow->set_mouse_filter(MOUSE_FILTER_IGNORE);
	branch_arrow->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	branch_arrow->set_anchors_and_offsets_preset(PRESET_RIGHT_WIDE);
	branch_button->add_child(branch_arrow);

	branch_popup = memnew(PopupPanel);
	branch_popup->set_name("BranchPopup");
	branch_popup->connect("popup_hide", callable_mp(this, &GitDock::_set_branch_hovered).bind((TreeItem *)nullptr));
	add_child(branch_popup);
	VBoxContainer *layout = memnew(VBoxContainer);
	branch_popup->add_child(layout);
	branch_search = memnew(LineEdit);
	branch_search->set_placeholder("Switch to a branch, or name a new one");
	branch_search->set_clear_button_enabled(true);
	branch_search->connect("text_changed", callable_mp(this, &GitDock::_on_branch_search_changed));
	branch_search->connect("text_submitted", callable_mp(this, &GitDock::_on_branch_row_activated).unbind(1));
	branch_search->connect("gui_input", callable_mp(this, &GitDock::_on_branch_search_input));
	layout->add_child(branch_search);
	branch_tree = memnew(Tree);
	branch_tree->set_hide_root(true);
	branch_tree->set_columns(2);
	branch_tree->set_column_expand(0, true);
	branch_tree->set_column_clip_content(0, true);
	branch_tree->set_column_expand(1, false);
	branch_tree->set_h_scroll_enabled(false);
	branch_tree->set_select_mode(Tree::SELECT_ROW);
	branch_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	branch_tree->connect("item_mouse_selected", callable_mp(this, &GitDock::_on_branch_row_clicked));
	branch_tree->connect("item_activated", callable_mp(this, &GitDock::_on_branch_row_activated));
	branch_tree->connect("button_clicked", callable_mp(this, &GitDock::_on_branch_row_button));
	branch_tree->connect("gui_input", callable_mp(this, &GitDock::_on_branch_tree_input));
	branch_tree->connect("mouse_exited", callable_mp(this, &GitDock::_set_branch_hovered).bind((TreeItem *)nullptr));
	layout->add_child(branch_tree);
}

// On every refresh: the button's name, and the panel's list if it's open.
void GitDock::_fill_branches() {
	branch_list = repo->get_branch_list();
	String current = sync_status.get("branch", String());
	if (current.is_empty() || current == "HEAD") {
		current = "(no branch)";
	}
	branch_button->set_text(current);
	if (branch_popup->is_visible()) {
		_fill_branch_tree();
	}
}

// The button's icon, and OptionButton's arrow drawn at its right end, with room left for it so
// a long name trims before the arrow. On theme changes (the styles are copies of the theme's).
void GitDock::_style_branch_button() {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	branch_button->set_button_icon(get_theme_icon("VcsBranches", "EditorIcons"));
	// Copied from a real OptionButton (hidden, next to the button): what the editor theme draws
	// dropdowns with isn't what EditorInterface::get_editor_theme() says for "OptionButton" (that
	// gave Godot's placeholder, a red outline without fill), nor Button's style (too faint).
	const Ref<Texture2D> arrow = branch_style_source->get_theme_icon("arrow");
	branch_arrow->set_texture(arrow);
	const float right = branch_style_source->get_theme_stylebox("normal")->get_margin(SIDE_RIGHT);
	branch_arrow->set_offset(SIDE_LEFT, -arrow->get_width() - right);
	branch_arrow->set_offset(SIDE_RIGHT, -right);
	for (const char *state : { "normal", "hover", "pressed", "hover_pressed", "disabled", "focus" }) {
		Ref<StyleBox> style = branch_style_source->get_theme_stylebox(state);
		if (style.is_valid()) {
			style = style->duplicate();
			style->set_content_margin(SIDE_RIGHT, style->get_content_margin(SIDE_RIGHT) + arrow->get_width() + 4 * scale);
			branch_button->add_theme_stylebox_override(state, style);
		}
	}
	for (const char *color : { "font_color", "font_hover_color", "font_pressed_color", "font_focus_color", "font_disabled_color" }) {
		branch_button->add_theme_color_override(color, branch_style_source->get_theme_color(color));
	}
}

void GitDock::_show_branch_popup() {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	branch_search->clear();
	_fill_branch_tree();
	// The Tree's own button hover is its row hover in the 4.7 theme (gotcha 48).
	branch_tree->remove_theme_stylebox_override("button_hover");
	if (branch_tree->get_theme_stylebox("button_hover") == branch_tree->get_theme_stylebox("hovered")) {
		branch_tree->add_theme_stylebox_override("button_hover", branch_tree->get_theme_stylebox("button_pressed"));
	}
	Ref<StyleBoxEmpty> none;
	none.instantiate();
	branch_tree->add_theme_stylebox_override("panel", none);
	branch_tree->add_theme_stylebox_override("focus", none);
	// Framed like Godot's own dropdown menus (the ⋮ menu's popup is one), not PopupPanel's plainer
	// box, whose thin border looked off next to them.
	branch_popup->add_theme_stylebox_override("panel", more_menu->get_popup()->get_theme_stylebox("panel"));
	// As wide as the button (at least a comfortable width), as tall as its rows up to a limit.
	const float width = MAX(branch_button->get_size().x + more_menu->get_size().x, 300 * scale);
	branch_popup->set_size(Vector2i(width, 0));
	branch_popup->set_position(branch_button->get_screen_position() + Vector2(0, branch_button->get_size().y));
	branch_popup->popup();
	branch_search->grab_focus();
}

// The rows: a Create row while the search names no branch, local branches, then remote ones
// with no local branch, filtered by the search. The first row you can pick is selected, so
// Enter picks it.
void GitDock::_fill_branch_tree() {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	const String query = branch_search->get_text().strip_edges();
	const String lower = query.to_lower();
	branch_hovered = 0;
	branch_tree->clear();
	TreeItem *root = branch_tree->create_item();
	const Color dim = _dim_color();

	bool exact = false;
	for (int i = 0; i < branch_list.size(); i++) {
		exact = exact || String(Dictionary(branch_list[i])["name"]) == query;
	}
	bool remote_heading = false;
	for (int i = 0; i < branch_list.size(); i++) {
		const Dictionary branch = branch_list[i];
		const String name = branch["name"];
		if (!lower.is_empty() && !name.to_lower().contains(lower)) {
			continue;
		}
		const bool local = branch["local"];
		const bool current = branch["current"];
		if (!local && !remote_heading) {
			remote_heading = true;
			TreeItem *heading = branch_tree->create_item(root);
			heading->set_meta("git_branch_row", ROW_HEADING);
			heading->set_text(0, "Remote branches");
			heading->set_custom_color(0, dim);
			heading->set_selectable(0, false);
			heading->set_selectable(1, false);
		}
		TreeItem *item = branch_tree->create_item(root);
		item->set_meta("git_branch_row", local ? ROW_LOCAL : ROW_REMOTE);
		item->set_meta("git_current", current);
		item->set_metadata(0, name);
		item->set_text(0, name);
		// The current branch has a check; the others the branch icon, remote ones dimmed.
		item->set_icon(0, get_theme_icon(current ? "ImportCheck" : "VcsBranches", "EditorIcons"));
		if (!local) {
			item->set_icon_modulate(0, Color(1, 1, 1, 0.5));
		}

		// Right: how it stands against the branch it follows, and its age.
		const int ahead = branch["ahead"];
		const int behind = branch["behind"];
		const int64_t time = branch["time"];
		const String upstream = branch["upstream"];
		PackedStringArray parts;
		if (ahead > 0) {
			parts.push_back(String::utf8("↑") + itos(ahead));
		}
		if (behind > 0) {
			parts.push_back(String::utf8("↓") + itos(behind));
		}
		if (time > 0) {
			parts.push_back(relative_time(time));
		}
		item->set_text(1, String::utf8(" · ").join(parts));
		item->set_custom_color(1, dim);
		item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
		item->set_text_overrun_behavior(1, TextServer::OVERRUN_NO_TRIMMING); // Gotcha 4.

		String tooltip = name;
		if (current) {
			tooltip += String("\nThe current branch.");
		}
		if (!local) {
			tooltip += vformat("\nOn the remote only: picking it creates a local %s that follows it.", name.substr(name.find("/") + 1));
		} else if (upstream.is_empty()) {
			tooltip += String("\nNot on the remote yet: Publish sends it there.");
		} else if (ahead == 0 && behind == 0) {
			tooltip += vformat("\nUp to date with %s.", upstream);
		} else {
			tooltip += vformat("\nAgainst %s: %s to push, %s to pull.", upstream, itos(ahead), itos(behind));
		}
		if (time > 0) {
			tooltip += vformat("\nLast commit %s.", local_date_time(time));
		}
		item->set_tooltip_text(0, tooltip);
		item->set_tooltip_text(1, tooltip);
	}
	// After the matches: Enter should pick "experiment" when you typed "exp", not create "exp".
	if (!query.is_empty() && !exact && has_commits) {
		TreeItem *create = branch_tree->create_item(root);
		create->set_meta("git_branch_row", ROW_CREATE);
		create->set_metadata(0, query);
		create->set_icon(0, get_theme_icon("Add", "EditorIcons"));
		create->set_text(0, vformat("Create branch \"%s\"", query));
		create->set_custom_color(0, get_theme_color("accent_color", "Editor"));
		create->set_tooltip_text(0, vformat("Create %s from the current commit and switch to it. Your uncommitted changes come along.", query));
		create->set_selectable(1, false);
	}
	if (!root->get_first_child()) {
		TreeItem *none = branch_tree->create_item(root);
		none->set_meta("git_branch_row", ROW_HEADING);
		none->set_text(0, has_commits ? String("No branch has that name.") : String("Make a first commit to create branches."));
		none->set_custom_color(0, dim);
		none->set_selectable(0, false);
		none->set_selectable(1, false);
	}

	// Selected (what Enter picks): while searching the first match, or Create when nothing matches;
	// else the current branch.
	TreeItem *pick = nullptr;
	for (TreeItem *item = root->get_first_child(); item && !pick; item = item->get_next()) {
		const String kind = item->get_meta("git_branch_row", String());
		if (query.is_empty() ? bool(item->get_meta("git_current", false)) : kind != ROW_HEADING) {
			pick = item;
		}
	}
	if (pick) {
		pick->select(0);
		branch_tree->scroll_to_item(pick);
	}

	_fit_branch_popup();
	// Once more a frame later: row positions are only final after the Tree's layout.
	callable_mp(this, &GitDock::_fit_branch_popup).call_deferred();
}

// As tall as the rows, up to a limit (it scrolls beyond): where the last row ends. The height a
// Tree reports with scrolling off (gotcha 8) has ~20 px more, an empty stripe under the list.
void GitDock::_fit_branch_popup() {
	TreeItem *root = branch_tree->get_root();
	if (!root || root->get_child_count() == 0) {
		return;
	}
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	const Rect2 last = branch_tree->get_item_area_rect(root->get_child(root->get_child_count() - 1));
	float rows = last.get_end().y + 2 * scale;
	if (last.size.y <= 0) {
		branch_tree->set_v_scroll_enabled(false);
		rows = branch_tree->get_combined_minimum_size().y;
		branch_tree->set_v_scroll_enabled(true);
	}
	branch_tree->set_custom_minimum_size(Vector2(0, MIN(rows, 380 * scale)));
	const int width = branch_popup->get_size().x; // Set by _show_branch_popup; keep it.
	branch_popup->reset_size();
	branch_popup->set_size(Vector2i(MAX(width, branch_popup->get_size().x), branch_popup->get_size().y));
}

void GitDock::_on_branch_search_changed(const String &p_text) {
	_fill_branch_tree();
}

// Down from the search field goes into the list (so arrows and Enter work without the mouse).
void GitDock::_on_branch_search_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && (key->get_keycode() == KEY_DOWN || key->get_keycode() == KEY_UP)) {
		branch_search->accept_event();
		branch_tree->grab_focus();
		if (!branch_tree->get_selected() && branch_tree->get_root() && branch_tree->get_root()->get_first_child()) {
			branch_tree->get_root()->get_first_child()->select(0);
		}
	}
}

void GitDock::_on_branch_tree_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		_set_branch_hovered(branch_tree->get_item_at_position(motion->get_position()));
	}
}

// Delete on the row under the mouse, like the file rows' buttons; not on the current branch or
// remote branches. (Rename was here too; dropped, maintainer 2026-09-30: nobody renames branches.)
void GitDock::_set_branch_hovered(TreeItem *p_item) {
	const uint64_t id = p_item && String(p_item->get_meta("git_branch_row", String())) == ROW_LOCAL && !bool(p_item->get_meta("git_current", false)) ? p_item->get_instance_id() : 0;
	if (id == branch_hovered) {
		return;
	}
	if (TreeItem *old = Object::cast_to<TreeItem>(ObjectDB::get_instance(ObjectID(branch_hovered)))) {
		while (old->get_button_count(1) > 0) {
			old->erase_button(1, 0);
		}
	}
	branch_hovered = id;
	if (!id) {
		return;
	}
	const String name = p_item->get_metadata(0);
	p_item->add_button(1, get_theme_icon("Remove", "EditorIcons"), BRANCH_DELETE, false, vformat("Delete %s (asks first, and says if commits would be lost)", name));
}

void GitDock::_on_branch_row_button(TreeItem *p_item, int p_column, int p_id, int p_mouse_button) {
	const String name = p_item->get_metadata(0);
	branch_popup->hide();
	if (p_id == BRANCH_DELETE) {
		callable_mp(this, &GitDock::_show_delete_branch_dialog).call_deferred(name);
	}
}

// A click picks the row. Deferred: switching refreshes, which rebuilds this tree, and a Tree
// can't rebuild while it handles the click (gotcha 41).
void GitDock::_on_branch_row_clicked(const Vector2 &p_position, int p_mouse_button) {
	TreeItem *item = branch_tree->get_item_at_position(p_position);
	if (p_mouse_button == MOUSE_BUTTON_LEFT && item) {
		callable_mp(this, &GitDock::_pick_branch_row).call_deferred(item->get_instance_id());
	}
}

// Enter (in the search field or the list) or a double click: the selected row.
void GitDock::_on_branch_row_activated() {
	TreeItem *item = branch_tree->get_selected();
	if (item) {
		callable_mp(this, &GitDock::_pick_branch_row).call_deferred(item->get_instance_id());
	}
}

void GitDock::_pick_branch_row(uint64_t p_item) {
	TreeItem *item = Object::cast_to<TreeItem>(ObjectDB::get_instance(ObjectID(p_item)));
	if (!item) {
		return;
	}
	const String kind = item->get_meta("git_branch_row", String());
	if (kind == ROW_HEADING) {
		return;
	}
	const String name = item->get_metadata(0);
	branch_popup->hide();
	if (kind == ROW_CREATE) {
		const Error err = repo->create_branch(name);
		_report(err, "Create branch");
		refresh();
		if (err == OK) {
			_set_status(STATUS_SUCCESS, vformat("Created %s and switched to it", name));
		}
		return;
	}
	_pick_branch(name);
}

// Switches to p_branch (a local branch, or a remote one to check out as a local branch).
void GitDock::_pick_branch(const String &p_branch) {
	if (p_branch == repo->get_current_branch()) {
		return;
	}
	const String addon = _addon_removed_by(p_branch);
	if (!addon.is_empty()) {
		// Godot unloads an addon whose files disappear, so the panel would close mid-switch.
		pending_switch = p_branch;
		switch_confirm->set_text(vformat("\"%s\" doesn't include this Git panel (%s).\nSwitching removes it from the project, so the panel closes.\nSwitch back and restart the editor to get it back.", p_branch, addon));
		switch_confirm->popup_centered();
		return;
	}
	_switch_branch(p_branch);
}

// The addon's folder ("addons/godot_git") if switching to p_branch would delete it: it's
// committed on the current branch but missing on p_branch. Empty otherwise.
String GitDock::_addon_removed_by(const String &p_branch) const {
	const String manifest = godot_git::addon_manifest_path();
	if (manifest.is_empty()) {
		return String();
	}
	const String absolute = manifest.simplify_path();
	const String workdir = repo->get_workdir().simplify_path().trim_suffix("/") + "/";
	if (!absolute.begins_with(workdir)) {
		return String();
	}
	const String path = absolute.substr(workdir.length());
	if (!repo->has_file_at("HEAD", path) || repo->has_file_at(p_branch, path)) {
		return String();
	}
	return path.get_base_dir();
}

// p_branch is empty when it comes from the switch confirmation.
void GitDock::_switch_branch(const String &p_branch) {
	const String target = p_branch.is_empty() ? pending_switch : p_branch;
	pending_switch = String();
	if (target.is_empty() || !repo.is_valid()) {
		return;
	}
	if (_ask_to_save("Switch", callable_mp(this, &GitDock::_switch_branch).bind(target))) {
		return;
	}
	if (repo->uses_lfs()) {
		// May download LFS files: in the background, with progress and Cancel.
		network_branch = target;
		_start_network(NETWORK_SWITCH);
		return;
	}
	_remember_open_scenes();
	const Error err = repo->checkout_branch(target);
	if (err == ERR_BUSY) {
		// Your changes are in the way: offer to set them aside (they wait under Stashes).
		pending_switch = target;
		stash_switch_confirm->set_text(vformat("%s\n\nStash them and switch to %s? They'll wait under Stashes, where you can restore them when you come back.", GitRepository::get_last_error(), target));
		stash_switch_confirm->popup_centered();
		return;
	}
	_report(err, "Switch branch");
	if (err == OK) {
		EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	}
	refresh();
	if (err == OK) {
		_set_status(STATUS_SUCCESS, vformat("Switched to %s", repo->get_current_branch()));
		_reload_changed_scenes(); // After the status: a scene it couldn't reload warns there.
	}
}

// Create Branch Here (History): new branches from the current commit are made by typing a name in
// the branch picker.
void GitDock::_on_branch_dialog_confirmed() {
	const String name = branch_name_edit->get_text().strip_edges();
	if (name.is_empty() || branch_here.is_empty()) {
		return;
	}
	const String hash = branch_here;
	branch_here = String();
	const Error err = repo->create_branch_at(name, hash);
	_report(err, "Create branch");
	refresh();
	if (err == OK) {
		_set_status(STATUS_SUCCESS, vformat("Created branch %s at %s (switch to it from the branch picker)", name, hash.left(7)));
	}
}

void GitDock::_build_branch_dialogs() {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();

	// A plain dialog (not _make_confirm): its own text label would be an empty row above ours.
	delete_branch_dialog = memnew(ConfirmationDialog);
	delete_branch_dialog->set_title("Delete Branch");
	delete_branch_dialog->connect("confirmed", callable_mp(this, &GitDock::_on_delete_branch_confirmed));
	add_child(delete_branch_dialog);
	delete_branch_label = memnew(Label);
	delete_branch_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	delete_branch_label->set_custom_minimum_size(Vector2(360 * scale, 0)); // Wraps at this width (gotcha 28).
	delete_branch_dialog->add_child(delete_branch_label);
}

// Asks before deleting p_branch, saying what it means: commits lost, or nothing lost.
void GitDock::_show_delete_branch_dialog(const String &p_branch) {
	pending_delete = p_branch;
	const Dictionary details = repo->get_branch_details(p_branch);
	const int unique = details["unique"];
	const String upstream = details["upstream"];
	String text;
	if (unique > 0) {
		text = vformat("Delete %s? It has %s that no other branch has. Deleting it loses %s.", p_branch, plural(unique, "commit", "commits"), unique == 1 ? String("it") : String("them"));
	} else if (unique == 0) {
		text = vformat("Delete %s? All of its commits are on other branches too, so nothing is lost.", p_branch);
	} else {
		text = vformat("Delete %s? Couldn't check whether it has commits no other branch has.", p_branch);
	}
	if (!upstream.is_empty()) {
		text += vformat(" The branch it follows, %s, stays.", upstream);
	}
	delete_branch_label->set_text(text);
	delete_branch_label->add_theme_color_override("font_color", unique != 0 ? get_theme_color("warning_color", "Editor") : get_theme_color("font_color", "Label"));
	delete_branch_dialog->set_ok_button_text(unique != 0 ? "Delete Anyway" : "Delete");
	delete_branch_dialog->popup_centered();
}

void GitDock::_on_delete_branch_confirmed() {
	if (pending_delete.is_empty()) {
		return;
	}
	const Error err = repo->delete_branch(pending_delete);
	_report(err, "Delete branch");
	refresh();
	if (err == OK) {
		_set_status(STATUS_SUCCESS, vformat("Deleted branch %s", pending_delete));
	}
}
