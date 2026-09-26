// The Diff panel: shows the changes to one file as the script editor would show code, with
// added and removed lines tinted, old and new line numbers, and a unified or side-by-side view.

#include "editor/git_diff_dock.h"

#include <godot_cpp/classes/center_container.hpp>
#include <godot_cpp/classes/code_highlighter.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/gd_script_syntax_highlighter.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_split_container.hpp>
#include <godot_cpp/classes/margin_container.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scroll_bar.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/core/math.hpp>

#include "editor/ui_text.h"

using namespace godot_git;

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

namespace {

EditorSettings *editor_settings() {
	return *EditorInterface::get_singleton()->get_editor_settings();
}

Color highlighting_color(const String &p_name) {
	return editor_settings()->get_setting("text_editor/theme/highlighting/" + p_name);
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
	HBoxContainer *header_hb = memnew(HBoxContainer);
	main_vb->add_child(header_hb);
	header = header_hb;

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
	open_button->set_tooltip_text("Open the file");
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
	_make_pane(unified, unified_box, 2);

	HSplitContainer *split = memnew(HSplitContainer);
	split->set_v_size_flags(SIZE_EXPAND_FILL);
	body->add_child(split);
	split_view = split;
	_make_pane(split_old, split, 1);
	_make_pane(split_new, split, 1);
	split_old.frame->set_h_size_flags(SIZE_EXPAND_FILL);
	split_new.frame->set_h_size_flags(SIZE_EXPAND_FILL);
	// Both sides have the same number of rows (blank fillers opposite one-sided lines), so they
	// scroll together line for line.
	split_old.edit->get_v_scroll_bar()->connect("value_changed", callable_mp(this, &GitDiffDock::_on_scrolled).bind(1));
	split_new.edit->get_v_scroll_bar()->connect("value_changed", callable_mp(this, &GitDiffDock::_on_scrolled).bind(2));

	CenterContainer *center = memnew(CenterContainer);
	center->set_v_size_flags(SIZE_EXPAND_FILL);
	body->add_child(center);
	message_view = center;
	message_label = memnew(Label);
	message_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	center->add_child(message_label);

	message = "Click a file in the Git dock to see its changes here.";
}

void GitDiffDock::_make_pane(Pane &r_pane, Control *p_parent, int p_number_gutters) {
	const int index = &r_pane == &unified ? 0 : (&r_pane == &split_old ? 1 : 2);

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
	edit->set_context_menu_enabled(true);
	edit->set_deselect_on_focus_loss_enabled(true);
	// Line numbers (old and new in the unified view), then the +/− column. Drawn by _draw_gutter.
	r_pane.number_gutter_count = p_number_gutters;
	r_pane.first_gutter = edit->get_gutter_count();
	for (int i = 0; i <= p_number_gutters; i++) {
		edit->add_gutter();
		const int gutter = edit->get_gutter_count() - 1;
		edit->set_gutter_type(gutter, TextEdit::GUTTER_TYPE_CUSTOM);
		edit->set_gutter_custom_draw(gutter, callable_mp(this, &GitDiffDock::_draw_gutter).bind(index));
	}
	r_pane.frame = memnew(PanelContainer);
	r_pane.frame->set_v_size_flags(SIZE_EXPAND_FILL);
	p_parent->add_child(r_pane.frame);
	r_pane.frame->add_child(edit);
	r_pane.edit = edit;

	CodeEdit *mirror = memnew(CodeEdit);
	mirror->hide();
	add_child(mirror);
	r_pane.mirror = mirror;

	r_pane.highlighter.instantiate();
	edit->set_syntax_highlighter(r_pane.highlighter);
}

GitDiffDock::Pane *GitDiffDock::_pane(int p_index) {
	switch (p_index) {
		case 0:
			return &unified;
		case 1:
			return &split_old;
		default:
			return &split_new;
	}
}

void GitDiffDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			const int view = editor_settings()->get_project_metadata("godot_git", "diff_view", (int)VIEW_UNIFIED);
			view_select->select(view == VIEW_SPLIT ? VIEW_SPLIT : VIEW_UNIFIED);
			_render();
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			open_button->set_button_icon(get_theme_icon("Load", "EditorIcons"));
			const Color dim = _dim_color();
			folder_label->add_theme_color_override("font_color", dim);
			source_label->add_theme_color_override("font_color", dim);
			message_label->add_theme_color_override("font_color", dim);
			added_label->add_theme_color_override("font_color", get_theme_color("success_color", "Editor"));
			removed_label->add_theme_color_override("font_color", get_theme_color("error_color", "Editor"));
			for (Pane *pane : { &unified, &split_old, &split_new }) {
				_style_pane(*pane);
			}
			if (is_node_ready()) {
				_render(); // Row colors come from the theme.
			}
		} break;
	}
}

void GitDiffDock::_style_pane(Pane &r_pane) {
	// A TextEdit clips its lines to its whole rect, padding included, so a row scrolled half out
	// of view was drawn into the padding, up to the rounded edge (very visible with tinted rows).
	// The frame draws the code editor's background and padding instead, and the CodeEdit has
	// none, so rows are cut off at the inner edge.
	r_pane.frame->add_theme_stylebox_override("panel", get_theme_stylebox("read_only", "CodeEdit"));
	Ref<StyleBoxEmpty> none;
	none.instantiate();
	for (const char *name : { "normal", "read_only", "focus" }) {
		r_pane.edit->add_theme_stylebox_override(name, none);
	}
	// Read-only text is dimmed by default; this is for reading.
	r_pane.edit->add_theme_color_override("font_readonly_color", r_pane.edit->get_theme_color("font_color"));
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	const Ref<Font> font = r_pane.edit->get_theme_font("font");
	const int font_size = r_pane.edit->get_theme_font_size("font_size");
	const float digit = font->get_string_size("0", HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
	// The number columns' widths are set per diff, for the number of digits (see _fill_pane).
	r_pane.edit->set_gutter_width(r_pane.first_gutter + r_pane.number_gutter_count, (int)(digit * 2 + 6 * scale));
}

Color GitDiffDock::_dim_color() const {
	return get_theme_color("font_color", "Label") * Color(1, 1, 1, 0.55);
}

Color GitDiffDock::_row_background(RowKind p_kind) const {
	switch (p_kind) {
		case ROW_ADDED:
			return get_theme_color("success_color", "Editor") * Color(1, 1, 1, 0.14);
		case ROW_REMOVED:
			return get_theme_color("error_color", "Editor") * Color(1, 1, 1, 0.14);
		case ROW_HEADER:
			return get_theme_color("accent_color", "Editor") * Color(1, 1, 1, 0.1);
		case ROW_FILLER:
			return get_theme_color("font_color", "Label") * Color(1, 1, 1, 0.03);
		default:
			return Color(0, 0, 0, 0);
	}
}

// The script editor's highlighting for GDScript, a generic one (strings, numbers, comments) for
// other code and Godot's text formats, none for plain text.
Ref<SyntaxHighlighter> GitDiffDock::_make_code_highlighter() const {
	const String extension = String(diff.get("path", String())).get_extension().to_lower();
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
	const Color string_color = highlighting_color("string_color");
	code->add_color_region("\"", "\"", string_color, false);
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

void GitDiffDock::set_diff(const Dictionary &p_diff, const String &p_source, const Ref<Texture2D> &p_icon) {
	if (message.is_empty() && p_source == source && p_diff == diff) {
		return; // Nothing changed: keep the scroll position and selection.
	}
	diff = p_diff;
	source = p_source;
	file_icon = p_icon;
	message = String();
	_render();
}

void GitDiffDock::clear(const String &p_message) {
	if (message == p_message && diff.is_empty()) {
		return;
	}
	diff = Dictionary();
	source = String();
	file_icon = Ref<Texture2D>();
	message = p_message;
	_render();
}

String GitDiffDock::get_path() const {
	return diff.get("path", String());
}

void GitDiffDock::_render() {
	const String path = diff.get("path", String());
	const String name = path.get_file();
	const String kind = diff.get("kind", String());
	const Array hunks = diff.get("hunks", Array());

	// Header.
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
	const bool has_lines = kind == "text" && !hunks.is_empty();
	view_select->set_visible(has_lines);
	const bool deleted = diff.get("status", String()) == "deleted";
	open_button->set_disabled(deleted);
	open_button->set_tooltip_text(deleted ? vformat("%s is deleted, so there's nothing to open.", name) : String("Open the file"));

	// Nothing to show as lines: say why instead.
	String text = message;
	if (text.is_empty()) {
		const String status = diff.get("status", String());
		if (kind == "unchanged") {
			text = vformat("No %s changes to %s anymore.", source.to_lower(), name);
		} else if (kind == "binary") {
			text = vformat("%s is a binary file, so there are no lines to compare.", name);
		} else if (kind == "too_large") {
			text = vformat("%s is larger than 2 MB, too large to compare line by line.", name);
		} else if (kind == "lfs") {
			text = vformat("%s is stored with Git LFS, so its lines aren't compared.", name);
		} else if (!has_lines && status == "renamed") {
			text = vformat("Renamed from %s, with the same content.", old_path);
		} else if (!has_lines && (status == "new" || status == "untracked")) {
			text = vformat("%s is a new, empty file.", name);
		} else if (!has_lines && status == "deleted") {
			text = vformat("%s was an empty file and is deleted.", name);
		} else if (!has_lines) {
			text = "The content is the same; only the file's permissions changed.";
		}
	}
	message_label->set_text(text);
	message_view->set_visible(!text.is_empty());
	const bool split = view_select->get_selected_id() == VIEW_SPLIT;
	unified_view->set_visible(text.is_empty() && !split);
	split_view->set_visible(text.is_empty() && split);
	if (!text.is_empty()) {
		for (Pane *pane : { &unified, &split_old, &split_new }) {
			_fill_pane(*pane, PackedStringArray(), PackedByteArray(), PackedInt32Array(), PackedInt32Array());
		}
		return;
	}

	// Rows. The unified view lists each hunk's lines in order. The split view puts each run of
	// removed lines next to the added lines that replace it, with blank fillers where one side
	// has more, so the two columns stay aligned.
	PackedStringArray u_text, o_text, n_text;
	PackedByteArray u_kinds, o_kinds, n_kinds;
	PackedInt32Array u_old, u_new, o_numbers, n_numbers, none;
	auto add_split = [&](bool p_old, const String &p_text, RowKind p_kind, int p_number) {
		(p_old ? o_text : n_text).push_back(p_text);
		(p_old ? o_kinds : n_kinds).push_back(p_kind);
		(p_old ? o_numbers : n_numbers).push_back(p_number);
	};

	for (int h = 0; h < hunks.size(); h++) {
		const Dictionary hunk = hunks[h];
		const PackedByteArray origins = hunk["origins"];
		const PackedInt32Array old_numbers = hunk["old_numbers"];
		const PackedInt32Array new_numbers = hunk["new_numbers"];
		const PackedStringArray lines = hunk["text"];
		const String context = hunk["context"];
		const String header_text = vformat("@@ -%d,%d +%d,%d @@%s", (int)hunk["old_start"], (int)hunk["old_lines"], (int)hunk["new_start"], (int)hunk["new_lines"], context.is_empty() ? String() : " " + context);

		// A new or deleted file is one hunk of the whole file; "@@ -0,0 +1,9 @@" says nothing.
		if ((int)hunk["old_lines"] > 0 && (int)hunk["new_lines"] > 0) {
			u_text.push_back(header_text);
			u_kinds.push_back(ROW_HEADER);
			u_old.push_back(-1);
			u_new.push_back(-1);
			add_split(true, header_text, ROW_HEADER, -1);
			add_split(false, header_text, ROW_HEADER, -1);
		}

		for (int i = 0; i < origins.size(); i++) {
			const RowKind row = origins[i] == '+' ? ROW_ADDED : (origins[i] == '-' ? ROW_REMOVED : ROW_CONTEXT);
			u_text.push_back(lines[i]);
			u_kinds.push_back(row);
			u_old.push_back(old_numbers[i]);
			u_new.push_back(new_numbers[i]);
		}

		int i = 0;
		while (i < origins.size()) {
			if (origins[i] == ' ') {
				add_split(true, lines[i], ROW_CONTEXT, old_numbers[i]);
				add_split(false, lines[i], ROW_CONTEXT, new_numbers[i]);
				i++;
				continue;
			}
			LocalVector<int> removed_rows, added_rows;
			while (i < origins.size() && origins[i] == '-') {
				removed_rows.push_back(i++);
			}
			while (i < origins.size() && origins[i] == '+') {
				added_rows.push_back(i++);
			}
			const uint32_t rows = MAX(removed_rows.size(), added_rows.size());
			for (uint32_t r = 0; r < rows; r++) {
				if (r < removed_rows.size()) {
					add_split(true, lines[removed_rows[r]], ROW_REMOVED, old_numbers[removed_rows[r]]);
				} else {
					add_split(true, String(), ROW_FILLER, -1);
				}
				if (r < added_rows.size()) {
					add_split(false, lines[added_rows[r]], ROW_ADDED, new_numbers[added_rows[r]]);
				} else {
					add_split(false, String(), ROW_FILLER, -1);
				}
			}
		}
	}

	if (split) {
		_fill_pane(split_old, o_text, o_kinds, o_numbers, none);
		_fill_pane(split_new, n_text, n_kinds, none, n_numbers);
		_fill_pane(unified, PackedStringArray(), PackedByteArray(), none, none);
	} else {
		_fill_pane(unified, u_text, u_kinds, u_old, u_new);
		_fill_pane(split_old, PackedStringArray(), PackedByteArray(), none, none);
		_fill_pane(split_new, PackedStringArray(), PackedByteArray(), none, none);
	}
}

void GitDiffDock::_fill_pane(Pane &r_pane, const PackedStringArray &p_text, const PackedByteArray &p_kinds, const PackedInt32Array &p_old, const PackedInt32Array &p_new) {
	CodeEdit *edit = r_pane.edit;
	const String previous_path = edit->get_meta("git_diff_path", String());
	const String path = diff.get("path", String());
	const double scroll = previous_path == path ? edit->get_v_scroll() : 0.0;

	r_pane.kinds = p_kinds;
	r_pane.old_numbers = p_old;
	r_pane.new_numbers = p_new;

	// The highlighter reads the mirror, where hunk headers and fillers are blank lines.
	PackedStringArray code_lines = p_text;
	for (int i = 0; i < code_lines.size(); i++) {
		if (p_kinds[i] == ROW_HEADER || p_kinds[i] == ROW_FILLER) {
			code_lines.set(i, String());
		}
	}
	Ref<SyntaxHighlighter> code = p_text.is_empty() ? Ref<SyntaxHighlighter>() : _make_code_highlighter();
	r_pane.mirror->set_syntax_highlighter(code);
	r_pane.mirror->set_text(String("\n").join(code_lines));
	r_pane.highlighter->setup(code, p_kinds, _dim_color());

	edit->set_text(String("\n").join(p_text));
	edit->set_meta("git_diff_path", path);
	edit->clear_undo_history();

	for (int i = 0; i < p_kinds.size(); i++) {
		edit->set_line_background_color(i, _row_background((RowKind)p_kinds[i]));
	}

	// Number columns as wide as the largest number in them.
	int largest = 0;
	for (int i = 0; i < p_old.size(); i++) {
		largest = MAX(largest, p_old[i]);
	}
	for (int i = 0; i < p_new.size(); i++) {
		largest = MAX(largest, p_new[i]);
	}
	const Ref<Font> font = edit->get_theme_font("font");
	const int font_size = edit->get_theme_font_size("font_size");
	const float digit = font->get_string_size("0", HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	const int digits = MAX(2, itos(largest).length());
	for (int i = 0; i < r_pane.number_gutter_count; i++) {
		edit->set_gutter_width(r_pane.first_gutter + i, (int)(digit * digits + 16 * scale));
	}

	if (scroll > 0) {
		edit->set_v_scroll(scroll);
	}
}

// Paints one gutter cell: a line number (old or new), or the +/− sign, on the row's tint so the
// whole row reads as one band.
void GitDiffDock::_draw_gutter(int p_line, int p_gutter, const Rect2 &p_region, int p_pane) {
	const Pane *pane = _pane(p_pane);
	if (p_line < 0 || p_line >= pane->kinds.size()) {
		return;
	}
	CodeEdit *edit = pane->edit;
	const RowKind kind = (RowKind)pane->kinds[p_line];
	const RID canvas = edit->get_canvas_item();
	const Color background = _row_background(kind);
	if (background.a > 0) {
		RenderingServer::get_singleton()->canvas_item_add_rect(canvas, p_region, background);
	}

	const Ref<Font> font = edit->get_theme_font("font");
	const int font_size = edit->get_theme_font_size("font_size");
	Color color = edit->get_theme_color("line_number_color");
	if (kind == ROW_ADDED) {
		color = get_theme_color("success_color", "Editor");
	} else if (kind == ROW_REMOVED) {
		color = get_theme_color("error_color", "Editor");
	}

	const int gutter = p_gutter - pane->first_gutter;
	String text;
	if (gutter < pane->number_gutter_count) {
		const PackedInt32Array &numbers = (pane->number_gutter_count == 2 ? gutter == 0 : pane == &split_old) ? pane->old_numbers : pane->new_numbers;
		const int number = p_line < numbers.size() ? numbers[p_line] : -1;
		if (number < 0) {
			return;
		}
		text = itos(number);
		const float scale = EditorInterface::get_singleton()->get_editor_scale();
		const float width = p_region.size.x - 8 * scale; // A gap before the next column.
		const float y = p_region.position.y + (p_region.size.y - font->get_height(font_size)) / 2 + font->get_ascent(font_size);
		font->draw_string(canvas, Vector2(p_region.position.x, y), text, HORIZONTAL_ALIGNMENT_RIGHT, width, font_size, color);
		return;
	}

	if (kind == ROW_ADDED) {
		text = "+";
	} else if (kind == ROW_REMOVED) {
		text = minus();
	} else {
		return;
	}
	const float y = p_region.position.y + (p_region.size.y - font->get_height(font_size)) / 2 + font->get_ascent(font_size);
	font->draw_string(canvas, Vector2(p_region.position.x, y), text, HORIZONTAL_ALIGNMENT_CENTER, p_region.size.x, font_size, color);
}

void GitDiffDock::_on_scrolled(double p_value, int p_from) {
	if (syncing_scroll) {
		return;
	}
	syncing_scroll = true;
	CodeEdit *other = p_from == 1 ? split_new.edit : split_old.edit;
	other->set_v_scroll(p_value);
	syncing_scroll = false;
}

void GitDiffDock::_on_view_selected(int p_index) {
	editor_settings()->set_project_metadata("godot_git", "diff_view", view_select->get_item_id(p_index));
	_render();
}

void GitDiffDock::_on_open_pressed() {
	const String path = get_path();
	if (!path.is_empty()) {
		emit_signal("open_requested", path);
	}
}
