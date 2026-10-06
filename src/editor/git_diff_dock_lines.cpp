// The Diff panel's part in staging, unstaging and discarding part of a file: buttons on each
// hunk's header row, and items in the text's right-click menu for the selected lines. The panel
// only asks ("line_changes_requested"); the Git dock does it (GitRepository::apply_line_changes).

#include "editor/git_diff_dock.h"

#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/templates/hash_set.hpp>



namespace {

// Ids in the text's right-click menu, past TextEdit's own.
enum LineMenu {
	LINE_MENU_SEPARATOR = 1000,
	LINE_MENU_LINES_FIRST, // One per action, in _line_actions' order.
	LINE_MENU_HUNK_FIRST = 1010,
};

String verb(const String &p_action) {
	return p_action == "stage" ? String("Stage") : (p_action == "unstage" ? String("Unstage") : String("Discard"));
}

// A row of the diff: hunk index and line index in it, as one key.
int64_t entry_key(int p_hunk, int p_line) {
	return ((int64_t)p_hunk << 32) | (uint32_t)p_line;
}

} // namespace

// What can be done to the diff shown: staged changes can be unstaged, unstaged ones staged or
// discarded. Nothing for a commit's or stash's diff, or a file that isn't text.
PackedStringArray GitDiffDock::_line_actions() const {
	PackedStringArray actions;
	if (String(diff.get("kind", String())) != "text" || ((Array)diff.get("hunks", Array())).is_empty()) {
		return actions;
	}
	if (source == "Unstaged") {
		actions.push_back("stage");
		actions.push_back("discard");
	} else if (source == "Staged") {
		actions.push_back("unstage");
	}
	return actions;
}

// The changed lines of the given rows (keys from entry_key), as apply_line_changes takes them. A
// removed line and the added line that replaces it (by position in their runs, as the split view
// pairs them) go together: staging only the new line of an edit would put both in the index.
Array GitDiffDock::_lines_for(const HashSet<int64_t> &p_entries) const {
	const Array hunks = diff.get("hunks", Array());
	HashSet<int64_t> chosen;
	for (const int64_t &entry : p_entries) {
		chosen.insert(entry);
	}
	for (int h = 0; h < hunks.size(); h++) {
		const PackedByteArray origins = Dictionary(hunks[h])["origins"];
		for (int i = 0; i < origins.size();) {
			if (origins[i] == ' ') {
				i++;
				continue;
			}
			const int removed_start = i;
			while (i < origins.size() && origins[i] == '-') {
				i++;
			}
			const int added_start = i;
			while (i < origins.size() && origins[i] == '+') {
				i++;
			}
			for (int r = 0; r < MIN(added_start - removed_start, i - added_start); r++) {
				const int64_t removed = entry_key(h, removed_start + r);
				const int64_t added = entry_key(h, added_start + r);
				if (p_entries.has(removed) || p_entries.has(added)) {
					chosen.insert(removed);
					chosen.insert(added);
				}
			}
		}
	}
	Array lines;
	for (int h = 0; h < hunks.size(); h++) {
		const Dictionary hunk = hunks[h];
		const PackedByteArray origins = hunk["origins"];
		const PackedInt32Array old_numbers = hunk["old_numbers"];
		const PackedInt32Array new_numbers = hunk["new_numbers"];
		const PackedStringArray text = hunk["text"];
		for (int i = 0; i < origins.size(); i++) {
			if (origins[i] == ' ' || !chosen.has(entry_key(h, i))) {
				continue;
			}
			Dictionary line;
			line["old"] = origins[i] == '-' ? old_numbers[i] : -1;
			line["new"] = origins[i] == '+' ? new_numbers[i] : -1;
			line["text"] = text[i];
			lines.push_back(line);
		}
	}
	return lines;
}

Array GitDiffDock::_hunk_lines(int p_hunk) const {
	HashSet<int64_t> entries;
	const Array hunks = diff.get("hunks", Array());
	if (p_hunk >= 0 && p_hunk < hunks.size()) {
		const PackedByteArray origins = Dictionary(hunks[p_hunk])["origins"];
		for (int i = 0; i < origins.size(); i++) {
			entries.insert(entry_key(p_hunk, i));
		}
	}
	return _lines_for(entries);
}

// The changed lines in p_pane's selection, or on the caret's line when nothing is selected. In the
// side-by-side view a row is both sides of it.
Array GitDiffDock::_selected_lines(int p_pane) const {
	const CodeEdit *edit = panes[p_pane].edit;
	HashSet<int64_t> entries;
	for (int c = 0; c < edit->get_caret_count(); c++) {
		int from = edit->get_caret_line(c);
		int to = from;
		if (edit->has_selection(c)) {
			from = edit->get_selection_from_line(c);
			to = edit->get_selection_to_line(c);
			if (to > from && edit->get_selection_to_column(c) == 0) {
				to--; // Selected up to the start of a line: that line isn't part of it.
			}
		}
		for (int row = from; row <= to; row++) {
			for (int p = 0; p < PANE_COUNT; p++) {
				if (p != p_pane && (p_pane == PANE_UNIFIED || p == PANE_UNIFIED)) {
					continue;
				}
				const Pane &pane = panes[p];
				if (row < pane.lines.size() && pane.lines[row] >= 0 && (pane.kinds[row] == ROW_ADDED || pane.kinds[row] == ROW_REMOVED)) {
					entries.insert(entry_key(pane.hunks[row], pane.lines[row]));
				}
			}
		}
	}
	return _lines_for(entries);
}

// The hunk's buttons ("Stage Hunk", "Discard Hunk") at the right end of its header row, drawn on
// the CodeEdit (under its text) and hit-tested in _on_pane_input. In the side-by-side view only on
// the new side.
void GitDiffDock::_draw_hunk_buttons(int p_pane) {
	Pane &pane = panes[p_pane];
	pane.buttons.clear();
	const PackedStringArray actions = _line_actions();
	if (actions.is_empty() || p_pane == PANE_OLD) {
		return;
	}
	CodeEdit *edit = pane.edit;
	const Ref<Font> font = get_theme_font("font", "Button");
	const int font_size = get_theme_font_size("font_size", "Button");
	const Ref<StyleBox> hover = get_theme_stylebox("hover", "Button");
	const Color text_color = get_theme_color("font_color", "Editor");
	const float pad = 8 * theme.scale;
	float right_edge = edit->get_size().x - 6 * theme.scale;
	if (edit->get_v_scroll_bar()->is_visible()) {
		right_edge -= edit->get_v_scroll_bar()->get_size().x;
	}
	const int first = edit->get_first_visible_line();
	const int last = MIN(edit->get_last_full_visible_line() + 1, pane.kinds.size() - 1);
	for (int line = first; line <= last; line++) {
		if (pane.kinds[line] != ROW_HEADER) {
			continue;
		}
		const Rect2i row = edit->get_rect_at_line_column(line, 0);
		if (row.position.y < 0) {
			continue;
		}
		const float height = edit->get_line_height();
		float right = right_edge;
		for (int a = actions.size() - 1; a >= 0; a--) {
			const String label = verb(actions[a]) + " Hunk";
			const float width = font->get_string_size(label, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x + pad * 2;
			const Rect2 rect(right - width, row.position.y + 1 * theme.scale, width, height - 2 * theme.scale);
			const bool hovered = hovered_button_pane == p_pane && hovered_button == (int)pane.buttons.size();
			if (hovered && hover.is_valid()) {
				edit->draw_style_box(hover, rect);
			}
			const float baseline = rect.position.y + (rect.size.y - font->get_height(font_size)) / 2 + font->get_ascent(font_size);
			edit->draw_string(font, Vector2(rect.position.x + pad, baseline), label, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, hovered ? text_color : theme.dim);
			HunkButton button;
			button.rect = rect;
			button.hunk = pane.hunks[line];
			button.action = actions[a];
			pane.buttons.push_back(button);
			right -= width + 2 * theme.scale;
		}
	}
}

// Hover and clicks on the hunk buttons. Godot emits gui_input before TextEdit handles the event,
// so accepting a click here keeps it from moving the caret or selecting.
void GitDiffDock::_on_pane_input(const Ref<InputEvent> &p_event, int p_pane) {
	Pane &pane = panes[p_pane];
	const Ref<InputEventMouse> mouse = p_event;
	if (mouse.is_null()) {
		return;
	}
	int found = -1;
	for (int i = 0; i < (int)pane.buttons.size(); i++) {
		if (pane.buttons[i].rect.has_point(mouse->get_position())) {
			found = i;
			break;
		}
	}
	if ((hovered_button_pane == p_pane ? hovered_button : -1) != found) {
		hovered_button_pane = found >= 0 ? p_pane : -1;
		hovered_button = found;
		pane.edit->queue_redraw();
	}
	pane.edit->set_tooltip_text(found >= 0 ? (pane.buttons[found].action == "discard" ? String("Discard the changes in this hunk from the file (asks first).") : vformat("%s just the changes in this hunk.", verb(pane.buttons[found].action))) : String());
	const Ref<InputEventMouseButton> button = p_event;
	if (found >= 0 && button.is_valid() && button->get_button_index() == MOUSE_BUTTON_LEFT) {
		pane.edit->accept_event();
		if (button->is_pressed()) {
			emit_signal("line_changes_requested", pane.buttons[found].action, _hunk_lines(pane.buttons[found].hunk));
		}
	}
}

void GitDiffDock::_on_pane_mouse_exited(int p_pane) {
	if (hovered_button_pane == p_pane) {
		hovered_button_pane = -1;
		hovered_button = -1;
		panes[p_pane].edit->queue_redraw();
	}
}

// Before the right-click menu opens: our items, for the selected lines (or the caret's) and the
// caret's hunk, after TextEdit's own (Copy, Select All).
void GitDiffDock::_update_pane_menu(int p_pane) {
	PopupMenu *menu = panes[p_pane].edit->get_menu();
	for (int id = LINE_MENU_SEPARATOR; id < LINE_MENU_HUNK_FIRST + 10; id++) {
		const int index = menu->get_item_index(id);
		if (index >= 0) {
			menu->remove_item(index);
		}
	}
	const PackedStringArray actions = _line_actions();
	if (actions.is_empty()) {
		return;
	}
	const CodeEdit *edit = panes[p_pane].edit;
	const bool selection = edit->has_selection();
	const int lines = _selected_lines(p_pane).size();
	menu->add_separator(String(), LINE_MENU_SEPARATOR);
	for (int a = 0; a < actions.size(); a++) {
		const String action = actions[a];
		menu->add_item(vformat("%s %s%s", verb(action), selection ? "Selected Lines" : "Line", action == "discard" ? "..." : ""), LINE_MENU_LINES_FIRST + a);
		menu->set_item_disabled(menu->get_item_index(LINE_MENU_LINES_FIRST + a), lines == 0);
	}
	const Pane &pane = panes[p_pane];
	const int caret = edit->get_caret_line();
	if (caret >= 0 && caret < pane.hunks.size() && pane.hunks[caret] >= 0) {
		for (int a = 0; a < actions.size(); a++) {
			menu->add_item(vformat("%s Hunk%s", verb(actions[a]), actions[a] == "discard" ? "..." : ""), LINE_MENU_HUNK_FIRST + a);
		}
	}
}

void GitDiffDock::_on_pane_menu(int p_id, int p_pane) {
	const PackedStringArray actions = _line_actions();
	if (p_id >= LINE_MENU_LINES_FIRST && p_id < LINE_MENU_LINES_FIRST + actions.size()) {
		emit_signal("line_changes_requested", actions[p_id - LINE_MENU_LINES_FIRST], _selected_lines(p_pane));
	} else if (p_id >= LINE_MENU_HUNK_FIRST && p_id < LINE_MENU_HUNK_FIRST + actions.size()) {
		const Pane &pane = panes[p_pane];
		const int caret = pane.edit->get_caret_line();
		if (caret >= 0 && caret < pane.hunks.size()) {
			emit_signal("line_changes_requested", actions[p_id - LINE_MENU_HUNK_FIRST], _hunk_lines(pane.hunks[caret]));
		}
	}
}
