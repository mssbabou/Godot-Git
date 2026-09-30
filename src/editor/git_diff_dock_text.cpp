// The Diff panel's text views: the rows of the unified and side-by-side views, the CodeEdits that
// show them (tinted rows, line numbers and +/- in the gutters), and the syntax highlighting.

#include "editor/git_diff_dock.h"

#include <godot_cpp/classes/code_highlighter.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/gd_script_syntax_highlighter.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/math.hpp>

#include "editor/ui_text.h"

using namespace godot_git;

namespace {

Color highlighting_color(const String &p_name) {
	return EditorInterface::get_singleton()->get_editor_settings()->get_setting("text_editor/theme/highlighting/" + p_name);
}

// The line comment and block comment markers of a file type, for the generic highlighter.
struct CommentStyle {
	const char *line = nullptr;
	const char *block_start = nullptr;
	const char *block_end = nullptr;
};

CommentStyle comment_style(const String &p_extension) {
	static const char *hash_files[] = { "cfg", "py", "sh", "yml", "yaml", "toml", "gitignore", "gitattributes", "editorconfig" };
	static const char *semicolon_files[] = { "tscn", "tres", "godot", "import", "ini", "escn" };
	static const char *c_files[] = { "cs", "gdshader", "gdshaderinc", "shader", "glsl", "c", "cc", "cpp", "h", "hpp", "js", "ts", "java", "json", "jsonc", "rs", "go" };
	for (const char *ext : hash_files) {
		if (p_extension == ext) {
			return { "#" };
		}
	}
	for (const char *ext : semicolon_files) {
		if (p_extension == ext) {
			return { ";" };
		}
	}
	for (const char *ext : c_files) {
		if (p_extension == ext) {
			return { "//", "/*", "*/" };
		}
	}
	return {};
}

} // namespace

void GitDiffHighlighter::setup(const Ref<SyntaxHighlighter> &p_code, const PackedByteArray &p_kinds, const Color &p_header_color) {
	code = p_code;
	kinds = p_kinds;
	header_color = p_header_color;
	clear_highlighting_cache();
}

Dictionary GitDiffHighlighter::_get_line_syntax_highlighting(int32_t p_line) const {
	if (p_line < 0 || p_line >= kinds.size() || kinds[p_line] == GitDiffDock::ROW_FILLER) {
		return Dictionary();
	}
	if (kinds[p_line] == GitDiffDock::ROW_HEADER) {
		Dictionary color;
		color["color"] = header_color;
		Dictionary result;
		result[0] = color;
		return result;
	}
	return code.is_valid() ? code->get_line_syntax_highlighting(p_line) : Dictionary();
}

void GitDiffDock::Rows::add(const String &p_text, RowKind p_kind, int p_old, int p_new) {
	text.push_back(p_text);
	kinds.push_back(p_kind);
	old_numbers.push_back(p_old);
	new_numbers.push_back(p_new);
}

void GitDiffDock::_make_pane(PaneIndex p_index, Control *p_parent, int p_number_gutters) {
	Pane &pane = panes[p_index];

	CodeEdit *edit = memnew(CodeEdit);
	edit->set_editable(false);
	edit->set_v_size_flags(SIZE_EXPAND_FILL);
	edit->set_draw_line_numbers(false);
	edit->set_draw_bookmarks_gutter(false);
	edit->set_draw_breakpoints_gutter(false);
	edit->set_draw_executing_lines_gutter(false);
	edit->set_draw_fold_gutter(false);
	edit->set_line_folding_enabled(false);
	edit->set_highlight_current_line(false);
	edit->set_highlight_all_occurrences(true);
	edit->set_deselect_on_focus_loss_enabled(true);
	// The gutters (tint, numbers, +/−) are drawn on the CodeEdit's own canvas item, which isn't
	// clipped, while TextEdit draws its rows on an inner one clipped to the control. A row cut off
	// at the bottom then had its gutter part hang out past the code, into the frame's padding.
	edit->set_clip_contents(true);
	// Line numbers (old and new in the unified view), then the +/− column. Drawn by _draw_gutter.
	pane.number_gutters = p_number_gutters;
	pane.first_gutter = edit->get_gutter_count();
	for (int i = 0; i <= p_number_gutters; i++) {
		edit->add_gutter();
		const int gutter = edit->get_gutter_count() - 1;
		edit->set_gutter_type(gutter, TextEdit::GUTTER_TYPE_CUSTOM);
		edit->set_gutter_custom_draw(gutter, callable_mp(this, &GitDiffDock::_draw_gutter).bind(p_index));
	}
	pane.frame = memnew(PanelContainer);
	pane.frame->set_v_size_flags(SIZE_EXPAND_FILL);
	p_parent->add_child(pane.frame);
	pane.frame->add_child(edit);
	pane.edit = edit;

	pane.mirror = memnew(CodeEdit);
	pane.mirror->hide();
	add_child(pane.mirror);

	pane.highlighter.instantiate();
	edit->set_syntax_highlighter(pane.highlighter);
}

// The rows of both views. The unified view lists each hunk's lines in order. The split view puts
// each run of removed lines next to the added lines that replace it, with blank fillers where
// one side has more, so the two columns stay aligned.
void GitDiffDock::_build_rows(Rows &r_unified, Rows &r_old, Rows &r_new) const {
	const Array hunks = diff.get("hunks", Array());
	for (int h = 0; h < hunks.size(); h++) {
		const Dictionary hunk = hunks[h];
		const PackedByteArray origins = hunk["origins"];
		const PackedInt32Array old_numbers = hunk["old_numbers"];
		const PackedInt32Array new_numbers = hunk["new_numbers"];
		const PackedStringArray lines = hunk["text"];

		// A new or deleted file is one hunk of the whole file; "@@ -0,0 +1,9 @@" says nothing.
		if ((int)hunk["old_lines"] > 0 && (int)hunk["new_lines"] > 0) {
			const String context = hunk["context"];
			const String header_text = vformat("@@ -%d,%d +%d,%d @@%s", (int)hunk["old_start"], (int)hunk["old_lines"], (int)hunk["new_start"], (int)hunk["new_lines"], context.is_empty() ? String() : " " + context);
			r_unified.add(header_text, ROW_HEADER, -1, -1);
			r_old.add(header_text, ROW_HEADER, -1, -1);
			r_new.add(header_text, ROW_HEADER, -1, -1);
		}

		for (int i = 0; i < origins.size(); i++) {
			const RowKind kind = origins[i] == '+' ? ROW_ADDED : (origins[i] == '-' ? ROW_REMOVED : ROW_CONTEXT);
			r_unified.add(lines[i], kind, old_numbers[i], new_numbers[i]);
		}

		int i = 0;
		while (i < origins.size()) {
			if (origins[i] == ' ') {
				r_old.add(lines[i], ROW_CONTEXT, old_numbers[i], -1);
				r_new.add(lines[i], ROW_CONTEXT, -1, new_numbers[i]);
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
			const int removed = added_start - removed_start;
			const int added = i - added_start;
			for (int r = 0; r < MAX(removed, added); r++) {
				if (r < removed) {
					r_old.add(lines[removed_start + r], ROW_REMOVED, old_numbers[removed_start + r], -1);
				} else {
					r_old.add(String(), ROW_FILLER, -1, -1);
				}
				if (r < added) {
					r_new.add(lines[added_start + r], ROW_ADDED, -1, new_numbers[added_start + r]);
				} else {
					r_new.add(String(), ROW_FILLER, -1, -1);
				}
			}
		}
	}
}

void GitDiffDock::_fill_pane(Pane &r_pane, const Rows &p_rows) {
	CodeEdit *edit = r_pane.edit;
	const String path = diff.get("path", String());
	const double scroll = r_pane.path == path ? edit->get_v_scroll() : 0.0;
	r_pane.path = path;
	r_pane.kinds = p_rows.kinds;
	r_pane.old_numbers = p_rows.old_numbers;
	r_pane.new_numbers = p_rows.new_numbers;

	// The highlighter reads the mirror, where hunk headers and fillers are blank lines. Plain text
	// has no highlighter, and then no mirror text either: setting a long text costs real time.
	const Ref<SyntaxHighlighter> code = p_rows.text.is_empty() ? Ref<SyntaxHighlighter>() : make_code_highlighter(diff.get("path", String()));
	PackedStringArray code_lines;
	if (code.is_valid()) {
		code_lines = p_rows.text;
		for (int i = 0; i < code_lines.size(); i++) {
			if (p_rows.kinds[i] == ROW_HEADER || p_rows.kinds[i] == ROW_FILLER) {
				code_lines.set(i, String());
			}
		}
	}
	r_pane.mirror->set_syntax_highlighter(code);
	r_pane.mirror->set_text(String("\n").join(code_lines));
	r_pane.highlighter->setup(code, p_rows.kinds, theme.dim);

	edit->set_text(String("\n").join(p_rows.text));
	edit->clear_undo_history();
	for (int i = 0; i < p_rows.kinds.size(); i++) {
		edit->set_line_background_color(i, theme.row_background[p_rows.kinds[i]]);
	}

	// Number columns as wide as the largest number in them.
	int largest = 0;
	for (const PackedInt32Array *numbers : { &p_rows.old_numbers, &p_rows.new_numbers }) {
		for (int i = 0; i < numbers->size(); i++) {
			largest = MAX(largest, (*numbers)[i]);
		}
	}
	const int digits = MAX(2, itos(largest).length());
	for (int i = 0; i < r_pane.number_gutters; i++) {
		edit->set_gutter_width(r_pane.first_gutter + i, (int)(theme.digit_width * digits + 16 * theme.scale));
	}

	if (scroll > 0) {
		edit->set_v_scroll(scroll);
	}
}

// The script editor's highlighting for GDScript, a generic one (strings, numbers, comments) for
// other code and Godot's text formats, none for plain text.
// The script editor's highlighting for a file of p_path's type: Godot's own for GDScript, colored
// strings, numbers and comments for other code, none for plain text. Also used by the script
// editor's change preview (GitScriptMarks).
Ref<SyntaxHighlighter> GitDiffDock::make_code_highlighter(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	if (extension == "gd") {
		Ref<GDScriptSyntaxHighlighter> gdscript;
		gdscript.instantiate();
		return gdscript;
	}
	if (extension.is_empty() || extension == "txt" || extension == "md") {
		return Ref<SyntaxHighlighter>();
	}
	Ref<CodeHighlighter> code;
	code.instantiate();
	code->set_number_color(highlighting_color("number_color"));
	code->set_symbol_color(highlighting_color("symbol_color"));
	code->set_function_color(highlighting_color("function_color"));
	code->set_member_variable_color(highlighting_color("member_variable_color"));
	code->add_color_region("\"", "\"", highlighting_color("string_color"), false);
	const CommentStyle comments = comment_style(extension);
	const Color comment_color = highlighting_color("comment_color");
	if (comments.line) {
		code->add_color_region(comments.line, "", comment_color, true);
	}
	if (comments.block_start) {
		code->add_color_region(comments.block_start, comments.block_end, comment_color, false);
	}
	return code;
}

// Paints one gutter cell: a line number (old or new), or the +/− sign, on the row's tint so the
// whole row reads as one band.
void GitDiffDock::_draw_gutter(int p_line, int p_gutter, const Rect2 &p_region, int p_pane) {
	const Pane &pane = panes[p_pane];
	if (p_line < 0 || p_line >= pane.kinds.size()) {
		return;
	}
	const RowKind kind = (RowKind)pane.kinds[p_line];
	const RID canvas = pane.edit->get_canvas_item();
	const int gutter = p_gutter - pane.first_gutter;
	const Color &background = theme.row_background[kind];
	if (background.a > 0) {
		Rect2 tint = p_region;
		if (gutter == pane.number_gutters) {
			// TextEdit leaves a gap (gutter_padding, 2 px in 4.7.2) between the last gutter and
			// the text, which nobody tints: a dark seam through every tinted row. The last
			// column's tint covers it.
			int drawn = 0;
			for (int g = 0; g < pane.edit->get_gutter_count(); g++) {
				drawn += pane.edit->is_gutter_drawn(g) ? pane.edit->get_gutter_width(g) : 0;
			}
			tint.size.x += pane.edit->get_total_gutter_width() - drawn;
		}
		RenderingServer::get_singleton()->canvas_item_add_rect(canvas, tint, background);
	}
	const Color color = kind == ROW_ADDED ? theme.added : (kind == ROW_REMOVED ? theme.removed : theme.line_number);
	const float y = p_region.position.y + (p_region.size.y - theme.height) / 2 + theme.ascent;

	if (gutter < pane.number_gutters) {
		// Unified: old, then new. Split: each side has one, for its own numbers.
		const bool new_side = p_pane == PANE_NEW || gutter == 1;
		const int number = (new_side ? pane.new_numbers : pane.old_numbers)[p_line];
		if (number >= 0) {
			const float width = p_region.size.x - 8 * theme.scale; // A gap before the next column.
			theme.font->draw_string(canvas, Vector2(p_region.position.x, y), itos(number), HORIZONTAL_ALIGNMENT_RIGHT, width, theme.font_size, color);
		}
		return;
	}
	if (kind == ROW_ADDED || kind == ROW_REMOVED) {
		theme.font->draw_string(canvas, Vector2(p_region.position.x, y), kind == ROW_ADDED ? String("+") : minus(), HORIZONTAL_ALIGNMENT_CENTER, p_region.size.x, theme.font_size, color);
	}
}

void GitDiffDock::_on_scrolled(double p_value, int p_from) {
	if (syncing_scroll) {
		return;
	}
	syncing_scroll = true;
	panes[p_from == PANE_OLD ? PANE_NEW : PANE_OLD].edit->set_v_scroll(p_value);
	syncing_scroll = false;
}
