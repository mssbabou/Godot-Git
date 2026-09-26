// The dock's lists: Staged Changes, Changes and History, how their rows are drawn, and what
// clicking, hovering and right-clicking them does.

#include "editor/git_dock.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/text_line.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include "editor/git_diff_dock.h"
#include "editor/ui_text.h"

using namespace godot_git;

namespace {

// File rows are a single cell (status letter, icon, name, folder, hover buttons) so they highlight as one unit.
// The letter leads, so the buttons end flush right and line up with the section header's button.
enum FileColumn {
	COLUMN_NAME,
	FILE_COLUMN_COUNT,
};

} // namespace

void GitDock::_build_lists(Control *p_parent) {
	// Staged changes, changes, history: collapsible sections, each exactly as tall as its
	// content, in one shared scroll area.
	ScrollContainer *scroll = memnew(ScrollContainer);
	scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	p_parent->add_child(scroll);

	VBoxContainer *panes = memnew(VBoxContainer);
	panes->set_h_size_flags(SIZE_EXPAND_FILL);
	scroll->add_child(panes);

	_make_file_pane(staged_pane, panes, "Staged Changes", true);
	_make_file_pane(changes_pane, panes, "Changes", false);

	history_pane = memnew(FoldableContainer);
	history_pane->set_title("History");
	panes->add_child(history_pane);

	history_tree = memnew(Tree);
	history_tree->set_hide_root(true);
	history_tree->set_select_mode(Tree::SELECT_ROW);
	history_tree->set_allow_rmb_select(true);
	history_tree->set_v_scroll_enabled(false); // Grow to fit; the outer ScrollContainer scrolls.
	history_tree->set_h_scroll_enabled(false);
	history_tree->set_columns(2);
	history_tree->set_column_expand(0, true);
	history_tree->set_column_clip_content(0, true);
	history_tree->set_column_expand(1, false);
	history_tree->connect("item_mouse_selected", callable_mp(this, &GitDock::_on_tree_mouse_selected).bind(history_tree));
	history_empty = _make_body(history_pane, history_tree);
}

void GitDock::_make_file_pane(FilePane &r_pane, Control *p_parent, const String &p_title, bool p_staged) {
	r_pane.staged = p_staged;

	r_pane.container = memnew(FoldableContainer);
	r_pane.container->set_title(p_title);
	p_parent->add_child(r_pane.container);

	// Header: file count, line totals and the "all" actions. The actions sit in the same order
	// and exact positions as the row buttons (discard, then stage/unstage); see _align_header_buttons.
	r_pane.count = memnew(Label);
	r_pane.added = memnew(Label);
	r_pane.removed = memnew(Label);

	r_pane.buttons_margin = memnew(MarginContainer);
	r_pane.buttons = memnew(HBoxContainer);
	r_pane.buttons_margin->add_child(r_pane.buttons);
	if (!p_staged) {
		r_pane.discard = memnew(Button);
		r_pane.discard->set_tooltip_text("Discard all changes");
		r_pane.discard->connect("pressed", callable_mp(this, &GitDock::_on_more_menu_id).bind(MORE_DISCARD_ALL));
		r_pane.buttons->add_child(r_pane.discard);
	}
	r_pane.action = memnew(Button);
	r_pane.action->set_tooltip_text(p_staged ? "Unstage all" : "Stage all");
	r_pane.action->connect("pressed", callable_mp(this, &GitDock::_on_more_menu_id).bind(p_staged ? MORE_UNSTAGE_ALL : MORE_STAGE_ALL));
	r_pane.buttons->add_child(r_pane.action);

	for (Control *control : { (Control *)r_pane.count, (Control *)r_pane.added, (Control *)r_pane.removed, (Control *)r_pane.buttons_margin }) {
		control->set_v_size_flags(SIZE_SHRINK_CENTER);
		r_pane.container->add_title_bar_control(control);
	}
	r_pane.container->connect("sort_children", callable_mp(this, &GitDock::_queue_align_header_buttons));

	Tree *tree = memnew(Tree);
	tree->set_hide_root(true);
	tree->set_select_mode(Tree::SELECT_MULTI);
	tree->set_allow_rmb_select(true);
	tree->set_columns(FILE_COLUMN_COUNT);
	tree->set_v_scroll_enabled(false); // Grow to fit; the outer ScrollContainer scrolls.
	tree->set_h_scroll_enabled(false);
	tree->set_column_expand(COLUMN_NAME, true);
	tree->set_column_clip_content(COLUMN_NAME, true);
	tree->add_theme_constant_override("item_margin", 0); // Flat list: no hierarchy indent.
	tree->connect("button_clicked", callable_mp(this, &GitDock::_on_tree_button_clicked));
	tree->connect("item_activated", callable_mp(this, &GitDock::_on_file_activated).bind(tree));
	tree->connect("item_mouse_selected", callable_mp(this, &GitDock::_on_tree_mouse_selected).bind(tree));
	tree->connect("multi_selected", callable_mp(this, &GitDock::_on_file_multi_selected).bind(tree));
	tree->connect("gui_input", callable_mp(this, &GitDock::_on_tree_gui_input).bind(tree));
	tree->connect("mouse_exited", callable_mp(this, &GitDock::_on_tree_mouse_exited).bind(tree));
	r_pane.tree = tree;
	r_pane.empty_label = _make_body(r_pane.container, tree);
}

// A section's content: the tree, and a plain label shown instead when the tree is empty.
Label *GitDock::_make_body(Control *p_section, Tree *p_tree) {
	VBoxContainer *body = memnew(VBoxContainer);
	body->add_theme_constant_override("separation", 0);
	p_section->add_child(body);
	body->add_child(p_tree);

	MarginContainer *empty_margin = memnew(MarginContainer);
	empty_margin->hide();
	body->add_child(empty_margin);
	Label *label = memnew(Label);
	empty_margin->add_child(label);
	return label;
}

void GitDock::_fill_file_pane(FilePane &p_pane, const Array &p_status, const Dictionary &p_stats) {
	Tree *tree = p_pane.tree;
	tree->clear();
	p_pane.hovered_item = 0;
	TreeItem *root = tree->create_item();

	const String key = p_pane.staged ? "index" : "worktree";
	const Color dim = _dim_color();
	const Callable draw_row = callable_mp(this, &GitDock::_draw_file_row);

	int total_added = 0, total_removed = 0, files = 0;
	if (!p_pane.staged) {
		unstaged_paths.clear();
	}

	for (int i = 0; i < p_status.size(); i++) {
		const Dictionary entry = p_status[i];
		const String state = entry[key];
		if (state.is_empty()) {
			continue;
		}
		const String path = entry["path"];
		files++;
		if (!p_pane.staged) {
			unstaged_paths.push_back(path);
		}

		// Line counts only show as totals in the section header.
		const Vector2i stats = p_stats.get(path, Vector2i());
		if (stats.x > 0) {
			total_added += stats.x;
		}
		if (stats.y > 0) {
			total_removed += stats.y;
		}

		TreeItem *item = tree->create_item(root);
		item->set_metadata(COLUMN_NAME, path);
		item->set_meta("git_state", state);
		item->set_meta("git_icon", _file_icon(path));

		// The whole row is one cell that _draw_file_row paints (status letter, icon, name, folder),
		// so hover and selection highlight it as one unit. The name stays as the cell's text, made
		// invisible, so row height, type-to-search and accessibility still work.
		item->set_cell_mode(COLUMN_NAME, TreeItem::CELL_MODE_CUSTOM);
		item->set_custom_draw_callback(COLUMN_NAME, draw_row);
		item->set_text(COLUMN_NAME, path.get_file());
		item->set_custom_color(COLUMN_NAME, Color(0, 0, 0, 0));
		// The row itself stays calm (totals are in the header); the tooltip has this file's counts.
		String lines;
		if (stats.x < 0) {
			lines = "binary or too large to count lines";
		} else if (stats.x > 0 || stats.y > 0) {
			lines = vformat("+%d %s%d", stats.x, minus(), stats.y);
		}
		item->set_tooltip_text(COLUMN_NAME, lines.is_empty() ? vformat("%s\n%s", path, status_name(state)) : vformat(String::utf8("%s\n%s · %s"), path, status_name(state), lines));
	}

	tree->set_visible(files > 0);
	p_pane.empty_label->get_parent_control()->set_visible(files == 0);
	p_pane.empty_label->set_text(p_pane.staged ? "Nothing staged." : "No changes.");

	p_pane.file_count = files;

	// Header: "27  +1204 −35  [⊖]".
	p_pane.count->set_text(files > 0 ? itos(files) : String());
	p_pane.added->set_text(files > 0 ? vformat("+%d", total_added) : String());
	p_pane.removed->set_text(files > 0 ? vformat("%s%d", minus(), total_removed) : String());
	p_pane.added->set_tooltip_text(plural(total_added, "line added", "lines added"));
	p_pane.removed->set_tooltip_text(plural(total_removed, "line removed", "lines removed"));
	p_pane.action->set_disabled(files == 0);
	if (p_pane.discard) {
		p_pane.discard->set_disabled(files == 0);
	}
}

void GitDock::_queue_align_header_buttons() {
	if (!align_queued) {
		align_queued = true;
		callable_mp(this, &GitDock::_align_header_buttons).call_deferred();
	}
}

// Shifts each header's "all" buttons so they end exactly where the Tree draws the rows' buttons
// (the right edge of the file column), whatever the theme margins and editor scale are.
void GitDock::_align_header_buttons() {
	align_queued = false;
	for (FilePane *pane : { &staged_pane, &changes_pane }) {
		if (!pane->container->is_visible_in_tree()) {
			continue;
		}
		Tree *tree = pane->tree;
		TreeItem *first = tree->get_root() ? tree->get_root()->get_first_child() : nullptr;
		if (!first || !tree->is_visible()) {
			continue; // Nothing to line up with while the section is empty.
		}
		const float rows_right = tree->get_global_position().x + tree->get_item_area_rect(first, COLUMN_NAME).get_end().x;
		const float buttons_right = pane->buttons->get_global_position().x + pane->buttons->get_size().x;
		const int margin = pane->buttons_margin->get_theme_constant("margin_right");
		const int aligned = MAX(0, (int)Math::round(margin + buttons_right - rows_right));
		if (aligned != margin) {
			pane->buttons_margin->add_theme_constant_override("margin_right", aligned);
		}
	}
}

// Paints a file row: status letter, file icon, name, and the folder dimmed after the name
// (like VS Code), each trimmed with an ellipsis when space runs out. p_rect is the cell's
// content area, which the Tree has already shrunk to leave room for the hover buttons.
void GitDock::_draw_file_row(TreeItem *p_item, const Rect2 &p_rect) {
	Tree *tree = p_item->get_tree();
	const FilePane *pane = _pane_for_tree(tree);
	if (!pane) {
		return;
	}
	const Ref<Font> font = tree->get_theme_font("font");
	const int font_size = tree->get_theme_font_size("font_size");
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	// The Tree's layer for custom drawing: above the hover/selection background, clipped to the rows.
	const RID canvas = tree->get_custom_drawing_canvas_item();
	const String path = p_item->get_metadata(COLUMN_NAME);
	const String state = p_item->get_meta("git_state", String());
	const float right = p_rect.get_end().x;

	// Draws one piece of text starting at p_x, vertically centered; returns where it ends.
	auto draw_text = [&](const String &p_text, float p_x, const Color &p_color) -> float {
		if (right - p_x < font_size) {
			return right; // Not enough room to show anything useful.
		}
		Ref<TextLine> line;
		line.instantiate();
		line->add_string(p_text, font, font_size);
		line->set_width(right - p_x);
		line->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		line->draw(canvas, Vector2(p_x, p_rect.position.y + (p_rect.size.y - line->get_size().y) / 2), p_color);
		return p_x + MIN(line->get_size().x, right - p_x);
	};

	// Status letter in a fixed-width slot, so icons and names line up down the list.
	float x = p_rect.position.x;
	const float letter_width = font->get_string_size("M", HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
	const String letter = status_letter(state);
	const float letter_x = x + (letter_width - font->get_string_size(letter, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x) / 2;
	draw_text(letter, letter_x, _status_color(state));
	x += letter_width + 6 * scale;

	const bool deleted = state == "deleted";
	const Ref<Texture2D> icon = p_item->get_meta("git_icon", Variant());
	if (icon.is_valid()) {
		icon->draw(canvas, Vector2(x, p_rect.position.y + (p_rect.size.y - icon->get_height()) / 2), Color(1, 1, 1, deleted ? 0.5 : 1));
		x += icon->get_width() + tree->get_theme_constant("icon_h_separation");
	}

	// Name in the Tree's own text colors, so selection and hover look native.
	Color name_color = tree->get_theme_color("font_color");
	if (p_item->is_selected(COLUMN_NAME)) {
		name_color = tree->get_theme_color("font_selected_color");
	} else if (p_item->get_instance_id() == pane->hovered_item) {
		name_color = tree->get_theme_color("font_hovered_color");
	}
	if (deleted) {
		name_color.a *= 0.5;
	}
	x = draw_text(path.get_file(), x, name_color);

	const String folder = path.get_base_dir();
	if (!folder.is_empty()) {
		draw_text(folder, x + 6 * scale, _dim_color());
	}
}

void GitDock::_fill_history() {
	history_tree->clear();
	TreeItem *root = history_tree->create_item();

	const Array commits = repo->get_commits(50);
	const Color dim = _dim_color();
	has_commits = !commits.is_empty();
	// For Amend. "unpushed" means on no remote-tracking branch; without remotes it's never set.
	const Dictionary last = commits.is_empty() ? Dictionary() : Dictionary(commits[0]);
	last_commit_id = last.get("id", String());
	last_commit_message = last.get("message", String());
	last_commit_pushed = has_commits && bool(sync_status.get("has_remotes", false)) && !bool(last.get("unpushed", false));

	history_tree->set_visible(!commits.is_empty());
	history_empty->get_parent_control()->set_visible(commits.is_empty());
	history_empty->set_text("No commits yet.");
	if (commits.is_empty()) {
		return;
	}

	const int64_t bias_minutes = Dictionary(Time::get_singleton()->get_time_zone_from_system()).get("bias", 0);
	const Ref<Texture2D> icon = get_theme_icon("VCSCommit", "EditorIcons");
	const Color accent = get_theme_color("accent_color", "Editor");

	for (int i = 0; i < commits.size(); i++) {
		const Dictionary commit = commits[i];
		const int64_t time = commit["time"];
		const bool unpushed = commit["unpushed"];
		const String date = Time::get_singleton()->get_datetime_string_from_unix_time(time + bias_minutes * 60, true);

		TreeItem *item = history_tree->create_item(root);
		item->set_metadata(0, commit);
		item->set_icon(0, icon);
		if (unpushed) {
			item->set_icon_modulate(0, accent);
		}
		item->set_text(0, commit["summary"]);
		item->set_tooltip_text(0, vformat(String::utf8("%s\n\n%s · %s · %s%s"), commit["message"], commit["id"], commit["author"], date, unpushed ? "\nNot pushed yet" : ""));
		const String when = relative_time(time);
		item->set_text(1, when);
		item->set_text_overrun_behavior(1, TextServer::OVERRUN_NO_TRIMMING);
		item->set_custom_color(1, dim);
		item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
		item->set_tooltip_text(1, date);
	}
}

GitDock::FilePane *GitDock::_pane_for_tree(Object *p_tree) {
	if (p_tree == staged_pane.tree) {
		return &staged_pane;
	}
	if (p_tree == changes_pane.tree) {
		return &changes_pane;
	}
	return nullptr;
}

PackedStringArray GitDock::_selected_paths(Tree *p_tree) const {
	PackedStringArray paths;
	for (TreeItem *item = p_tree->get_next_selected(nullptr); item; item = p_tree->get_next_selected(item)) {
		const String path = item->get_metadata(COLUMN_NAME);
		if (!path.is_empty()) {
			paths.push_back(path);
		}
	}
	return paths;
}

// Row buttons only show on the row under the mouse, to keep the list calm.
void GitDock::_set_hovered(FilePane &p_pane, TreeItem *p_item) {
	TreeItem *previous = Object::cast_to<TreeItem>(ObjectDB::get_instance(p_pane.hovered_item));
	if (previous == p_item) {
		return;
	}
	if (previous) {
		previous->clear_buttons();
	}
	p_pane.hovered_item = 0;
	if (!p_item || p_item->get_metadata(COLUMN_NAME).get_type() != Variant::STRING) {
		return;
	}

	if (p_pane.staged) {
		p_item->add_button(COLUMN_NAME, get_theme_icon("ZoomLess", "EditorIcons"), BUTTON_UNSTAGE, false, "Unstage");
	} else {
		p_item->add_button(COLUMN_NAME, get_theme_icon("UndoRedo", "EditorIcons"), BUTTON_DISCARD, false, "Discard changes");
		p_item->add_button(COLUMN_NAME, get_theme_icon("ZoomMore", "EditorIcons"), BUTTON_STAGE, false, "Stage");
	}
	p_pane.hovered_item = p_item->get_instance_id();
}

void GitDock::_on_tree_gui_input(const Ref<InputEvent> &p_event, Object *p_tree) {
	Ref<InputEventMouseMotion> motion = p_event;
	FilePane *pane = _pane_for_tree(p_tree);
	if (motion.is_valid() && pane) {
		_set_hovered(*pane, pane->tree->get_item_at_position(motion->get_position()));
	}
}

void GitDock::_on_tree_mouse_exited(Object *p_tree) {
	FilePane *pane = _pane_for_tree(p_tree);
	if (pane) {
		_set_hovered(*pane, nullptr);
	}
}

void GitDock::_on_tree_button_clicked(TreeItem *p_item, int p_column, int p_id, int p_mouse_button) {
	const String path = p_item->get_metadata(COLUMN_NAME);
	PackedStringArray paths;
	paths.push_back(path);

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

void GitDock::_on_tree_mouse_selected(const Vector2 &p_position, int p_mouse_button, Object *p_tree) {
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

	if (tree == history_tree) {
		if (!tree->get_selected() || tree->get_selected()->get_metadata(0).get_type() != Variant::DICTIONARY) {
			return;
		}
		context_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), "Copy Commit Hash", MENU_COPY_HASH);
		context_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), "Copy Commit Message", MENU_COPY_MESSAGE);
	} else {
		const FilePane *pane = _pane_for_tree(tree);
		const PackedStringArray paths = _selected_paths(tree);
		if (!pane || paths.is_empty()) {
			return;
		}
		const bool single = paths.size() == 1;
		const String count = single ? String() : vformat(" (%d)", paths.size());

		if (single) {
			context_menu->add_icon_item(get_theme_icon("Load", "EditorIcons"), "Open", MENU_OPEN);
			context_menu->add_separator();
		}
		if (pane->staged) {
			context_menu->add_icon_item(get_theme_icon("ZoomLess", "EditorIcons"), "Unstage" + count, MENU_UNSTAGE);
		} else {
			context_menu->add_icon_item(get_theme_icon("ZoomMore", "EditorIcons"), "Stage" + count, MENU_STAGE);
			context_menu->add_icon_item(get_theme_icon("UndoRedo", "EditorIcons"), "Discard Changes..." + count, MENU_DISCARD);
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
	}

	context_menu->set_position(Vector2i(tree->get_screen_position() + p_position));
	context_menu->reset_size();
	context_menu->popup();
}

void GitDock::_on_context_menu_id(int p_id) {
	if (!context_tree) {
		return;
	}
	const PackedStringArray paths = context_tree == history_tree ? PackedStringArray() : _selected_paths(context_tree);

	switch (p_id) {
		case MENU_OPEN: {
			if (!paths.is_empty()) {
				_open_path(paths[0]);
			}
		} break;
		case MENU_STAGE: {
			_stage_paths(paths, true);
		} break;
		case MENU_UNSTAGE: {
			_stage_paths(paths, false);
		} break;
		case MENU_DISCARD: {
			_confirm_discard(paths);
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
	_show_diff(p_item->get_metadata(COLUMN_NAME), pane->staged, false);
}

void GitDock::set_diff_dock(GitDiffDock *p_dock) {
	diff_dock = p_dock;
	diff_dock->connect("open_requested", callable_mp(this, &GitDock::_open_path));
}

// p_focus: also bring the Diff panel up (a click), not just update it (keyboard, right-click).
void GitDock::_show_diff(const String &p_path, bool p_staged, bool p_focus) {
	diff_path = p_path;
	diff_staged = p_staged;
	_update_diff();
	if (p_focus && diff_dock) {
		diff_dock->make_visible();
	}
}

// Brings the Diff panel up to date after a refresh. A file that was staged or unstaged meanwhile
// is followed to its other list, so staging the file you're looking at keeps it on screen.
void GitDock::_update_diff() {
	if (!diff_dock || diff_path.is_empty() || repo.is_null() || !repo->is_open()) {
		return;
	}
	Dictionary diff = repo->get_diff(diff_path, diff_staged);
	if (diff.get("kind", String()) == "unchanged") {
		const Dictionary other = repo->get_diff(diff_path, !diff_staged);
		if (other.get("kind", String()) != "unchanged") {
			diff_staged = !diff_staged;
			diff = other;
		}
	}
	diff_dock->set_diff(diff, diff_staged ? "Staged" : "Unstaged", _file_icon(diff_path));
	_select_diff_row();
}

// Marks the file the Diff panel shows in its list (the lists are rebuilt on every refresh).
void GitDock::_select_diff_row() {
	FilePane &pane = diff_staged ? staged_pane : changes_pane;
	TreeItem *root = pane.tree->get_root();
	for (TreeItem *item = root ? root->get_first_child() : nullptr; item; item = item->get_next()) {
		if (item->get_metadata(COLUMN_NAME).get_type() == Variant::STRING && String(item->get_metadata(COLUMN_NAME)) == diff_path) {
			if (!pane.tree->get_next_selected(nullptr)) {
				item->select(COLUMN_NAME); // Doesn't emit multi_selected, so it can't loop back here.
			}
			return;
		}
	}
}
