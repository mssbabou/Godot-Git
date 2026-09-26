// The Diff panel: shows the changes to one file as the script editor would show code, with
// added and removed lines tinted, old and new line numbers, and a unified or side-by-side view.

#include "editor/git_diff_dock.h"

#include <godot_cpp/classes/center_container.hpp>
#include <godot_cpp/classes/code_highlighter.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/gd_script_syntax_highlighter.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_split_container.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/margin_container.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
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

// The image formats Godot can read from bytes, by extension.
const char *IMAGE_EXTENSIONS[] = { "png", "jpg", "jpeg", "webp", "bmp", "tga", "svg", "exr", "dds" };

// r_size is the image's own size; an SVG is rendered larger than that, so it stays sharp when
// it's shown bigger (it's a vector image).
Ref<Image> decode_image(const PackedByteArray &p_bytes, const String &p_extension, Size2i &r_size) {
	Ref<Image> image;
	image.instantiate();
	Error err = ERR_FILE_UNRECOGNIZED;
	if (p_extension == "png") {
		err = image->load_png_from_buffer(p_bytes);
	} else if (p_extension == "jpg" || p_extension == "jpeg") {
		err = image->load_jpg_from_buffer(p_bytes);
	} else if (p_extension == "webp") {
		err = image->load_webp_from_buffer(p_bytes);
	} else if (p_extension == "bmp") {
		err = image->load_bmp_from_buffer(p_bytes);
	} else if (p_extension == "tga") {
		err = image->load_tga_from_buffer(p_bytes);
	} else if (p_extension == "svg") {
		err = image->load_svg_from_buffer(p_bytes);
		r_size = err == OK ? image->get_size() : Size2i();
		const int largest = MAX(r_size.x, r_size.y);
		if (err == OK && largest > 0 && largest < 1024) {
			err = image->load_svg_from_buffer(p_bytes, 1024.0f / largest);
		}
		return err == OK && !image->is_empty() ? image : Ref<Image>();
	} else if (p_extension == "exr") {
		err = image->load_exr_from_buffer(p_bytes);
	} else if (p_extension == "dds") {
		err = image->load_dds_from_buffer(p_bytes);
	}
	r_size = err == OK ? image->get_size() : Size2i();
	return err == OK && !image->is_empty() ? image : Ref<Image>();
}

String file_size_text(int64_t p_bytes) {
	if (p_bytes < 1024) {
		return vformat("%d bytes", p_bytes);
	}
	if (p_bytes < 1024 * 1024) {
		return vformat("%d KB", (p_bytes + 512) / 1024);
	}
	return vformat("%.1f MB", p_bytes / (1024.0 * 1024.0));
}

} // namespace

bool GitDiffDock::is_image_path(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	for (const char *image_extension : IMAGE_EXTENSIONS) {
		if (extension == image_extension) {
			return true;
		}
	}
	return false;
}

void GitDiffDock::Rows::add(const String &p_text, RowKind p_kind, int p_old, int p_new) {
	text.push_back(p_text);
	kinds.push_back(p_kind);
	old_numbers.push_back(p_old);
	new_numbers.push_back(p_new);
}

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

	CenterContainer *center = memnew(CenterContainer);
	center->set_v_size_flags(SIZE_EXPAND_FILL);
	body->add_child(center);
	message_view = center;
	message_label = memnew(Label);
	message_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	center->add_child(message_label);
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

void GitDiffDock::_make_image_side(int p_index, Control *p_parent) {
	ImageSide &side = image_sides[p_index];
	VBoxContainer *column = memnew(VBoxContainer);
	column->set_h_size_flags(SIZE_EXPAND_FILL);
	p_parent->add_child(column);

	side.caption = memnew(Label);
	side.caption->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	column->add_child(side.caption);

	// The frame lays its children over each other: the picture, or the note.
	side.frame = memnew(PanelContainer);
	side.frame->set_v_size_flags(SIZE_EXPAND_FILL);
	column->add_child(side.frame);
	side.picture = memnew(Control);
	side.picture->connect("draw", callable_mp(this, &GitDiffDock::_draw_image_side).bind(p_index));
	side.frame->add_child(side.picture);
	side.note = memnew(Label);
	side.note->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	side.note->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	side.note->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	side.frame->add_child(side.note);
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

void GitDiffDock::_update_theme() {
	theme.added = get_theme_color("success_color", "Editor");
	theme.removed = get_theme_color("error_color", "Editor");
	theme.dim = get_theme_color("font_color", "Label") * Color(1, 1, 1, 0.55);
	theme.row_background[ROW_CONTEXT] = Color(0, 0, 0, 0);
	theme.row_background[ROW_ADDED] = theme.added * Color(1, 1, 1, 0.14);
	theme.row_background[ROW_REMOVED] = theme.removed * Color(1, 1, 1, 0.14);
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
	for (ImageSide &side : image_sides) {
		side.frame->add_theme_stylebox_override("panel", get_theme_stylebox("read_only", "CodeEdit"));
		side.caption->add_theme_color_override("font_color", theme.dim);
		side.note->add_theme_color_override("font_color", theme.dim);
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

void GitDiffDock::_render() {
	_update_header();

	const View view = _current_view();
	const String text = view == VIEW_IMAGE ? String() : _empty_text();
	message_label->set_text(text);
	message_view->set_visible(!text.is_empty());
	const bool split = view == VIEW_SPLIT;
	unified_view->set_visible(text.is_empty() && view == VIEW_UNIFIED);
	split_view->set_visible(text.is_empty() && split);
	image_view->set_visible(view == VIEW_IMAGE);
	_show_images();

	// Only the visible view holds lines; the others are emptied.
	Rows unified, old_side, new_side;
	if (text.is_empty() && view != VIEW_IMAGE) {
		_build_rows(unified, old_side, new_side);
	}
	_fill_pane(panes[PANE_UNIFIED], split ? Rows() : unified);
	_fill_pane(panes[PANE_OLD], split ? old_side : Rows());
	_fill_pane(panes[PANE_NEW], split ? new_side : Rows());
}

// Images open as images; an SVG (also text) can be switched to its text diff.
GitDiffDock::View GitDiffDock::_current_view() const {
	const bool has_lines = diff.get("kind", String()) == "text" && !Array(diff.get("hunks", Array())).is_empty();
	if (diff.has("image_new") && (!has_lines || !images_as_text)) {
		return VIEW_IMAGE;
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
	const Ref<SyntaxHighlighter> code = p_rows.text.is_empty() ? Ref<SyntaxHighlighter>() : _make_code_highlighter();
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
	const Color &background = theme.row_background[kind];
	if (background.a > 0) {
		RenderingServer::get_singleton()->canvas_item_add_rect(canvas, p_region, background);
	}
	const Color color = kind == ROW_ADDED ? theme.added : (kind == ROW_REMOVED ? theme.removed : theme.line_number);
	const float y = p_region.position.y + (p_region.size.y - theme.height) / 2 + theme.ascent;

	const int gutter = p_gutter - pane.first_gutter;
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

void GitDiffDock::_on_view_selected(int p_index) {
	const View view = (View)view_select->get_item_id(p_index);
	if (diff.has("image_new")) {
		images_as_text = view != VIEW_IMAGE; // Only a choice made on an image counts for images.
	}
	if (view != VIEW_IMAGE) {
		text_view = view;
		editor_settings()->set_project_metadata("godot_git", "diff_view", (int)view);
	}
	_render();
}

// The picture as large as fits, centered, on a checkerboard of exactly its size (so transparent
// parts show, and where the image ends is clear).
void GitDiffDock::_draw_image_side(int p_index) {
	const ImageSide &side = image_sides[p_index];
	if (side.texture.is_null()) {
		return;
	}
	const Size2 area = side.picture->get_size();
	const Size2 size = side.texture->get_size();
	const float scale = MIN(area.x / size.x, area.y / size.y);
	const Size2 shown = (size * scale).floor();
	const Rect2 rect(((area - shown) / 2).floor(), shown);
	side.picture->draw_texture_rect(theme.checkerboard, rect, true);
	side.picture->draw_texture_rect(side.texture, rect, false);
}

// Before | after for an image: each side's picture with its size, or why there's none.
void GitDiffDock::_show_images() {
	const String extension = String(diff.get("path", String())).get_extension().to_lower();
	Ref<Image> images[2];
	Size2i sizes[2];
	for (int i = 0; i < 2; i++) {
		ImageSide &side = image_sides[i];
		const Dictionary version = diff.get(i == 0 ? "image_old" : "image_new", Dictionary());
		String caption = i == 0 ? "Before" : "After";
		String note;
		if (_current_view() != VIEW_IMAGE) {
			// Not shown; drop the textures.
		} else if (!bool(version.get("exists", false))) {
			note = i == 0 ? "Not in the old version: this is a new file." : "Not in the new version: the file is deleted.";
		} else if (String(version.get("lfs", String())) == "missing") {
			note = "Stored with Git LFS, and this version hasn't been downloaded.";
		} else {
			const PackedByteArray bytes = version["bytes"];
			images[i] = decode_image(bytes, extension, sizes[i]);
			if (images[i].is_valid()) {
				caption += vformat(String::utf8(" · %d×%d · %s"), sizes[i].x, sizes[i].y, file_size_text(bytes.size()));
			} else {
				note = "Couldn't read this image.";
			}
		}
		side.caption->set_text(caption);
		side.note->set_text(note);
		side.note->set_visible(!note.is_empty());
		side.texture = images[i].is_valid() ? Ref<Texture2D>(ImageTexture::create_from_image(images[i])) : Ref<Texture2D>();
		// Small images are mostly pixel art: scaled up without blurring. SVGs are rendered large.
		const bool small = images[i].is_valid() && extension != "svg" && MAX(sizes[i].x, sizes[i].y) <= 256;
		side.picture->set_texture_filter(small ? TEXTURE_FILTER_NEAREST : TEXTURE_FILTER_LINEAR);
		side.picture->queue_redraw();
	}
	// Re-saved or re-compressed without a visible change: say so, or it looks like a missed edit.
	if (images[0].is_valid() && images[1].is_valid() && images[0]->get_size() == images[1]->get_size() && images[0]->get_format() == images[1]->get_format() && images[0]->get_data() == images[1]->get_data()) {
		image_sides[1].caption->set_text(image_sides[1].caption->get_text() + String::utf8(" · same pixels"));
		image_sides[1].caption->set_tooltip_text("Pixel for pixel the same image; only the file changed (saved again, or compressed differently).");
	} else {
		image_sides[1].caption->set_tooltip_text(String());
	}
}

void GitDiffDock::_on_open_pressed() {
	const String path = diff.get("path", String());
	if (!path.is_empty()) {
		emit_signal("open_requested", path);
	}
}
