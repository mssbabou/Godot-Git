#pragma once

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/confirmation_dialog.hpp>
#include <godot_cpp/classes/editor_dock.hpp>
#include <godot_cpp/classes/foldable_container.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/margin_container.hpp>
#include <godot_cpp/classes/menu_button.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/thread.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/tree_item.hpp>

#include "git_repository.h"

using namespace godot;

// The "Git" panel shown on the right side of the editor.
class GitDock : public EditorDock {
	GDCLASS(GitDock, EditorDock)

	enum ItemButton {
		BUTTON_STAGE,
		BUTTON_UNSTAGE,
		BUTTON_DISCARD,
	};

	enum MenuId {
		// Right-click menu on files and commits.
		MENU_OPEN,
		MENU_STAGE,
		MENU_UNSTAGE,
		MENU_DISCARD,
		MENU_SHOW_IN_FILESYSTEM,
		MENU_SHOW_IN_FILE_MANAGER,
		MENU_COPY_PATH,
		MENU_COPY_RELATIVE_PATH,
		MENU_COPY_HASH,
		MENU_COPY_MESSAGE,
		// "More" (⋮) menu.
		MORE_REFRESH,
		MORE_STAGE_ALL,
		MORE_UNSTAGE_ALL,
		MORE_DISCARD_ALL,
		MORE_NEW_BRANCH,
		MORE_OPEN_FOLDER,
	};

	enum NetworkOp {
		NETWORK_NONE,
		NETWORK_FETCH,
		NETWORK_PULL,
		NETWORK_PUSH,
	};

	// A collapsible section ("Staged Changes", "Changes") listing files.
	struct FilePane {
		FoldableContainer *container = nullptr;
		Tree *tree = nullptr;
		Label *count = nullptr;
		Label *added = nullptr;
		Label *removed = nullptr;
		MarginContainer *buttons_margin = nullptr; // Its right margin aligns the buttons with the rows'.
		HBoxContainer *buttons = nullptr;
		Button *action = nullptr; // Stage all / unstage all.
		Button *discard = nullptr; // Discard all (unstaged pane only).
		bool staged = false;
		int file_count = 0; // Files listed (the "No changes." placeholder row doesn't count).
		uint64_t hovered_item = 0;
	};

	Ref<GitRepository> repo;

	OptionButton *branch_select = nullptr;
	Button *fetch_button = nullptr;
	MenuButton *more_menu = nullptr;
	TextEdit *commit_message = nullptr;
	Button *commit_button = nullptr;
	Button *pull_button = nullptr;
	Button *push_button = nullptr;

	FilePane staged_pane;
	FilePane changes_pane;
	FoldableContainer *history_pane = nullptr;
	Tree *history_tree = nullptr;

	Label *no_repo_label = nullptr;
	Control *repo_ui = nullptr;

	PopupMenu *context_menu = nullptr;
	Tree *context_tree = nullptr;
	ConfirmationDialog *discard_confirm = nullptr;
	PackedStringArray pending_discard;
	ConfirmationDialog *branch_dialog = nullptr;
	LineEdit *branch_name_edit = nullptr;

	Dictionary sync_status;
	int staged_count = 0;
	bool has_commits = false;
	bool align_queued = false;
	PackedStringArray unstaged_paths;

	Ref<Thread> network_thread;
	NetworkOp network_op = NETWORK_NONE;

	void _make_file_pane(FilePane &r_pane, Control *p_parent, const String &p_title, bool p_staged);
	void _fill_file_pane(FilePane &p_pane, const Array &p_status, const Dictionary &p_stats);
	void _fill_history();
	void _draw_file_row(TreeItem *p_item, const Rect2 &p_rect);
	void _fill_branches();
	void _update_actions();
	void _build_more_menu();
	void _update_icons();
	void _queue_align_header_buttons();
	void _align_header_buttons();
	void _report(Error p_err, const String &p_action);

	FilePane *_pane_for_tree(Object *p_tree);
	PackedStringArray _selected_paths(Tree *p_tree) const;
	String _to_res_path(const String &p_path) const;
	Ref<Texture2D> _file_icon(const String &p_path) const;
	Color _status_color(const String &p_state) const;
	Color _dim_color() const;

	void _set_hovered(FilePane &p_pane, TreeItem *p_item);
	void _on_tree_gui_input(const Ref<InputEvent> &p_event, Object *p_tree);
	void _on_tree_mouse_exited(Object *p_tree);
	void _on_tree_button_clicked(TreeItem *p_item, int p_column, int p_id, int p_mouse_button);
	void _on_tree_mouse_selected(const Vector2 &p_position, int p_mouse_button, Object *p_tree);
	void _on_file_activated(Object *p_tree);
	void _on_context_menu_id(int p_id);
	void _on_more_menu_id(int p_id);
	void _on_branch_selected(int p_index);
	void _on_branch_dialog_confirmed();
	void _on_commit_message_input(const Ref<InputEvent> &p_event);
	void _on_discard_confirmed();

	void _open_path(const String &p_path);
	bool _open_in_code_editor(const String &p_absolute_path);
	void _stage_paths(const PackedStringArray &p_paths, bool p_stage);
	void _confirm_discard(const PackedStringArray &p_paths);
	void _commit();

	void _start_network(int p_op);
	void _network_worker(int p_op, const String &p_workdir);
	void _network_done(int p_op, int p_err, const String &p_message, const String &p_upstream, const String &p_notice);
	void _finish_network_thread();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void refresh();

	GitDock();
};
