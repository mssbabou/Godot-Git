#include "editor/git_conflict_view.h"

#include <godot_cpp/classes/center_container.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_flow_container.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/v_separator.hpp>

#include "editor/git_colors.h"
#include "editor/git_diff_dock.h"
#include "editor/ui_text.h"

using namespace godot_git;

namespace {

enum Choice {
	CHOICE_MINE,
	CHOICE_THEIRS,
	CHOICE_MINE_FIRST,
	CHOICE_THEIRS_FIRST,
};

// A row of a side pane (see diff_rows).
enum SideRow : uint8_t {
	ROW_KEPT,
	ROW_REMOVED,
	ROW_ADDED,
	ROW_FILLER,
};

enum Frame {
	FRAME_MINE,
	FRAME_THEIRS,
	FRAME_RESULT,
};

// Unresolved blocks in the result are marked by rows of our own, not git's <<<<<<< ======= >>>>>>>
// (maintainer, 2026-10-02): "Mine · main" over your lines, "Theirs · feature" over theirs, and an
// empty row ending the block. Each starts with an invisible zero-width space, which is how they're
// told from the file's own lines. They never reach the file: Resolve File waits until none is left.
constexpr char32_t MARK = 0x200B;

bool is_marker(const String &p_line) {
	return !p_line.is_empty() && p_line[0] == MARK;
}

// A block's "Mine" row also carries which conflict it is (its index in get_conflict's blocks), in
// invisible characters after the mark: zero-width non-joiners and joiners as bits. That's how the
// original lines (the base) are found again for it after other blocks were settled or edited.
constexpr char32_t BIT_ZERO = 0x200C;
constexpr char32_t BIT_ONE = 0x200D;

String encode_id(int p_id) {
	String bits;
	for (int bit = 15; bit >= 0; bit--) {
		bits += String::chr(((p_id >> bit) & 1) ? BIT_ONE : BIT_ZERO);
	}
	return bits;
}

int decode_id(const String &p_line) {
	int id = 0;
	int bits = 0;
	for (int i = 1; i < p_line.length() && (p_line[i] == BIT_ZERO || p_line[i] == BIT_ONE); i++) {
		id = (id << 1) | (p_line[i] == BIT_ONE ? 1 : 0);
		bits++;
	}
	return bits == 16 ? id : -1;
}

PackedStringArray text_lines(const String &p_text) {
	if (p_text.is_empty()) {
		return PackedStringArray();
	}
	return (p_text.ends_with("\n") ? p_text.left(-1) : p_text).split("\n");
}

// What one side did to the original lines, as rows: kept (context), removed, added. A plain
// longest-common-subsequence diff; conflicts are a few lines, and past a few hundred it gives up
// and shows all removed, then all added.
void diff_rows(const PackedStringArray &p_base, const PackedStringArray &p_side, PackedStringArray &r_text, PackedByteArray &r_kinds) {
	const int n = p_base.size();
	const int m = p_side.size();
	auto add = [&](const String &p_line, uint8_t p_kind) {
		r_text.push_back(p_line);
		r_kinds.push_back(p_kind);
	};
	if ((int64_t)n * m > 250000) {
		for (const String &line : p_base) {
			add(line, ROW_REMOVED);
		}
		for (const String &line : p_side) {
			add(line, ROW_ADDED);
		}
		return;
	}
	// lcs[i][j]: longest common run of p_base[i..] and p_side[j..].
	LocalVector<int> lcs;
	lcs.resize((n + 1) * (m + 1));
	for (int i = n; i >= 0; i--) {
		for (int j = m; j >= 0; j--) {
			int &cell = lcs[i * (m + 1) + j];
			if (i == n || j == m) {
				cell = 0;
			} else if (p_base[i] == p_side[j]) {
				cell = lcs[(i + 1) * (m + 1) + j + 1] + 1;
			} else {
				cell = MAX(lcs[(i + 1) * (m + 1) + j], lcs[i * (m + 1) + j + 1]);
			}
		}
	}
	int i = 0, j = 0;
	while (i < n || j < m) {
		if (i < n && j < m && p_base[i] == p_side[j]) {
			add(p_side[j], ROW_KEPT);
			i++;
			j++;
		} else if (i < n && (j == m || lcs[(i + 1) * (m + 1) + j] >= lcs[i * (m + 1) + j + 1])) {
			add(p_base[i], ROW_REMOVED);
			i++;
		} else {
			add(p_side[j], ROW_ADDED);
			j++;
		}
	}
}

} // namespace

void GitConflictView::_bind_methods() {
	// The result to write and stage (resolve_requested), or a whole side to take ("mine" /
	// "theirs"; side_requested). The Git dock does the writing.
	ADD_SIGNAL(MethodInfo("resolve_requested", PropertyInfo(Variant::STRING, "path"), PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("side_requested", PropertyInfo(Variant::STRING, "path"), PropertyInfo(Variant::STRING, "side")));
}

GitConflictView::GitConflictView() {
	set_v_size_flags(SIZE_EXPAND_FILL);

	HFlowContainer *bar = memnew(HFlowContainer);
	add_child(bar);
	toolbar = bar;
	prev_button = memnew(Button);
	prev_button->set_flat(true);
	prev_button->set_tooltip_text("Previous conflict");
	prev_button->connect("pressed", callable_mp(this, &GitConflictView::_go).bind(-1));
	bar->add_child(prev_button);
	position_label = memnew(Label);
	position_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	bar->add_child(position_label);
	next_button = memnew(Button);
	next_button->set_flat(true);
	next_button->set_tooltip_text("Next conflict");
	next_button->connect("pressed", callable_mp(this, &GitConflictView::_go).bind(1));
	bar->add_child(next_button);
	const char *choice_names[4] = { "Keep Mine", "Take Theirs", "Both, Mine First", "Both, Theirs First" };
	for (int i = 0; i < 4; i++) {
		choice_buttons[i] = memnew(Button);
		choice_buttons[i]->set_text(choice_names[i]);
		choice_buttons[i]->connect("pressed", callable_mp(this, &GitConflictView::_choose).bind(i));
		bar->add_child(choice_buttons[i]);
	}
	bar->add_child(memnew(VSeparator));
	all_mine_button = memnew(Button);
	all_mine_button->set_text("Keep All Mine");
	all_mine_button->connect("pressed", callable_mp(this, &GitConflictView::_take_whole).bind("mine"));
	bar->add_child(all_mine_button);
	all_theirs_button = memnew(Button);
	all_theirs_button->set_text("Take All Theirs");
	all_theirs_button->connect("pressed", callable_mp(this, &GitConflictView::_take_whole).bind("theirs"));
	bar->add_child(all_theirs_button);
	resolve_button = memnew(Button);
	resolve_button->set_text("Resolve File");
	resolve_button->connect("pressed", callable_mp(this, &GitConflictView::_on_resolve));
	bar->add_child(resolve_button);

	// The two sides of the current conflict above, the whole result below.
	split = memnew(VSplitContainer);
	split->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(split);
	HBoxContainer *sides = memnew(HBoxContainer);
	sides->set_custom_minimum_size(Vector2(0, 130 * EditorInterface::get_singleton()->get_editor_scale()));
	split->add_child(sides);
	mine_edit = _make_code(sides, FRAME_MINE, false, &mine_caption);
	theirs_edit = _make_code(sides, FRAME_THEIRS, false, &theirs_caption);
	VBoxContainer *result_box = memnew(VBoxContainer);
	result_box->set_v_size_flags(SIZE_EXPAND_FILL);
	split->add_child(result_box);
	result_edit = _make_code(result_box, FRAME_RESULT, true, &result_caption);
	result_edit->connect("text_changed", callable_mp(this, &GitConflictView::_on_result_changed));

	VBoxContainer *whole = memnew(VBoxContainer);
	whole->hide();
	add_child(whole);
	whole_view = whole;
	whole_label = memnew(Label);
	whole_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	whole_label->set_custom_minimum_size(Vector2(200 * EditorInterface::get_singleton()->get_editor_scale(), 0)); // Gotcha 45.
	whole->add_child(whole_label);
	// Two halves, so with pictures above (an image conflict, shown by the Git Diff panel) each
	// button is under its side.
	HBoxContainer *whole_buttons = memnew(HBoxContainer);
	whole->add_child(whole_buttons);
	for (int i = 0; i < 2; i++) {
		CenterContainer *half = memnew(CenterContainer);
		half->set_h_size_flags(SIZE_EXPAND_FILL);
		whole_buttons->add_child(half);
		Button *button = memnew(Button);
		button->connect("pressed", callable_mp(this, &GitConflictView::_take_whole).bind(i == 0 ? "mine" : "theirs"));
		half->add_child(button);
		(i == 0 ? whole_mine_button : whole_theirs_button) = button;
	}
}

// A caption over a CodeEdit in a frame that draws the code editor's background (gotcha 38).
CodeEdit *GitConflictView::_make_code(Control *p_parent, int p_frame, bool p_editable, Label **r_caption) {
	VBoxContainer *box = memnew(VBoxContainer);
	box->set_h_size_flags(SIZE_EXPAND_FILL);
	box->set_v_size_flags(SIZE_EXPAND_FILL);
	box->add_theme_constant_override("separation", 2);
	p_parent->add_child(box);
	*r_caption = memnew(Label);
	box->add_child(*r_caption);
	frames[p_frame] = memnew(PanelContainer);
	frames[p_frame]->set_v_size_flags(SIZE_EXPAND_FILL);
	box->add_child(frames[p_frame]);
	CodeEdit *edit = memnew(CodeEdit);
	edit->set_editable(p_editable);
	if (!p_editable) {
		// "+" and "−" before the lines, like the Git Diff panel (see _draw_sign).
		edit->add_gutter();
		const int gutter = edit->get_gutter_count() - 1;
		edit->set_gutter_type(gutter, TextEdit::GUTTER_TYPE_CUSTOM);
		edit->set_gutter_custom_draw(gutter, callable_mp(this, &GitConflictView::_draw_sign).bind(p_frame));
		edit->set_gutter_width(gutter, (int)(16 * EditorInterface::get_singleton()->get_editor_scale()));
	}
	edit->set_v_size_flags(SIZE_EXPAND_FILL);
	edit->set_draw_line_numbers(p_editable);
	edit->set_highlight_current_line(p_editable);
	edit->set_clip_contents(true);
	frames[p_frame]->add_child(edit);
	return edit;
}

void GitConflictView::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED || p_what == NOTIFICATION_READY) {
		_update_theme();
	}
}

void GitConflictView::_update_theme() {
	prev_button->set_button_icon(get_theme_icon("ArrowUp", "EditorIcons"));
	next_button->set_button_icon(get_theme_icon("ArrowDown", "EditorIcons"));
	Ref<StyleBoxEmpty> none;
	none.instantiate();
	for (int i = 0; i < 3; i++) {
		frames[i]->add_theme_stylebox_override("panel", get_theme_stylebox(i == FRAME_RESULT ? "normal" : "read_only", "CodeEdit"));
	}
	for (CodeEdit *edit : { mine_edit, theirs_edit, result_edit }) {
		for (const char *name : { "normal", "read_only", "focus" }) {
			edit->add_theme_stylebox_override(name, none);
		}
		edit->add_theme_color_override("font_readonly_color", edit->get_theme_color("font_color"));
	}
	mine_caption->add_theme_color_override("font_color", conflict_side_color(true));
	theirs_caption->add_theme_color_override("font_color", conflict_side_color(false));
	result_caption->add_theme_color_override("font_color", get_theme_color("font_color", "Label") * Color(1, 1, 1, 0.55));
	if (!path.is_empty()) {
		_show_current(false);
	}
}

void GitConflictView::set_conflict(const Dictionary &p_conflict, const Ref<SyntaxHighlighter> &p_result_highlighter, const Ref<SyntaxHighlighter> &p_mine_highlighter, const Ref<SyntaxHighlighter> &p_theirs_highlighter) {
	conflict = p_conflict;
	path = p_conflict.get("path", String());
	current = 0;
	const String mine_label = p_conflict.get("mine_label", String());
	const String theirs_label = p_conflict.get("theirs_label", String());
	mine_caption->set_text(mine_label.is_empty() ? String("Mine") : vformat(String::utf8("Mine · %s"), mine_label));
	theirs_caption->set_text(theirs_label.is_empty() ? String("Theirs") : vformat(String::utf8("Theirs · %s"), theirs_label));
	result_caption->set_text("Result: the file as it will be. Choose above, or edit it here.");

	// Whole sides only: binary or LFS files, or a side that deleted the file.
	const Array conflict_blocks = p_conflict.get("blocks", Array());
	const bool whole = conflict_blocks.is_empty();
	toolbar->set_visible(!whole);
	split->set_visible(!whole);
	whole_view->set_visible(whole);
	if (whole) {
		const String name = path.get_file();
		const String mine_name = mine_label.is_empty() ? String("you") : mine_label;
		const String theirs_name = theirs_label.is_empty() ? String("the other side") : theirs_label;
		if (!bool(p_conflict.get("mine_exists", true))) {
			whole_label->set_text(vformat("%s deleted %s; %s changed it. Keep it deleted, or take %s version?", mine_name, name, theirs_name, theirs_name));
			whole_mine_button->set_text("Keep It Deleted");
			whole_theirs_button->set_text("Take Theirs");
		} else if (!bool(p_conflict.get("theirs_exists", true))) {
			whole_label->set_text(vformat("%s deleted %s; %s changed it. Keep your version, or delete it?", theirs_name, name, mine_name));
			whole_mine_button->set_text("Keep Mine");
			whole_theirs_button->set_text("Delete It");
		} else {
			// An image shows both versions above this (the Git Diff panel's image view).
			whole_label->set_text(GitDiffDock::is_image_path(path) ? vformat("Both sides changed %s. Pick the one to keep.", name) : vformat("%s can't be merged line by line (it's binary, or stored with Git LFS). Keep one version whole.", name));
			whole_mine_button->set_text(vformat("Keep Mine (%s)", mine_name));
			whole_theirs_button->set_text(vformat("Take Theirs (%s)", theirs_name));
		}
		return;
	}

	// The result starts as git would leave the file: what both sides agree on, and each conflict
	// between markers.
	String text;
	bases.clear();
	bases.resize(conflict_blocks.size());
	for (int i = 0; i < conflict_blocks.size(); i++) {
		const Dictionary block = conflict_blocks[i];
		if (String(block["kind"]) == "same") {
			text += String(block["text"]);
			continue;
		}
		String mine = block["mine"];
		String theirs = block["theirs"];
		if (!mine.is_empty() && !mine.ends_with("\n")) {
			mine += "\n";
		}
		if (!theirs.is_empty() && !theirs.ends_with("\n")) {
			theirs += "\n";
		}
		const String mark = String::chr(MARK);
		bases[i] = String(block["base"]);
		text += vformat("%s%s%s\n%s%s%s\n%s%s\n", mark, encode_id(i), mine_caption->get_text(), mine, mark, theirs_caption->get_text(), theirs, mark);
	}
	result_edit->set_syntax_highlighter(p_result_highlighter);
	mine_edit->set_syntax_highlighter(p_mine_highlighter);
	theirs_edit->set_syntax_highlighter(p_theirs_highlighter);
	updating = true;
	result_edit->set_text(text.trim_suffix("\n"));
	result_edit->clear_undo_history();
	updating = false;
	_parse();
	_show_current(true);
}

// Finds the conflict blocks in the result as it is now (edited by hand or not).
void GitConflictView::_parse() {
	blocks.clear();
	Block block;
	int stage = 0; // 0 outside a block, 1 in mine, 2 in theirs.
	int markers = 0;
	for (int line = 0; line < result_edit->get_line_count(); line++) {
		if (!is_marker(result_edit->get_line(line))) {
			continue;
		}
		markers++;
		if (stage == 0) {
			block.start = line;
			block.id = decode_id(result_edit->get_line(line));
			stage = 1;
		} else if (stage == 1) {
			block.middle = line;
			stage = 2;
		} else {
			block.end = line;
			blocks.push_back(block);
			stage = 0;
		}
	}
	// A block's rows partly deleted by hand: what's left isn't a block, and must not be written.
	stray_markers = markers - (int)blocks.size() * 3;
	current = blocks.is_empty() ? 0 : CLAMP(current, 0, (int)blocks.size() - 1);
}

String GitConflictView::_lines(int p_from, int p_to) const {
	PackedStringArray lines;
	for (int line = p_from; line <= p_to; line++) {
		lines.push_back(result_edit->get_line(line));
	}
	return String("\n").join(lines);
}

// The current conflict's sides at the top, the result's blocks tinted (the current one stronger),
// and what can be done now.
void GitConflictView::_show_current(bool p_scroll) {
	const bool any = !blocks.is_empty();
	position_label->set_text(any ? vformat("Conflict %d of %d", current + 1, blocks.size()) : String("No conflicts left"));
	prev_button->set_disabled(!any || current == 0);
	next_button->set_disabled(!any || current >= (int)blocks.size() - 1);
	for (Button *button : choice_buttons) {
		button->set_disabled(!any);
	}
	resolve_button->set_disabled(any || stray_markers > 0);
	if (any) {
		resolve_button->set_tooltip_text(vformat("%s still undecided in the result.", plural(blocks.size(), "conflict is", "conflicts are")));
	} else if (stray_markers > 0) {
		resolve_button->set_tooltip_text("Some of a conflict's header rows (\"Mine\", \"Theirs\" and the empty row after them) are left in the result. Remove them, or undo, to finish.");
	} else {
		resolve_button->set_tooltip_text(vformat("Write the result to %s and stage it, which marks it resolved.", path.get_file()));
	}
	const String mine_label = conflict.get("mine_label", String());
	const String theirs_label = conflict.get("theirs_label", String());
	choice_buttons[CHOICE_MINE]->set_tooltip_text(vformat("Keep your side of this conflict%s.", mine_label.is_empty() ? String() : vformat(" (%s)", mine_label)));
	choice_buttons[CHOICE_THEIRS]->set_tooltip_text(vformat("Take their side of this conflict%s.", theirs_label.is_empty() ? String() : vformat(" (%s)", theirs_label)));
	all_mine_button->set_tooltip_text(vformat("Resolve %s with your whole version, then stage it.", path.get_file()));
	all_theirs_button->set_tooltip_text(vformat("Resolve %s with their whole version, then stage it.", path.get_file()));

	// Each side's header row in its color, its lines lighter, the current block stronger than the
	// rest.
	const Color mine_color = conflict_side_color(true);
	const Color theirs_color = conflict_side_color(false);
	for (int line = 0; line < result_edit->get_line_count(); line++) {
		result_edit->set_line_background_color(line, Color(0, 0, 0, 0));
	}
	for (int i = 0; i < (int)blocks.size(); i++) {
		const Block &block = blocks[i];
		const float strength = i == current ? 1.0f : 0.55f;
		for (int line = block.start; line <= block.end; line++) {
			Color tint;
			if (line == block.start) {
				tint = mine_color * Color(1, 1, 1, 0.3);
			} else if (line == block.middle) {
				tint = theirs_color * Color(1, 1, 1, 0.3);
			} else if (line == block.end) {
				tint = theirs_color * Color(1, 1, 1, 0.06);
			} else {
				tint = (line < block.middle ? mine_color : theirs_color) * Color(1, 1, 1, 0.14);
			}
			tint.a *= strength;
			result_edit->set_line_background_color(line, tint);
		}
	}

	String mine, theirs;
	if (any) {
		const Block &block = blocks[current];
		mine = block.middle > block.start + 1 ? _lines(block.start + 1, block.middle - 1) : String();
		theirs = block.end > block.middle + 1 ? _lines(block.middle + 1, block.end - 1) : String();
		if (p_scroll) {
			result_edit->set_line_as_first_visible(MAX(0, block.start - 3));
		}
	}
	// Each side as what it did to the original: removed lines red, added green, like the Git Diff
	// panel; grey filler rows at the end of the shorter side keep the two the same height.
	const PackedStringArray base = any && blocks[current].id >= 0 && blocks[current].id < bases.size() ? text_lines(bases[blocks[current].id]) : PackedStringArray();
	PackedStringArray rows[2];
	for (int side = 0; side < 2; side++) {
		side_kinds[side].clear();
		if (any) {
			diff_rows(base, text_lines(side == 0 ? mine : theirs), rows[side], side_kinds[side]);
		}
	}
	const int height = MAX(rows[0].size(), rows[1].size());
	for (int side = 0; side < 2; side++) {
		while (rows[side].size() < height) {
			rows[side].push_back(String());
			side_kinds[side].push_back(ROW_FILLER);
		}
		CodeEdit *edit = side == 0 ? mine_edit : theirs_edit;
		edit->set_text(String("\n").join(rows[side]));
		for (int line = 0; line < edit->get_line_count() && line < side_kinds[side].size(); line++) {
			const uint8_t kind = side_kinds[side][line];
			Color tint;
			if (kind == ROW_ADDED || kind == ROW_REMOVED) {
				tint = GitDiffDock::row_tint(kind == ROW_ADDED);
			} else if (kind == ROW_FILLER) {
				tint = _filler_tint();
			}
			edit->set_line_background_color(line, tint);
		}
	}
}

void GitConflictView::_on_result_changed() {
	if (updating) {
		return;
	}
	_parse();
	_show_current(false);
}

// Replaces the current block with the chosen side(s), as one undoable edit. A side with no lines
// (it removed them) adds nothing; choosing only that removes the block's lines altogether.
void GitConflictView::_choose(int p_choice) {
	if (blocks.is_empty()) {
		return;
	}
	const Block block = blocks[current];
	const bool has_mine = block.middle > block.start + 1;
	const bool has_theirs = block.end > block.middle + 1;
	const String mine = has_mine ? _lines(block.start + 1, block.middle - 1) : String();
	const String theirs = has_theirs ? _lines(block.middle + 1, block.end - 1) : String();
	PackedStringArray chosen;
	auto add = [&](bool p_has, const String &p_text) {
		if (p_has) {
			chosen.push_back(p_text);
		}
	};
	if (p_choice == CHOICE_MINE || p_choice == CHOICE_MINE_FIRST) {
		add(has_mine, mine);
	}
	if (p_choice != CHOICE_MINE) {
		add(has_theirs, theirs);
	}
	if (p_choice == CHOICE_THEIRS_FIRST) {
		add(has_mine, mine);
	}

	const int last = result_edit->get_line_count() - 1;
	result_edit->begin_complex_operation();
	if (!chosen.is_empty()) {
		result_edit->remove_text(block.start, 0, block.end, result_edit->get_line(block.end).length());
		result_edit->insert_text(String("\n").join(chosen), block.start, 0);
	} else if (block.end < last) {
		result_edit->remove_text(block.start, 0, block.end + 1, 0);
	} else if (block.start > 0) {
		result_edit->remove_text(block.start - 1, result_edit->get_line(block.start - 1).length(), block.end, result_edit->get_line(block.end).length());
	} else {
		result_edit->remove_text(block.start, 0, block.end, result_edit->get_line(block.end).length());
	}
	result_edit->end_complex_operation();
	_parse();
	_show_current(true);
}

void GitConflictView::_go(int p_step) {
	if (blocks.is_empty()) {
		return;
	}
	current = CLAMP(current + p_step, 0, (int)blocks.size() - 1);
	_show_current(true);
}

void GitConflictView::_take_whole(const String &p_side) {
	emit_signal("side_requested", path, p_side);
}

void GitConflictView::_on_resolve() {
	_parse();
	if (!blocks.is_empty()) {
		_show_current(true);
		return;
	}
	// As git stores it: the result ends with a newline, like the file it came from.
	emit_signal("resolve_requested", path, result_edit->get_text() + "\n");
}

// The +/− before a side pane's row, on the row's tint.
void GitConflictView::_draw_sign(int p_line, int p_gutter, const Rect2 &p_region, int p_side) {
	const PackedByteArray &kinds = side_kinds[p_side == FRAME_MINE ? 0 : 1];
	if (p_line < 0 || p_line >= kinds.size() || kinds[p_line] == ROW_KEPT) {
		return;
	}
	CodeEdit *edit = p_side == FRAME_MINE ? mine_edit : theirs_edit;
	const bool added = kinds[p_line] == ROW_ADDED;
	const bool filler = kinds[p_line] == ROW_FILLER;
	// TextEdit leaves a gap (gutter_padding) between the last gutter and the text that nothing
	// paints, a dark seam through the row (gotcha 46): the tint reaches across it.
	Rect2 tint = p_region;
	int drawn = 0;
	for (int g = 0; g < edit->get_gutter_count(); g++) {
		drawn += edit->is_gutter_drawn(g) ? edit->get_gutter_width(g) : 0;
	}
	tint.size.x += edit->get_total_gutter_width() - drawn;
	// Filler rows are grey from edge to edge, like the text part (see _show_current).
	edit->draw_rect(tint, filler ? _filler_tint() : GitDiffDock::row_tint(added));
	if (filler) {
		return;
	}
	const Ref<Font> font = edit->get_theme_font("font");
	const int size = edit->get_theme_font_size("font_size");
	const String sign = added ? String("+") : String::utf8("\u2212");
	const Vector2 at(p_region.position.x + (p_region.size.x - font->get_string_size(sign, HORIZONTAL_ALIGNMENT_LEFT, -1, size).x) / 2, p_region.position.y + (p_region.size.y - font->get_height(size)) / 2 + font->get_ascent(size));
	edit->draw_string(font, at, sign, HORIZONTAL_ALIGNMENT_LEFT, -1, size, get_theme_color(added ? "success_color" : "error_color", "Editor"));
}

Color GitConflictView::_filler_tint() const {
	return get_theme_color("font_color", "Label") * Color(1, 1, 1, 0.04);
}
