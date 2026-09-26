#pragma once

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/editor_dock.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/syntax_highlighter.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// Highlights one side of a diff: code lines like the script editor would, hunk headers dimmed.
// A syntax highlighter reads the lines it colors from its own TextEdit, so the code highlighter
// (GDScript's, or a generic one) is attached to a hidden copy of the text instead, in which the
// hunk headers are blank lines that can't confuse it (e.g. look like an unclosed string).
class GitDiffHighlighter : public SyntaxHighlighter {
	GDCLASS(GitDiffHighlighter, SyntaxHighlighter)

	Ref<SyntaxHighlighter> code;
	PackedByteArray kinds;
	Color header_color;

protected:
	static void _bind_methods() {}

public:
	void setup(const Ref<SyntaxHighlighter> &p_code, const PackedByteArray &p_kinds, const Color &p_header_color);
	Dictionary _get_line_syntax_highlighting(int32_t p_line) const override;
};

// The "Diff" panel at the bottom of the editor: the changes to the file picked in the Git dock.
// It only shows what the Git dock hands it (see GitDock::_show_diff), and never reads the
// repository itself.
class GitDiffDock : public EditorDock {
	GDCLASS(GitDiffDock, EditorDock)

public:
	// What a line of the diff view is.
	enum RowKind : uint8_t {
		ROW_CONTEXT,
		ROW_ADDED,
		ROW_REMOVED,
		ROW_HEADER, // "@@ -12,7 +12,9 @@ func _ready():" before each hunk.
		ROW_FILLER, // Split view: the blank opposite a line that only exists on the other side.
	};

private:
	enum View {
		VIEW_UNIFIED,
		VIEW_SPLIT,
	};

	// One CodeEdit showing a diff, with its gutters.
	struct Pane {
		PanelContainer *frame = nullptr; // Draws the editor's background and padding (see _style_pane).
		CodeEdit *edit = nullptr;
		CodeEdit *mirror = nullptr; // Hidden: the text the syntax highlighter reads (see GitDiffHighlighter).
		Ref<GitDiffHighlighter> highlighter;
		PackedByteArray kinds;
		PackedInt32Array old_numbers;
		PackedInt32Array new_numbers;
		int number_gutter_count = 0; // 2 in the unified view (old and new), 1 in the split view.
		int first_gutter = 0; // Ours come after CodeEdit's own (breakpoints, line numbers, ...), which are hidden.
	};

	Dictionary diff; // GitRepository::get_diff(), or empty for nothing.
	String source; // "Unstaged", "Staged".
	Ref<Texture2D> file_icon;
	String message; // Shown instead of a diff when there's none (no file picked, binary, ...).

	TextureRect *icon_rect = nullptr;
	Label *name_label = nullptr;
	Label *folder_label = nullptr;
	Label *source_label = nullptr;
	Label *added_label = nullptr;
	Label *removed_label = nullptr;
	OptionButton *view_select = nullptr;
	Button *open_button = nullptr;
	Control *header = nullptr;

	Control *unified_view = nullptr;
	Control *split_view = nullptr;
	Pane unified;
	Pane split_old;
	Pane split_new;
	Label *message_label = nullptr;
	Control *message_view = nullptr;
	bool syncing_scroll = false;

	void _make_pane(Pane &r_pane, Control *p_parent, int p_number_gutters);
	void _style_pane(Pane &r_pane);
	void _fill_pane(Pane &r_pane, const PackedStringArray &p_text, const PackedByteArray &p_kinds, const PackedInt32Array &p_old, const PackedInt32Array &p_new);
	void _draw_gutter(int p_line, int p_gutter, const Rect2 &p_region, int p_pane);
	Pane *_pane(int p_index);
	void _on_scrolled(double p_value, int p_from);
	void _on_view_selected(int p_index);
	void _on_open_pressed();
	Ref<SyntaxHighlighter> _make_code_highlighter() const;
	Color _row_background(RowKind p_kind) const;
	Color _dim_color() const;
	void _render();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	// Shows p_diff (GitRepository::get_diff) for p_source ("Unstaged", "Staged"). Keeps the scroll
	// position when it's the same file and nothing changed.
	void set_diff(const Dictionary &p_diff, const String &p_source, const Ref<Texture2D> &p_icon);
	// Shows no diff, just p_message.
	void clear(const String &p_message);
	String get_path() const;

	GitDiffDock();
};
