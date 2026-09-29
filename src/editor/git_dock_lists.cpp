// The dock's lists: the Staged Changes and Changes sections, their line counts and header
// buttons, and how a file row is drawn (History is in git_dock_history.cpp, what rows do on
// hover and click in git_dock_rows.cpp).

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/text_line.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include "editor/git_diff_dock.h"
#include "editor/git_dock_util.h"
#include "editor/ui_text.h"

using namespace godot_git;

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
	history_tree->connect("item_selected", callable_mp(this, &GitDock::_on_history_item_selected));
	history_tree->connect("item_collapsed", callable_mp(this, &GitDock::_on_history_item_collapsed));
	history_empty = _make_body(history_pane, history_tree);

	stats_slow_timer = memnew(Timer);
	stats_slow_timer->set_one_shot(true);
	stats_slow_timer->connect("timeout", callable_mp(this, &GitDock::_on_line_stats_slow));
	add_child(stats_slow_timer);
}

void GitDock::_make_file_pane(FilePane &r_pane, Control *p_parent, const String &p_title, bool p_staged) {
	r_pane.staged = p_staged;

	r_pane.title = p_title;
	r_pane.container = memnew(FoldableContainer);
	r_pane.container->set_title(p_title);
	p_parent->add_child(r_pane.container);

	// Header: line totals and the "all" actions (the file count is in the title: "Changes (9)").
	// The actions sit in the same order and exact positions as the row buttons (discard, then
	// stage/unstage); see _align_header_buttons.
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

	for (Control *control : { (Control *)r_pane.added, (Control *)r_pane.removed, (Control *)r_pane.buttons_margin }) {
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
	tree->connect("item_activated", callable_mp(this, &GitDock::_on_file_activated).bind(tree));
	tree->connect("item_mouse_selected", callable_mp(this, &GitDock::_on_tree_mouse_selected).bind(tree));
	tree->connect("multi_selected", callable_mp(this, &GitDock::_on_file_multi_selected).bind(tree));
	tree->connect("gui_input", callable_mp(this, &GitDock::_on_tree_gui_input).bind(tree));
	tree->connect("mouse_exited", callable_mp(this, &GitDock::_on_tree_mouse_exited).bind(tree));
	r_pane.tree = tree;
	r_pane.empty_label = _make_body(r_pane.container, tree);
}

// Rounds a section's corners like the editor's other panels (the theme's corner radius): the
// header's top corners and the content's bottom corners, all four while folded. The editor theme
// gives FoldableContainer square ones. Re-done on every theme change, from the theme's own styles.
void GitDock::_round_section(FoldableContainer *p_section) {
	const int radius = Math::round((int)EditorInterface::get_singleton()->get_editor_settings()->get_setting("interface/theme/corner_radius") * EditorInterface::get_singleton()->get_editor_scale());
	auto rounded = [&](const char *p_name, bool p_top, bool p_bottom) {
		const Ref<StyleBoxFlat> style = get_theme_stylebox(p_name, "FoldableContainer");
		if (style.is_null()) {
			return; // Some other kind of style: left as the theme has it.
		}
		Ref<StyleBoxFlat> copy = style->duplicate();
		copy->set_corner_radius(CORNER_TOP_LEFT, p_top ? radius : 0);
		copy->set_corner_radius(CORNER_TOP_RIGHT, p_top ? radius : 0);
		copy->set_corner_radius(CORNER_BOTTOM_LEFT, p_bottom ? radius : 0);
		copy->set_corner_radius(CORNER_BOTTOM_RIGHT, p_bottom ? radius : 0);
		// The theme's square styles come with the lowest corner detail (it's derived from their
		// corner width, 0): one straight segment per corner, which draws a rounded corner as a
		// chamfer. Same formula the editor uses for its rounded styles (make_flat_stylebox).
		copy->set_corner_detail(MAX(1, (int)Math::ceil(0.8 * radius)));
		copy->set_anti_aliased(true);
		p_section->add_theme_stylebox_override(p_name, copy);
	};
	rounded("title_panel", true, false);
	rounded("title_hover_panel", true, false);
	rounded("title_collapsed_panel", true, true);
	rounded("title_collapsed_hover_panel", true, true);
	rounded("panel", false, true);
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

void GitDock::_fill_file_pane(FilePane &p_pane, const Array &p_status) {
	Tree *tree = p_pane.tree;
	tree->clear();
	p_pane.hovered_item = 0;
	p_pane.hovered_button = -1;
	TreeItem *root = tree->create_item();

	const String key = p_pane.staged ? "index" : "worktree";
	const Callable draw_row = callable_mp(this, &GitDock::_draw_file_row);

	int files = 0;
	int rows = 0;
	if (!p_pane.staged) {
		unstaged_paths.clear();
	}

	// Godot's companion files (player.gd.uid, coin.png.import) go with their file: when both are
	// in this list, the companion gets no row of its own; the file's row says "+uid" / "+import"
	// and its actions include it. A companion whose file didn't change stays a row: then it's the
	// change (new import settings, say). Without this, adding assets filled the list with them.
	HashSet<String> listed;
	for (int i = 0; i < p_status.size(); i++) {
		const Dictionary entry = p_status[i];
		if (!String(entry[key]).is_empty()) {
			listed.insert(entry["path"]);
		}
	}
	Dictionary companions; // File -> PackedStringArray of its companions in this list.
	for (const String &path : listed) {
		const String owner = companion_owner(path);
		if (!owner.is_empty() && listed.has(owner)) {
			PackedStringArray of = companions.get(owner, PackedStringArray());
			of.push_back(path);
			companions[owner] = of;
		}
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
		const String owner = companion_owner(path);
		if (!owner.is_empty() && listed.has(owner)) {
			continue; // Shown on its file's row.
		}
		rows++;

		TreeItem *item = tree->create_item(root);
		if (companions.has(path)) {
			item->set_meta("git_companions", companions[path]);
		}
		item->set_metadata(COLUMN_NAME, path);
		item->set_meta("git_path", path);
		item->set_meta("git_state", state);
		item->set_meta("git_icon", _file_icon(path));

		// The whole row is one cell that _draw_file_row paints (status letter, icon, name, folder),
		// so hover and selection highlight it as one unit. The name stays as the cell's text, made
		// invisible, so row height, type-to-search and accessibility still work.
		item->set_cell_mode(COLUMN_NAME, TreeItem::CELL_MODE_CUSTOM);
		item->set_custom_draw_callback(COLUMN_NAME, draw_row);
		item->set_text(COLUMN_NAME, path.get_file());
		item->set_custom_color(COLUMN_NAME, Color(0, 0, 0, 0));
	}

	tree->set_visible(files > 0);
	p_pane.empty_label->get_parent_control()->set_visible(files == 0);
	p_pane.empty_label->set_text(p_pane.staged ? "Nothing staged." : "No changes.");

	// The title counts the rows you see; commits and their messages count every file.
	p_pane.file_count = files;
	p_pane.container->set_title(files > 0 ? vformat("%s (%d)", p_pane.title, rows) : p_pane.title);
	_show_line_stats(p_pane); // The last counts, until the new ones arrive.
	p_pane.action->set_disabled(files == 0);
	if (p_pane.discard) {
		p_pane.discard->set_disabled(files == 0);
	}
}

// Counts the lines of both lists in the background (see stats_thread). A refresh during a count
// asks for one more count afterwards, instead of piling up threads.
void GitDock::_start_line_stats() {
	if (stats_thread.is_valid() && stats_thread->is_started()) {
		stats_again = true;
		return;
	}
	stats_thread.instantiate();
	stats_thread->start(callable_mp(this, &GitDock::_line_stats_worker).bind(repo->get_workdir()));
	stats_slow_timer->start(0.25);
}

// Runs on stats_thread. A GitRepository must only be used by one thread, so this opens its own.
void GitDock::_line_stats_worker(const String &p_workdir) {
	Ref<GitRepository> worker_repo;
	worker_repo.instantiate();
	Dictionary staged, unstaged;
	if (worker_repo->open(p_workdir) == OK) {
		staged = worker_repo->get_line_stats(true);
		unstaged = worker_repo->get_line_stats(false);
	}
	callable_mp(this, &GitDock::_line_stats_done).call_deferred(staged, unstaged);
}

void GitDock::_line_stats_done(const Dictionary &p_staged, const Dictionary &p_unstaged) {
	_finish_stats_thread();
	stats_slow_timer->stop();
	stats_slow = false;
	staged_stats = p_staged;
	unstaged_stats = p_unstaged;
	_show_line_stats(staged_pane);
	_show_line_stats(changes_pane);
	if (stats_again) {
		stats_again = false;
		_start_line_stats();
	}
}

// Still counting after a quarter second: say so, rather than show old totals as if current.
void GitDock::_on_line_stats_slow() {
	stats_slow = true;
	_show_line_stats(staged_pane);
	_show_line_stats(changes_pane);
}

// Puts the line counts on a list: totals in its header ("+1204 −35"), each file's in its tooltip.
// Files not counted yet (new since the last count) have none until the count arrives.
void GitDock::_show_line_stats(FilePane &p_pane) {
	const Dictionary &stats = p_pane.staged ? staged_stats : unstaged_stats;
	int total_added = 0, total_removed = 0;
	bool counted = false;
	TreeItem *root = p_pane.tree->get_root();
	for (TreeItem *item = root ? root->get_first_child() : nullptr; item; item = item->get_next()) {
		const String path = item->get_meta("git_path", String());
		const String state = item->get_meta("git_state", String());
		String lines;
		if (stats.has(path)) {
			counted = true;
			const Vector2i count = stats[path];
			if (count.x < 0) {
				lines = "binary or too large to count lines";
			} else {
				total_added += count.x;
				total_removed += count.y;
				if (count.x > 0 || count.y > 0) {
					lines = vformat("+%d %s%d", count.x, minus(), count.y);
				}
			}
		}
		// The row itself stays calm (totals are in the header); the tooltip has this file's counts.
		String tooltip = lines.is_empty() ? vformat("%s\n%s", path, status_name(state)) : vformat(String::utf8("%s\n%s · %s"), path, status_name(state), lines);
		const PackedStringArray with = item->get_meta("git_companions", PackedStringArray());
		if (!with.is_empty()) {
			tooltip += vformat("\nWith %s, which Godot keeps next to it: staged, unstaged and discarded together.", String(", ").join(with));
		}
		const PackedStringArray blocking = _blocking_paths(item);
		if (!blocking.is_empty()) {
			tooltip += vformat("\n\nThe new commits on %s change %s too, in a way your uncommitted changes can't be merged with, so Pull waits until they're committed or discarded.", String(sync_status.get("upstream", String())), blocking.size() == 1 && blocking[0] == path ? String("this file") : String(", ").join(blocking));
		}
		item->set_tooltip_text(COLUMN_NAME, tooltip);
		item->set_meta("git_tooltip", tooltip); // Put back after the tooltip of a row button.
	}

	// Header: "Changes (27)  +1204 −35  [⊖]".
	const bool show = p_pane.file_count > 0 && counted;
	p_pane.added->set_text(show ? vformat("+%d", total_added) : String());
	p_pane.removed->set_text(show ? vformat("%s%d", minus(), total_removed) : String());
	const Color modulate = stats_slow ? Color(1, 1, 1, 0.5) : Color(1, 1, 1);
	p_pane.added->set_modulate(modulate);
	p_pane.removed->set_modulate(modulate);
	const String counting = String::utf8("\nCounting lines again…");
	p_pane.added->set_tooltip_text(plural(total_added, "line added", "lines added") + (stats_slow ? counting : String()));
	p_pane.removed->set_tooltip_text(plural(total_removed, "line removed", "lines removed") + (stats_slow ? counting : String()));
}

void GitDock::_finish_stats_thread() {
	if (stats_thread.is_valid() && stats_thread->is_started()) {
		stats_thread->wait_to_finish();
	}
	stats_thread.unref();
}

void GitDock::_queue_align_header_buttons() {
	if (!align_queued) {
		align_queued = true;
		callable_mp(this, &GitDock::_align_header_buttons).call_deferred();
	}
}

// Shifts each header's "all" buttons so they end exactly where the rows' status letters end (the
// right edge of the file column), whatever the theme margins and editor scale are.
void GitDock::_align_header_buttons() {
	align_queued = false;
	for (FilePane *pane : { &staged_pane, &changes_pane }) {
		if (!pane->container->is_visible_in_tree()) {
			continue;
		}
		// In a narrow dock the header can't hold the title, the +/- totals and the buttons, and
		// the title got cut off ("Changes (1"). The count matters more, so the totals step aside
		// (each file's counts stay in its tooltip). Decided from widths that don't depend on
		// whether the totals are shown, so hiding them can't flip the decision back.
		FoldableContainer *section = pane->container;
		const Ref<Font> font = section->get_theme_font("font");
		const int font_size = section->get_theme_font_size("font_size");
		const int separation = section->get_theme_constant("h_separation");
		float needed = section->get_theme_stylebox("title_panel")->get_minimum_size().x + section->get_theme_icon("expanded_arrow")->get_width() + separation;
		needed += font->get_string_size(section->get_title(), HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
		for (Control *control : { (Control *)pane->added, (Control *)pane->removed, (Control *)pane->buttons_margin }) {
			needed += control->get_combined_minimum_size().x + separation;
		}
		float available = section->get_size().x;
		for (Node *parent = section->get_parent(); parent; parent = parent->get_parent()) {
			if (ScrollContainer *scroll = Object::cast_to<ScrollContainer>(parent)) {
				// The scroll bar depends on the lists' height, not on the totals: no loop either.
				const VScrollBar *bar = scroll->get_v_scroll_bar();
				available = MIN(available, (float)(scroll->get_size().x - (bar->is_visible() ? bar->get_size().x : 0.0f)));
				break;
			}
		}
		const bool totals_fit = needed <= available;
		pane->added->set_visible(totals_fit);
		pane->removed->set_visible(totals_fit);
		Tree *tree = pane->tree;
		TreeItem *first = tree->get_root() ? tree->get_root()->get_first_child() : nullptr;
		// Nothing to line up with while the section is empty or folded. A folded section's tree
		// has no row geometry: aligning to it moved the buttons, which re-sorted the header, which
		// aligned again, forever, and the flood of layout calls crashed the editor.
		if (!first || !tree->is_visible_in_tree()) {
			continue;
		}
		// Flush with the rows' right edge, above the status letters (see _draw_file_row), known
		// once a row has been drawn.
		if (!first->has_meta("git_row_right")) {
			continue;
		}
		const float rows_right = tree->get_global_position().x + float(first->get_meta("git_row_right"));
		const float buttons_right = pane->buttons->get_global_position().x + pane->buttons->get_size().x;
		const int margin = pane->buttons_margin->get_theme_constant("margin_right");
		const int aligned = MAX(0, (int)Math::round(margin + buttons_right - rows_right));
		if (aligned != margin) {
			pane->buttons_margin->add_theme_constant_override("margin_right", aligned);
		}
	}
}

// Paints a file row: file icon, name, "+uid"/"+import" and the folder dimmed after it, the status
// letter at the far right and, on the hovered row, its buttons just left of the letter (like VS
// Code). Text is trimmed when space runs out. p_rect is the cell's content area.
void GitDock::_draw_file_row(TreeItem *p_item, const Rect2 &p_rect) {
	Tree *tree = p_item->get_tree();
	const FilePane *pane = _pane_for_tree(tree); // Null for a commit's files in History.
	const Ref<Font> font = tree->get_theme_font("font");
	const int font_size = tree->get_theme_font_size("font_size");
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	// The Tree's layer for custom drawing: above the hover/selection background, clipped to the rows.
	const RID canvas = tree->get_custom_drawing_canvas_item();
	const String path = p_item->get_meta("git_path", String());
	const String state = p_item->get_meta("git_state", String());
	float right = p_rect.get_end().x; // Where text stops; moves left for the warning sign.

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

	// From the right: the status letter in a fixed-width slot so the letters line up down the list
	// (the FileSystem dock shows the same letter the same way; see filesystem_colors.cpp), a
	// pull warning next to it, and on the hovered row its buttons left of those, so neither the
	// letter nor the warning moves. A commit's files in History have the letter in the ages'
	// column instead (set in _fill_commit).
	const Color color = _status_color(state);
	if (pane) {
		// The header's "all" buttons end where the letters do (see _align_header_buttons).
		if (float(p_item->get_meta("git_row_right", -1.0f)) != right) {
			p_item->set_meta("git_row_right", right);
			_queue_align_header_buttons(); // Depends only on the tree's width, so it settles.
		}
		const float letter_width = font->get_string_size("M", HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
		const String letter = status_letter(state);
		right -= letter_width;
		const float letter_x = right + (letter_width - font->get_string_size(letter, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x) / 2;
		font->draw_string(canvas, Vector2(letter_x, p_rect.position.y + (p_rect.size.y - font->get_height(font_size)) / 2 + font->get_ascent(font_size)), letter, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, color);
		right -= 6 * scale;

		// A file the new commits change in a way your edit can't be merged with (Pull waits for it).
		if (!_blocking_paths(p_item).is_empty()) {
			const Ref<Texture2D> warning = get_theme_icon("StatusWarning", "EditorIcons");
			right -= warning->get_width();
			warning->draw(canvas, Vector2(right, p_rect.position.y + (p_rect.size.y - warning->get_height()) / 2));
			right -= 4 * scale;
		}

		if (p_item->get_instance_id() == pane->hovered_item) {
			right = _draw_row_buttons(p_item, *pane, p_rect, right) - 4 * scale;
		}
	}

	float x = p_rect.position.x;
	const bool deleted = state == "deleted";
	const Ref<Texture2D> icon = p_item->get_meta("git_icon", Variant());
	if (icon.is_valid()) {
		icon->draw(canvas, Vector2(x, p_rect.position.y + (p_rect.size.y - icon->get_height()) / 2), Color(1, 1, 1, deleted ? 0.5 : 1));
		x += icon->get_width() + tree->get_theme_constant("icon_h_separation");
	}

	// The name in the Tree's own text colors, so selection and hover look native. Not in its
	// status color, unlike the FileSystem dock: there color picks the changed files out of all
	// of them, but every file here is changed, and the letter already says how. Colored names
	// were tried (2026-09-28) and read busy; kept here, switched off, in case that's revisited.
	constexpr bool COLOR_NAMES = false;
	Color name_color = tree->get_theme_color("font_color");
	if (p_item->is_selected(COLUMN_NAME)) {
		name_color = tree->get_theme_color("font_selected_color");
	} else if (COLOR_NAMES) {
		name_color = color;
	} else if (pane && p_item->get_instance_id() == pane->hovered_item) {
		name_color = tree->get_theme_color("font_hovered_color");
	}
	if (deleted) {
		name_color.a *= 0.5;
	}
	// Too long a name loses its middle, not its end: files often differ only at the end
	// ("..._frame_012_variant_b.png"), and 500 rows of "final_boss_phase_two_attack_..." looked
	// identical. The folder then has no room and is left out (it's in the tooltip).
	// Companions shown on this row ("+import"), dimmed like the folder; see _fill_file_pane.
	String extras;
	for (const String &companion : PackedStringArray(p_item->get_meta("git_companions", PackedStringArray()))) {
		extras += (extras.is_empty() ? "+" : " +") + companion.get_extension();
	}
	const float extras_width = extras.is_empty() ? 0.0f : font->get_string_size(extras, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x + 6 * scale;
	x = draw_text(trim_middle(path.get_file(), font, font_size, right - x - extras_width), x, name_color);
	if (!extras.is_empty()) {
		x = draw_text(extras, x + 6 * scale, _dim_color());
	}

	const String folder = path.get_base_dir();
	if (!folder.is_empty()) {
		draw_text(folder, x + 6 * scale, _dim_color());
	}
}

// The hovered row's buttons, like VS Code, ending at p_right; returns where they start. Drawn
// here rather than as the Tree's cell buttons, which it always puts at the row's right end,
// pushing the letter aside. Clicks on them are caught in _on_tree_gui_input, which finds their
// rects on the row (meta "git_button_rects").
float GitDock::_draw_row_buttons(TreeItem *p_item, const FilePane &p_pane, const Rect2 &p_rect, float p_right) {
	Tree *tree = p_pane.tree;
	const RID canvas = tree->get_custom_drawing_canvas_item();
	const Ref<StyleBox> hover_style = tree->get_theme_stylebox("button_hover");
	const Size2 padding = tree->get_theme_stylebox("button_pressed")->get_minimum_size();
	const float margin = tree->get_theme_constant("button_margin");
	float right = p_right;
	Array rects;
	const Array buttons = _row_button_list(p_pane);
	for (int i = buttons.size() - 1; i >= 0; i--) {
		const Dictionary button = buttons[i];
		const Ref<Texture2D> icon = button["icon"];
		const Size2 size = icon->get_size() + padding;
		const Rect2 rect(right - size.x, p_rect.position.y + (p_rect.size.y - size.y) / 2, size.x, size.y);
		if ((int)button["id"] == p_pane.hovered_button) {
			hover_style->draw(canvas, rect);
		}
		icon->draw(canvas, rect.position + padding / 2); // Editor icons come in the theme's colors.
		Dictionary hit = button.duplicate();
		hit["rect"] = rect;
		rects.push_back(hit);
		right -= size.x + margin;
	}
	p_item->set_meta("git_button_rects", rects);
	return right;
}
