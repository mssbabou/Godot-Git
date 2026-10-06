#pragma once

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/editor_dock.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/menu_button.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/syntax_highlighter.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include "editor/git_conflict_view.h"

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
// It only shows what the Git dock hands it (see GitDock::_update_diff), and never reads the
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
		ROW_KIND_COUNT,
	};

private:
	enum View {
		VIEW_UNIFIED,
		VIEW_SPLIT,
		VIEW_IMAGE, // Before | after, for images. Not saved: images always open this way.
		VIEW_SETTINGS, // Setting by setting, for `.import` and `.uid` files. Not saved either.
		VIEW_AUDIO, // Before | after, for sounds: length, waveform, play.
	};

	enum PaneIndex {
		PANE_UNIFIED,
		PANE_OLD,
		PANE_NEW,
		PANE_COUNT,
	};

	// A hunk's button on its header row ("Stage Hunk"), where it was last drawn.
	struct HunkButton {
		Rect2 rect;
		int hunk = -1;
		String action;
	};

	// One CodeEdit showing a diff, with its gutters.
	struct Pane {
		PanelContainer *frame = nullptr; // Draws the editor's background and padding (see _update_theme).
		CodeEdit *edit = nullptr;
		CodeEdit *mirror = nullptr; // Hidden: the text the syntax highlighter reads (see GitDiffHighlighter).
		Ref<GitDiffHighlighter> highlighter;
		String path; // The file shown, to keep the scroll position when the same file is redrawn.
		PackedByteArray kinds;
		PackedInt32Array old_numbers; // All -1 on the split view's new side.
		PackedInt32Array new_numbers; // All -1 on the split view's old side.
		Array words; // Per row: the changed words' [start, end) columns, pairs in a PackedInt32Array.
		PackedInt32Array hunks; // Per row: its hunk in diff["hunks"] (-1: none).
		PackedInt32Array lines; // Per row: its line in that hunk (-1: a header or filler).
		LocalVector<HunkButton> buttons;
		int number_gutters = 0; // 2 in the unified view (old and new), 1 in the split view.
		int first_gutter = 0; // Ours come after CodeEdit's own (breakpoints, line numbers, ...), which are hidden.
	};

	// One side of the image view: a caption ("Before · 512×512 · 34 KB"), the picture on a
	// checkerboard (drawn by _draw_image_side, so the checkerboard is exactly as big as the
	// picture), or a note instead ("Deleted.").
	struct ImageSide {
		Label *caption = nullptr;
		PanelContainer *frame = nullptr;
		Control *picture = nullptr;
		Label *note = nullptr;
		Ref<Texture2D> texture;
	};

	// One side of the audio view: play button and caption ("Before · 2.4 s · 44.1 kHz · mono"),
	// the waveform (drawn by _draw_audio_side), or a note instead ("Deleted.").
	struct AudioSide {
		Button *play = nullptr;
		Label *caption = nullptr;
		PanelContainer *frame = nullptr;
		Control *wave = nullptr;
		Label *note = nullptr;
		Ref<AudioStream> stream;
		PackedFloat32Array peaks;
	};

	// The lines of one view, built from the hunks by _build_rows.
	struct Rows {
		PackedStringArray text;
		PackedByteArray kinds;
		PackedInt32Array old_numbers;
		PackedInt32Array new_numbers;
		Array words; // See Pane::words.
		PackedInt32Array hunks;
		PackedInt32Array lines;
		void add(const String &p_text, RowKind p_kind, int p_old, int p_new, const PackedInt32Array &p_words, int p_hunk, int p_line);
	};

	// Theme values, read once per theme change: the gutters are drawn for every visible row.
	struct ThemeCache {
		Color row_background[ROW_KIND_COUNT];
		Color added;
		Color removed;
		Color dim;
		Color line_number;
		Ref<Font> font;
		Ref<Texture2D> checkerboard;
		int font_size = 0;
		float ascent = 0;
		float height = 0;
		float digit_width = 0;
		float scale = 1;
	} theme;

	Dictionary diff; // GitRepository::get_diff() or get_commit_diff(), or empty for nothing.
	String source; // "Unstaged", "Staged", "Commit 4dff129".
	Ref<Texture2D> file_icon;

	TextureRect *icon_rect = nullptr;
	Label *name_label = nullptr;
	Label *folder_label = nullptr;
	Label *source_label = nullptr;
	Label *status_label = nullptr;
	HBoxContainer *counts_box = nullptr; // "+8 −6".
	Button *copy_hash_button = nullptr; // A commit's short hash after "Commit"; click copies the full one.
	Label *added_label = nullptr;
	Label *removed_label = nullptr;
	OptionButton *view_select = nullptr;
	// Context lines (3, 10, 25, the whole file) and ignoring whitespace, saved per project.
	MenuButton *options_button = nullptr;
	int context_lines = 3;
	bool ignore_whitespace = false;
	Button *open_button = nullptr;
	Control *header = nullptr;

	View text_view = VIEW_UNIFIED; // Unified or side by side, saved per project.
	bool as_text = false; // A file with a view of its own (an SVG, a `.import`) shown as text, by choice.

	Control *unified_view = nullptr;
	Control *split_view = nullptr;
	Control *image_view = nullptr;
	Pane panes[PANE_COUNT];
	ImageSide image_sides[2];
	Control *audio_view = nullptr;
	AudioSide audio_sides[2];
	AudioStreamPlayer *audio_player = nullptr;
	int audio_playing = -1; // The side playing, or -1.
	Control *settings_view = nullptr;
	Tree *settings_tree = nullptr;
	// Under any view: the settings of the file's companions (`player.png.import`, `player.gd.uid`),
	// which the Git dock lists on the file's row.
	Control *companion_view = nullptr;
	ScrollContainer *companion_scroll = nullptr;
	Tree *companion_tree = nullptr;
	Label *message_label = nullptr;
	Control *message_view = nullptr;
	// A conflicted file (the Git dock marks it "!"): the resolver instead of a diff.
	GitConflictView *conflict_view = nullptr;
	Dictionary conflict_shown; // What the resolver was last set to; anything else starts it over.
	bool syncing_scroll = false;
	int hovered_button_pane = -1; // The hunk button under the mouse.
	int hovered_button = -1;

	void _update_theme();
	void _render();
	View _current_view() const;
	void _update_header();
	String _empty_text() const;
	void _on_view_selected(int p_index);
	void _on_open_pressed();

	// git_diff_dock_text.cpp
	void _make_pane(PaneIndex p_index, Control *p_parent, int p_number_gutters);
	void _build_rows(Rows &r_unified, Rows &r_old, Rows &r_new) const;
	void _fill_pane(Pane &r_pane, const Rows &p_rows);
	void _draw_gutter(int p_line, int p_gutter, const Rect2 &p_region, int p_pane);
	void _on_scrolled(double p_value, int p_from);
	void _draw_words(int p_pane);
	void _on_option(int p_id);
	void _update_options_menu();

	// git_diff_dock_lines.cpp
	PackedStringArray _line_actions() const;
	Array _lines_for(const HashSet<int64_t> &p_entries) const;
	Array _hunk_lines(int p_hunk) const;
	Array _selected_lines(int p_pane) const;
	void _draw_hunk_buttons(int p_pane);
	void _on_pane_input(const Ref<InputEvent> &p_event, int p_pane);
	void _on_pane_mouse_exited(int p_pane);
	void _update_pane_menu(int p_pane);
	void _on_pane_menu(int p_id, int p_pane);

	// git_diff_dock_images.cpp
	void _make_image_side(int p_index, Control *p_parent);
	void _show_images();
	void _draw_image_side(int p_index);

	// git_diff_dock_audio.cpp
	void _make_audio_side(int p_index, Control *p_parent);
	void _show_audio();
	void _draw_audio_side(int p_index);
	void _on_audio_play(int p_index);
	void _stop_audio();
	void _update_audio_buttons();
	void _process_audio();

	// git_diff_dock_settings.cpp
	Tree *_make_settings_tree(Control *p_parent);
	void _show_settings();
	void _fill_settings(Tree *p_tree, TreeItem *p_parent, const Dictionary &p_diff);
	void _fit_companions();
	void _on_copy_hash();
	void _show_hash_label();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	// Shows p_diff (GitRepository::get_diff or get_commit_diff) for p_source ("Unstaged",
	// "Staged", "Commit 4dff129"). Does nothing when it's what's already shown, so a refresh keeps
	// the scroll position and selection; a changed diff of the same file keeps the scroll position.
	// For images, p_diff also holds both versions ("image_old", "image_new":
	// GitRepository::get_file_bytes), shown before | after instead of "binary file"; sounds the
	// same way ("audio_old", "audio_new"). "companions":
	// the diffs of the file's `.import` / `.uid` companions, shown setting by setting under it.
	// A dictionary with "conflict" (GitRepository::get_conflict, marked by the Git dock) opens the
	// resolver instead.
	void set_diff(const Dictionary &p_diff, const String &p_source, const Ref<Texture2D> &p_icon);
	void scroll_to_line(int p_new_line);

	// Whether the Diff panel shows p_path as an image (by its extension).
	static bool is_image_path(const String &p_path);
	// Whether it shows p_path as a sound (wav, ogg, mp3).
	static bool is_audio_path(const String &p_path);
	static Ref<SyntaxHighlighter> make_code_highlighter(const String &p_path);
	static Color row_tint(bool p_added);
	// A `.import` setting as the Import dock shows it (see git_diff_dock_settings.cpp).
	static String setting_display_name(const String &p_section, const String &p_key);
	static String setting_display_value(const String &p_importer, const String &p_section, const String &p_key, const String &p_raw, String &r_full);
	static bool is_generated_setting(const String &p_section, const String &p_key);
	GitConflictView *get_conflict_view() const { return conflict_view; }
	// The diff options the Git dock asks the repository for (see GitRepository::set_diff_options);
	// "options_changed" when they change.
	int get_context_lines() const { return context_lines; }
	bool is_ignoring_whitespace() const { return ignore_whitespace; }

	GitDiffDock();
};
