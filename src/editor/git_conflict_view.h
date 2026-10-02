#pragma once

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/syntax_highlighter.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/v_split_container.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

using namespace godot;

// The Git Diff panel's resolver, for a file with conflicts (GitRepository::get_conflict): the
// current conflict's two sides next to each other at the top, and the result (the whole file,
// editable) below. The result marks what's still undecided with header rows of its own;
// choosing a side replaces one block (one undoable edit), and anything can be edited by hand.
// Nothing is written until Resolve File, which needs every marker gone. Binary files, LFS files
// and a side that deleted the file can only be taken whole.
class GitConflictView : public VBoxContainer {
	GDCLASS(GitConflictView, VBoxContainer)

	// A conflict block in the result, by line: its "Mine" row, its "Theirs" row, its last row.
	struct Block {
		int id = -1; // Which of get_conflict's blocks it is (see encode_id).
		int start = 0;
		int middle = 0;
		int end = 0;
	};

	Dictionary conflict;
	String path;
	LocalVector<Block> blocks;
	int current = 0;
	PackedStringArray bases; // The original lines of each of get_conflict's blocks, by id.
	PackedByteArray side_kinds[2]; // What each side pane's rows are (SideRow), for the gutter.
	int stray_markers = 0; // Marker rows that don't make up a whole block (see _parse).
	bool updating = false;

	Control *toolbar = nullptr;
	Button *prev_button = nullptr;
	Label *position_label = nullptr;
	Button *next_button = nullptr;
	Button *choice_buttons[4] = {};
	Button *all_mine_button = nullptr;
	Button *all_theirs_button = nullptr;
	Button *resolve_button = nullptr;

	VSplitContainer *split = nullptr;
	Label *mine_caption = nullptr;
	Label *theirs_caption = nullptr;
	CodeEdit *mine_edit = nullptr;
	CodeEdit *theirs_edit = nullptr;
	Label *result_caption = nullptr;
	CodeEdit *result_edit = nullptr;
	PanelContainer *frames[3] = {};

	Control *whole_view = nullptr; // Instead of the split, when only whole sides can be taken.
	Label *whole_label = nullptr;
	Button *whole_mine_button = nullptr;
	Button *whole_theirs_button = nullptr;

	CodeEdit *_make_code(Control *p_parent, int p_frame, bool p_editable, Label **r_caption);
	void _update_theme();
	void _parse();
	void _show_current(bool p_scroll);
	void _on_result_changed();
	void _choose(int p_choice);
	void _go(int p_step);
	void _take_whole(const String &p_side);
	void _on_resolve();
	String _lines(int p_from, int p_to) const;
	void _draw_sign(int p_line, int p_gutter, const Rect2 &p_region, int p_side);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	// Shows p_conflict (GitRepository::get_conflict), starting over: what was chosen or typed
	// for another file is dropped.
	void set_conflict(const Dictionary &p_conflict, const Ref<SyntaxHighlighter> &p_result_highlighter, const Ref<SyntaxHighlighter> &p_mine_highlighter, const Ref<SyntaxHighlighter> &p_theirs_highlighter);
	String get_path() const { return path; }

	GitConflictView();
};
