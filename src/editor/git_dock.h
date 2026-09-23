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
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/progress_bar.hpp>
#include <godot_cpp/classes/rich_text_label.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/thread.hpp>
#include <godot_cpp/classes/timer.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/tree_item.hpp>

#include "git/git_repository.h"

using namespace godot;

// The "Git" panel shown on the right side of the editor. Its implementation is split over
// git_dock.cpp, git_dock_lists.cpp, git_dock_status.cpp and git_dock_network.cpp (see below).
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
		MORE_AUTO_FETCH,
	};

	enum NetworkOp {
		NETWORK_NONE,
		NETWORK_FETCH,
		NETWORK_PULL,
		NETWORK_PUSH,
	};

	// What the status strip is showing.
	enum StatusKind {
		STATUS_IDLE, // Nothing happened yet: shows when the remote was last fetched.
		STATUS_BUSY, // An operation is running: progress, and Cancel while it can be canceled.
		STATUS_SUCCESS, // The last operation's result, with how long ago.
		STATUS_NEUTRAL, // E.g. "Pull canceled."
		STATUS_WARNING, // Succeeded with a catch; stays until dismissed or replaced.
		STATUS_ERROR, // Failed; stays until dismissed or replaced.
	};

	// A collapsible section ("Staged Changes", "Changes") listing files.
	struct FilePane {
		FoldableContainer *container = nullptr;
		Tree *tree = nullptr;
		Label *empty_label = nullptr; // "No changes.", shown instead of the tree when it's empty.
		Label *count = nullptr;
		Label *added = nullptr;
		Label *removed = nullptr;
		MarginContainer *buttons_margin = nullptr; // Its right margin aligns the buttons with the rows'.
		HBoxContainer *buttons = nullptr;
		Button *action = nullptr; // Stage all / unstage all.
		Button *discard = nullptr; // Discard all (unstaged pane only).
		bool staged = false;
		int file_count = 0;
		uint64_t hovered_item = 0;
	};

	Ref<GitRepository> repo;
	Timer *project_file_timer = nullptr; // Watches project.godot, which Godot saves without telling anyone.
	uint64_t project_file_time = 0;

	OptionButton *branch_select = nullptr;
	Button *fetch_button = nullptr;
	MenuButton *more_menu = nullptr;

	// Status strip under the toolbar: what the panel is doing, or what it last did.
	PanelContainer *status_strip = nullptr;
	TextureRect *status_icon = nullptr;
	RichTextLabel *status_label = nullptr; // The text, then dimmed: the current step, or "5m ago".
	Button *status_button = nullptr; // Cancel (busy) or dismiss (warning, error).
	ProgressBar *status_progress = nullptr;
	Timer *status_timer = nullptr; // Keeps "5m ago" current.
	StatusKind status_kind = STATUS_IDLE;
	String status_text;
	String status_step;
	int64_t status_time = 0;
	bool status_cancellable = false;

	TextEdit *commit_message = nullptr;
	Button *commit_button = nullptr;
	Button *pull_button = nullptr;
	Button *push_button = nullptr;

	FilePane staged_pane;
	FilePane changes_pane;
	FoldableContainer *history_pane = nullptr;
	Tree *history_tree = nullptr;
	Label *history_empty = nullptr;

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
	int network_ahead = 0; // Commits a push is sending, counted when it starts.
	bool network_publish = false;
	// A background (automatic) fetch runs quietly: no strip, no disabled buttons, no login windows.
	// If you press Pull or Push meanwhile, it's queued and shown as waiting.
	bool network_quiet = false;
	NetworkOp queued_op = NETWORK_NONE;

	Timer *auto_fetch_timer = nullptr;
	int64_t last_auto_fetch_attempt = 0;
	bool auto_fetch_failed = false; // The failure is shown once, not every few minutes.

	// git_dock.cpp: building, refreshing, toolbar and action row, local actions.
	void _update_icons();
	void _fill_branches();
	void _update_actions();
	void _build_more_menu();
	void _on_more_menu_id(int p_id);
	void _check_project_file();
	String _to_res_path(const String &p_path) const;
	Ref<Texture2D> _file_icon(const String &p_path) const;
	Color _status_color(const String &p_state) const;
	Color _dim_color() const;
	void _open_path(const String &p_path);
	void _stage_paths(const PackedStringArray &p_paths, bool p_stage);
	void _confirm_discard(const PackedStringArray &p_paths);
	void _on_discard_confirmed();
	void _on_branch_selected(int p_index);
	void _on_branch_dialog_confirmed();
	void _on_commit_message_input(const Ref<InputEvent> &p_event);
	void _commit();

	// git_dock_lists.cpp: the Staged Changes / Changes / History sections.
	void _build_lists(Control *p_parent);
	void _make_file_pane(FilePane &r_pane, Control *p_parent, const String &p_title, bool p_staged);
	Label *_make_body(Control *p_section, Tree *p_tree);
	void _fill_file_pane(FilePane &p_pane, const Array &p_status, const Dictionary &p_stats);
	void _fill_history();
	void _draw_file_row(TreeItem *p_item, const Rect2 &p_rect);
	void _queue_align_header_buttons();
	void _align_header_buttons();
	FilePane *_pane_for_tree(Object *p_tree);
	PackedStringArray _selected_paths(Tree *p_tree) const;
	void _set_hovered(FilePane &p_pane, TreeItem *p_item);
	void _on_tree_gui_input(const Ref<InputEvent> &p_event, Object *p_tree);
	void _on_tree_mouse_exited(Object *p_tree);
	void _on_tree_button_clicked(TreeItem *p_item, int p_column, int p_id, int p_mouse_button);
	void _on_tree_mouse_selected(const Vector2 &p_position, int p_mouse_button, Object *p_tree);
	void _on_file_activated(Object *p_tree);
	void _on_context_menu_id(int p_id);

	// git_dock_status.cpp: the status strip.
	void _build_status_strip(Control *p_parent);
	void _report(Error p_err, const String &p_action);
	void _set_status(StatusKind p_kind, const String &p_text);
	void _update_status();
	void _update_status_style();
	void _on_status_button();

	// git_dock_network.cpp: fetch / pull / push on a worker thread, and auto-fetch.
	void _start_network(int p_op);
	void _run_network(NetworkOp p_op, bool p_quiet);
	NetworkOp _shown_network_op() const;
	String _network_description(int p_op) const;
	bool _is_auto_fetch_enabled() const;
	void _on_auto_fetch_timer();
	void _network_worker(int p_op, const String &p_workdir, bool p_quiet);
	void _network_progress(const String &p_step, double p_fraction, bool p_cancellable);
	void _network_done(int p_op, int p_err, const String &p_message, const String &p_upstream, const String &p_notice, const Dictionary &p_pull_result);
	void _finish_network_thread();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void refresh();

	GitDock();
};
