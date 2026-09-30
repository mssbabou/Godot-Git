#pragma once

#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/editor_context_menu_plugin.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/timer.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>

#include "git/git_repository.h"

using namespace godot;

// Change marks in the script editor: a thin column beside the line numbers showing which lines
// were added, changed or deleted since the last commit. They come from the editor's own text, so
// unsaved edits count, and are worked out in memory as you type (GitRepository::diff_lines).
// Clicking a mark opens GitChangePreview under the change. Every open script tab gets them.
//
// The column, its draw callback and our signal connections live on the script editor's CodeEdits,
// which outlive this library when the addon goes away (see gotcha 30), so everything is taken off
// again when this node leaves the tree.
class GitScriptMarks : public Node {
	GDCLASS(GitScriptMarks, Node)

	enum LineMark : uint8_t {
		MARK_NONE,
		MARK_ADDED,
		MARK_CHANGED,
	};

	// One script tab's CodeEdit.
	struct Tab {
		String path; // In the repository.
		String base_head; // The HEAD commit `base` was read from ("" until read).
		bool committed = false; // In HEAD as a text file; otherwise nothing is marked.
		String base; // Its text in HEAD.
		Array hunks; // GitRepository::diff_lines(base, the editor's text).
		PackedByteArray marks; // LineMark per line.
		PackedByteArray deleted_after; // 1 where lines were deleted below this line.
		bool deleted_at_top = false;
		PackedInt32Array hunk_at; // The hunk (index in hunks) a line's mark belongs to, or -1.
		Callable on_text_changed;
		Callable on_gutter_clicked;
	};

	Ref<GitRepository> repo;
	String head; // HEAD's commit id; the committed text is read again when it changes.
	bool enabled = false;
	HashMap<uint64_t, Tab> tabs; // By the CodeEdit's instance id.
	HashSet<uint64_t> changed; // Tabs typed in since their marks were worked out.
	Timer *update_timer = nullptr;
	ObjectID preview;

	String _repo_path(const String &p_res_path) const;
	CodeEdit *_code_edit(uint64_t p_id) const;
	int _gutter(CodeEdit *p_code_edit) const;
	void _install(CodeEdit *p_code_edit, Tab &r_tab);
	void _uninstall(CodeEdit *p_code_edit, Tab &r_tab);
	void _uninstall_all();
	void _update(uint64_t p_id);
	void _on_text_changed(uint64_t p_id);
	void _on_update_timer();
	void _draw_mark(int p_line, int p_gutter, const Rect2 &p_rect, uint64_t p_id);
	void _on_gutter_clicked(int p_line, int p_gutter, uint64_t p_id);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	// The dock's repository (null while there's none) and HEAD's commit. Called on every refresh.
	void set_repository(const Ref<GitRepository> &p_repo, const String &p_head);
	void set_enabled(bool p_enabled);
	// Finds new script tabs and forgets closed ones.
	void sync();

	// For GitChangePreview and GitScriptMenu.
	Array get_hunks(CodeEdit *p_code_edit) const;
	int get_hunk_at(CodeEdit *p_code_edit, int p_line) const;
	String get_path(CodeEdit *p_code_edit) const;
	void show_preview(CodeEdit *p_code_edit, int p_hunk);
	void close_preview();
	void revert(CodeEdit *p_code_edit, int p_hunk);
	void show_in_diff(CodeEdit *p_code_edit, int p_hunk);

	GitScriptMarks();
};

// What a change replaced, under it in the script editor: the old lines colored like the code, with
// Revert Change, Show in Diff and the previous / next change. A child of the CodeEdit rather than
// a popup window, so it scrolls with the code and can't open on the wrong screen.
class GitChangePreview : public PanelContainer {
	GDCLASS(GitChangePreview, PanelContainer)

	GitScriptMarks *marks = nullptr;
	CodeEdit *code_edit = nullptr;
	int hunk = -1;
	int first_line = 0; // Of the change in the editor (0-based)...
	int last_line = 0; // ...and the line it opens under.
	uint64_t opened_at = 0;

	void _reposition();
	void _on_caret_changed();
	void _on_code_edit_input(const Ref<InputEvent> &p_event);

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	void open(GitScriptMarks *p_marks, CodeEdit *p_code_edit, int p_hunk);
	void close();
};

// The script editor's right-click items: the change at the caret, the commit of its line, and the
// file's history.
class GitScriptMenu : public EditorContextMenuPlugin {
	GDCLASS(GitScriptMenu, EditorContextMenuPlugin)

	ObjectID marks;

	GitScriptMarks *_get_marks() const;
	static CodeEdit *_code_edit_from(const Variant &p_target);
	void _show_change(const Variant &p_target);
	void _revert(const Variant &p_target);
	void _show_in_diff(const Variant &p_target);
	void _line_commit(const Variant &p_target);
	void _file_history(const Variant &p_target);

protected:
	static void _bind_methods() {}

public:
	void set_marks(GitScriptMarks *p_marks);
	void _popup_menu(const PackedStringArray &p_paths) override;
};
