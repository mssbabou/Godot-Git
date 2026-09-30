// The Diff panel: shows the changes to one file as the script editor would show code, with
// added and removed lines tinted, old and new line numbers, and a unified or side-by-side view.
// This file builds it and picks the view; the views are in git_diff_dock_text.cpp,
// git_diff_dock_images.cpp and git_diff_dock_settings.cpp.

#include "editor/git_diff_dock.h"

#include <godot_cpp/classes/center_container.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_split_container.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/margin_container.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>

#include "editor/git_colors.h"
#include "editor/ui_text.h"

using namespace godot_git;

namespace {

EditorSettings *editor_settings() {
	return *EditorInterface::get_singleton()->get_editor_settings();
}

} // namespace

void GitDiffDock::_bind_methods() {
	ADD_SIGNAL(MethodInfo("open_requested", PropertyInfo(Variant::STRING, "path")));
}

GitDiffDock::GitDiffDock() {
	set_name("Diff");
	set_title("Diff");
	set_layout_key("GodotGitDiff");
	set_icon_name("VCSCommit");
	set_default_slot(DOCK_SLOT_BOTTOM);
	// Side by side needs width: the bottom panel, or a window of its own.
	set_available_layouts(DOCK_LAYOUT_HORIZONTAL | DOCK_LAYOUT_FLOATING);
	// Otherwise the bottom panel opens one line tall. Godot's own Version Control panel uses 300.
	set_custom_minimum_size(Vector2(0, 240 * EditorInterface::get_singleton()->get_editor_scale()));

	VBoxContainer *main_vb = memnew(VBoxContainer);
	add_child(main_vb);

	// Header: which file, which changes, how many lines; the view and Open on the right.
	// In a MarginContainer for its left inset (see _update_theme).
	MarginContainer *header_margin = memnew(MarginContainer);
	main_vb->add_child(header_margin);
	header = header_margin;
	HBoxContainer *header_hb = memnew(HBoxContainer);
	header_margin->add_child(header_hb);

	icon_rect = memnew(TextureRect);
	icon_rect->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	header_hb->add_child(icon_rect);

	name_label = memnew(Label);
	header_hb->add_child(name_label);

	folder_label = memnew(Label);
	folder_label->set_h_size_flags(SIZE_EXPAND_FILL);
	folder_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	header_hb->add_child(folder_label);

	source_label = memnew(Label);
	header_hb->add_child(source_label);

	added_label = memnew(Label);
	header_hb->add_child(added_label);
	removed_label = memnew(Label);
	header_hb->add_child(removed_label);

	view_select = memnew(OptionButton);
	view_select->add_item("Unified", VIEW_UNIFIED);
	view_select->add_item("Side by Side", VIEW_SPLIT);
	view_select->set_tooltip_text("Show the changes in one column, or the old and new version side by side.");
	view_select->connect("item_selected", callable_mp(this, &GitDiffDock::_on_view_selected));
	header_hb->add_child(view_select);

	open_button = memnew(Button);
	open_button->set_flat(true);
	open_button->set_text("Open");
	open_button->connect("pressed", callable_mp(this, &GitDiffDock::_on_open_pressed));
	header_hb->add_child(open_button);

	// The views. Only one of these is visible at a time.
	VBoxContainer *body = memnew(VBoxContainer);
	body->set_v_size_flags(SIZE_EXPAND_FILL);
	main_vb->add_child(body);

	MarginContainer *unified_box = memnew(MarginContainer);
	unified_box->set_v_size_flags(SIZE_EXPAND_FILL);
	body->add_child(unified_box);
	unified_view = unified_box;
	_make_pane(PANE_UNIFIED, unified_box, 2);

	HSplitContainer *split = memnew(HSplitContainer);
	split->set_v_size_flags(SIZE_EXPAND_FILL);
	body->add_child(split);
	split_view = split;
	_make_pane(PANE_OLD, split, 1);
	_make_pane(PANE_NEW, split, 1);
	// Both sides have the same number of rows (blank fillers opposite one-sided lines), so they
	// scroll together line for line.
	for (PaneIndex side : { PANE_OLD, PANE_NEW }) {
		panes[side].frame->set_h_size_flags(SIZE_EXPAND_FILL);
		panes[side].edit->get_v_scroll_bar()->connect("value_changed", callable_mp(this, &GitDiffDock::_on_scrolled).bind(side));
	}

	HBoxContainer *images = memnew(HBoxContainer);
	images->set_v_size_flags(SIZE_EXPAND_FILL);
	body->add_child(images);
	image_view = images;
	_make_image_side(0, images);
	_make_image_side(1, images);

	PanelContainer *settings_frame = memnew(PanelContainer);
	settings_frame->set_v_size_flags(SIZE_EXPAND_FILL);
	body->add_child(settings_frame);
	settings_view = settings_frame;
	settings_tree = _make_settings_tree(settings_frame);

	CenterContainer *center = memnew(CenterContainer);
	center->set_v_size_flags(SIZE_EXPAND_FILL);
	body->add_child(center);
	message_view = center;
	message_label = memnew(Label);
	message_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	center->add_child(message_label);

	// As tall as its rows, up to a limit (see _fit_companions), so the file's own view keeps the room.
	PanelContainer *companion_frame = memnew(PanelContainer);
	main_vb->add_child(companion_frame);
	companion_view = companion_frame;
	companion_scroll = memnew(ScrollContainer);
	companion_frame->add_child(companion_scroll);
	companion_tree = _make_settings_tree(companion_scroll);
	companion_tree->set_v_scroll_enabled(false); // Reports its full height; the ScrollContainer scrolls.
	companion_tree->set_h_size_flags(SIZE_EXPAND_FILL);
}

void GitDiffDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			const int view = editor_settings()->get_project_metadata("godot_git", "diff_view", (int)VIEW_UNIFIED);
			text_view = view == VIEW_SPLIT ? VIEW_SPLIT : VIEW_UNIFIED;
			// Again: at THEME_CHANGED the CodeEdit's code font size isn't final yet (gotcha 20),
			// and the gutter numbers came out smaller than the code.
			_update_theme();
			_render();
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			_update_theme();
			if (is_node_ready()) {
				_render(); // Row colors come from the theme.
			}
		} break;
	}
}

// The tint of an added or removed line, drawn over the code editor's background. Also used by
// the script editor's change preview (GitChangePreview), so both show changes in one red and green.
Color GitDiffDock::row_tint(bool p_added) {
	const Color color = EditorInterface::get_singleton()->get_editor_theme()->get_color(p_added ? "success_color" : "error_color", "Editor");
	return color * Color(1, 1, 1, 0.14);
}

void GitDiffDock::_update_theme() {
	theme.added = change_color(CHANGE_ADDED); // The +/- counts and signs; row tints below.
	theme.removed = change_color(CHANGE_REMOVED);
	theme.dim = get_theme_color("font_color", "Label") * Color(1, 1, 1, 0.55);
	theme.row_background[ROW_CONTEXT] = Color(0, 0, 0, 0);
	theme.row_background[ROW_ADDED] = row_tint(true);
	theme.row_background[ROW_REMOVED] = row_tint(false);
	theme.row_background[ROW_HEADER] = get_theme_color("accent_color", "Editor") * Color(1, 1, 1, 0.1);
	theme.row_background[ROW_FILLER] = get_theme_color("font_color", "Label") * Color(1, 1, 1, 0.03);

	CodeEdit *any_edit = panes[PANE_UNIFIED].edit;
	theme.line_number = any_edit->get_theme_color("line_number_color");
	theme.font = any_edit->get_theme_font("font");
	theme.checkerboard = get_theme_icon("GuiMiniCheckerboard", "EditorIcons");
	theme.font_size = any_edit->get_theme_font_size("font_size");
	theme.ascent = theme.font->get_ascent(theme.font_size);
	theme.height = theme.font->get_height(theme.font_size);
	theme.digit_width = theme.font->get_string_size("0", HORIZONTAL_ALIGNMENT_LEFT, -1, theme.font_size).x;
	theme.scale = EditorInterface::get_singleton()->get_editor_scale();

	open_button->set_button_icon(get_theme_icon("Load", "EditorIcons"));
	// The file icon starts as far in as the captions below it (a Label's padding) and as "Open"
	// ends on the right (the button's padding); flush with the edge, it looked lopsided.
	const Ref<StyleBox> label_box = get_theme_stylebox("normal", "Label");
	const int inset = label_box.is_valid() ? (int)label_box->get_margin(SIDE_LEFT) : 0;
	for (const char *side : { "margin_top", "margin_right", "margin_bottom" }) {
		header->add_theme_constant_override(side, 0);
	}
	header->add_theme_constant_override("margin_left", inset);
	for (ImageSide &side : image_sides) {
		side.frame->add_theme_stylebox_override("panel", get_theme_stylebox("read_only", "CodeEdit"));
		side.caption->add_theme_color_override("font_color", theme.dim);
		side.note->add_theme_color_override("font_color", theme.dim);
	}
	Ref<StyleBoxEmpty> no_box;
	no_box.instantiate();
	for (Control *frame : { settings_view, companion_view }) {
		frame->add_theme_stylebox_override("panel", get_theme_stylebox("read_only", "CodeEdit"));
	}
	// Nothing in these lists can be clicked, so nothing highlights: a Tree draws its hover box
	// over rows even when they can't be selected.
	for (Tree *tree : { settings_tree, companion_tree }) {
		for (const char *name : { "panel", "focus", "hovered", "hovered_dimmed", "hovered_selected", "hovered_selected_focus", "selected", "selected_focus", "cursor", "cursor_unfocused" }) {
			tree->add_theme_stylebox_override(name, no_box);
		}
	}
	for (Label *label : { folder_label, source_label, message_label }) {
		label->add_theme_color_override("font_color", theme.dim);
	}
	added_label->add_theme_color_override("font_color", theme.added);
	removed_label->add_theme_color_override("font_color", theme.removed);

	// A TextEdit clips its lines to its whole rect, padding included, so a row scrolled half out
	// of view was drawn into the padding, up to the rounded edge (very visible with tinted rows).
	// The frame draws the code editor's background and padding instead, and the CodeEdit has
	// none, so rows are cut off at the inner edge.
	const Ref<StyleBox> code_box = get_theme_stylebox("read_only", "CodeEdit");
	Ref<StyleBoxEmpty> none;
	none.instantiate();
	for (Pane &pane : panes) {
		pane.frame->add_theme_stylebox_override("panel", code_box);
		for (const char *name : { "normal", "read_only", "focus" }) {
			pane.edit->add_theme_stylebox_override(name, none);
		}
		// Read-only text is dimmed by default; this is for reading.
		pane.edit->add_theme_color_override("font_readonly_color", pane.edit->get_theme_color("font_color"));
		// The +/− column. The number columns' widths depend on the diff (see _fill_pane).
		pane.edit->set_gutter_width(pane.first_gutter + pane.number_gutters, (int)(theme.digit_width * 2 + 6 * theme.scale));
	}
}

void GitDiffDock::set_diff(const Dictionary &p_diff, const String &p_source, const Ref<Texture2D> &p_icon) {
	if (p_source == source && p_diff == diff) {
		return;
	}
	diff = p_diff;
	source = p_source;
	file_icon = p_icon;
	_render();
}

// Scrolls the text view to the new file's line p_new_line (1-based; the first row at or after it,
// else the last), a few rows of context above it. Show in Diff from the script editor uses it.
void GitDiffDock::scroll_to_line(int p_new_line) {
	const bool split = !panes[PANE_NEW].new_numbers.is_empty();
	Pane &pane = panes[split ? PANE_NEW : PANE_UNIFIED];
	if (pane.new_numbers.is_empty()) {
		return;
	}
	int row = pane.new_numbers.size() - 1;
	for (int i = 0; i < pane.new_numbers.size(); i++) {
		if (pane.new_numbers[i] >= p_new_line) {
			row = i;
			break;
		}
	}
	// Deferred: the rows were just filled, and a CodeEdit scrolls by its laid-out size.
	Callable(pane.edit, "set_line_as_first_visible").call_deferred(MAX(row - 3, 0), 0);
}

void GitDiffDock::_render() {
	_update_header();

	const View view = _current_view();
	const bool own_view = view == VIEW_IMAGE || view == VIEW_SETTINGS;
	const String text = own_view ? String() : _empty_text();
	message_label->set_text(text);
	message_view->set_visible(!text.is_empty());
	const bool split = view == VIEW_SPLIT;
	unified_view->set_visible(text.is_empty() && view == VIEW_UNIFIED);
	split_view->set_visible(text.is_empty() && split);
	image_view->set_visible(view == VIEW_IMAGE);
	settings_view->set_visible(view == VIEW_SETTINGS);
	_show_images();
	_show_settings();

	// Only the visible view holds lines; the others are emptied.
	Rows unified, old_side, new_side;
	if (text.is_empty() && !own_view) {
		_build_rows(unified, old_side, new_side);
	}
	_fill_pane(panes[PANE_UNIFIED], split ? Rows() : unified);
	_fill_pane(panes[PANE_OLD], split ? old_side : Rows());
	_fill_pane(panes[PANE_NEW], split ? new_side : Rows());
}

// Images open as images and settings files setting by setting; both can be switched to their
// text diff when they have one (an SVG is also text).
GitDiffDock::View GitDiffDock::_current_view() const {
	const bool has_lines = diff.get("kind", String()) == "text" && !Array(diff.get("hunks", Array())).is_empty();
	if (diff.has("image_new") && (!has_lines || !as_text)) {
		return VIEW_IMAGE;
	}
	if (diff.has("settings") && (!has_lines || !as_text)) {
		return VIEW_SETTINGS;
	}
	return text_view;
}

void GitDiffDock::_update_header() {
	const String path = diff.get("path", String());
	const String name = path.get_file();
	const String kind = diff.get("kind", String());
	header->set_visible(!path.is_empty());
	icon_rect->set_texture(file_icon);
	name_label->set_text(name);
	folder_label->set_text(path.get_base_dir());
	const String old_path = diff.get("old_path", path);
	source_label->set_text(old_path != path ? vformat("%s, renamed from %s", source, old_path.get_file()) : source);

	const int added = diff.get("added", 0);
	const int removed = diff.get("removed", 0);
	added_label->set_visible(kind == "text" && added > 0);
	removed_label->set_visible(kind == "text" && removed > 0);
	added_label->set_text(vformat("+%d", added));
	removed_label->set_text(vformat("%s%d", minus(), removed));
	added_label->set_tooltip_text(plural(added, "line added", "lines added"));
	removed_label->set_tooltip_text(plural(removed, "line removed", "lines removed"));

	// The views this file has: images as images, text as unified or side by side.
	const bool has_lines = kind == "text" && !Array(diff.get("hunks", Array())).is_empty();
	view_select->clear();
	if (diff.has("image_new") && has_lines) {
		view_select->add_item("Image", VIEW_IMAGE);
	}
	if (diff.has("settings") && has_lines) {
		view_select->add_item("Settings", VIEW_SETTINGS);
	}
	if (has_lines) {
		view_select->add_item("Unified", VIEW_UNIFIED);
		view_select->add_item("Side by Side", VIEW_SPLIT);
	}
	view_select->select(view_select->get_item_index(_current_view()));
	view_select->set_visible(view_select->get_item_count() > 1);
	const bool deleted = diff.get("status", String()) == "deleted";
	open_button->set_disabled(deleted);
	open_button->set_tooltip_text(deleted ? vformat("%s is deleted, so there's nothing to open.", name) : String("Open the file"));
}

// What to say instead of showing lines, or "" when there are lines to show.
String GitDiffDock::_empty_text() const {
	if (diff.is_empty()) {
		return "Click a file in the Git dock to see its changes here.";
	}
	const String name = String(diff.get("path", String())).get_file();
	const String kind = diff.get("kind", String());
	const String status = diff.get("status", String());
	if (kind == "unchanged") {
		return vformat("No %s changes to %s anymore.", source.to_lower(), name);
	}
	if (kind == "binary") {
		return vformat("%s is a binary file, so there are no lines to compare.", name);
	}
	if (kind == "too_large") {
		return vformat("%s is larger than 2 MB, too large to compare line by line.", name);
	}
	if (kind == "lfs") {
		return vformat("%s is stored with Git LFS, so its lines aren't compared.", name);
	}
	if (!Array(diff.get("hunks", Array())).is_empty()) {
		return String();
	}
	if (status == "renamed") {
		return vformat("Renamed from %s, with the same content.", diff.get("old_path", String()));
	}
	if (status == "new" || status == "untracked") {
		return vformat("%s is a new, empty file.", name);
	}
	if (status == "deleted") {
		return vformat("%s was an empty file and is deleted.", name);
	}
	return "The content is the same; only the file's permissions changed.";
}

void GitDiffDock::_on_view_selected(int p_index) {
	const View view = (View)view_select->get_item_id(p_index);
	const bool own_view = view == VIEW_IMAGE || view == VIEW_SETTINGS;
	if (diff.has("image_new") || diff.has("settings")) {
		as_text = !own_view; // Only a choice made on such a file counts for them.
	}
	if (!own_view) {
		text_view = view;
		editor_settings()->set_project_metadata("godot_git", "diff_view", (int)view);
	}
	_render();
}

void GitDiffDock::_on_open_pressed() {
	const String path = diff.get("path", String());
	if (!path.is_empty()) {
		emit_signal("open_requested", path);
	}
}
