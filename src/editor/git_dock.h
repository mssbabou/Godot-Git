#pragma once

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/check_box.hpp>
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

class GitDiffDock;

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
		MORE_ADD_REMOTE,
	};

	enum NetworkOp {
		NETWORK_NONE,
		NETWORK_FETCH,
		NETWORK_PULL,
		NETWORK_PUSH,
		NETWORK_SWITCH, // Switching branches in a repository with LFS files, which may download them.
		NETWORK_COMMIT, // Committing through git, because hooks run or commits get signed.
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

	// Sync row (Fetch, Pull, Push; hidden without a remote), then the message, then Amend + Commit.
	HBoxContainer *sync_row = nullptr;
	Button *fetch_button = nullptr;
	Button *pull_button = nullptr;
	Button *push_button = nullptr;
	TextEdit *commit_message = nullptr;
	CheckBox *amend_check = nullptr;
	Button *commit_button = nullptr;
	String amend_saved_draft; // What was in the message box before ticking Amend filled it in.
	String last_commit_id;
	String last_commit_message;
	bool last_commit_pushed = false;

	FilePane staged_pane;
	FilePane changes_pane;
	FoldableContainer *history_pane = nullptr;
	Tree *history_tree = nullptr;
	Label *history_empty = nullptr;
	int history_limit = 50; // Commits listed; "Load More" adds more.
	Array history_shown; // The commits in the tree, so a refresh that changed none keeps it as is.
	bool history_more = false;
	Dictionary history_expanded; // Hashes of expanded commits, kept across rebuilds.
	Dictionary commit_files; // Hash -> get_commit_files(). Commits never change, so it's kept.

	Control *no_repo_ui = nullptr; // "Not a git repository yet", with Initialize Repository.
	Label *no_repo_hint = nullptr;
	Control *repo_ui = nullptr;

	PopupMenu *context_menu = nullptr;
	Tree *context_tree = nullptr;
	ConfirmationDialog *discard_confirm = nullptr;
	PackedStringArray pending_discard;
	ConfirmationDialog *branch_dialog = nullptr;
	LineEdit *branch_name_edit = nullptr;
	ConfirmationDialog *switch_confirm = nullptr; // Switching to a branch without this addon.
	String pending_switch;

	// Setting a repository up: Initialize, Add Remote, and the name and email commits need.
	ConfirmationDialog *init_dialog = nullptr;
	Label *init_question = nullptr;
	CheckBox *init_here = nullptr;
	CheckBox *init_parent = nullptr;
	Control *init_parent_box = nullptr; // The "folder above" choice, hidden when that's no place for a repo.
	MarginContainer *init_here_indent = nullptr;
	MarginContainer *init_parent_indent = nullptr;
	Label *init_here_path = nullptr;
	Label *init_parent_path = nullptr;
	ConfirmationDialog *remote_dialog = nullptr;
	LineEdit *remote_url_edit = nullptr;
	ConfirmationDialog *identity_dialog = nullptr;
	LineEdit *identity_name_edit = nullptr;
	LineEdit *identity_email_edit = nullptr;
	CheckBox *identity_local_check = nullptr;
	NetworkOp identity_then = NETWORK_NONE; // What to do once saved: NETWORK_COMMIT or NETWORK_PULL.

	Dictionary sync_status;
	int staged_count = 0;
	bool has_commits = false;
	bool align_queued = false;
	PackedStringArray unstaged_paths;

	Ref<Thread> network_thread;
	NetworkOp network_op = NETWORK_NONE;
	int network_ahead = 0; // Commits a push is sending, counted when it starts.
	bool network_publish = false;
	String network_branch; // The branch a NETWORK_SWITCH switches to.
	String network_commit_message; // What a NETWORK_COMMIT commits, and how.
	bool network_amend = false;
	int network_commit_files = 0;
	String network_amended_id;
	// A background (automatic) fetch runs quietly: no strip, no disabled buttons, no login windows.
	// If you press Pull or Push meanwhile, it's queued and shown as waiting.
	bool network_quiet = false;
	NetworkOp queued_op = NETWORK_NONE;

	// Git itself (the program) runs hooks, signing, LFS, SSH and logins. Without it the rest works
	// on libgit2, and what won't work is disabled with the reason, plus one warning that lists it.
	bool git_missing = false;
	Dictionary git_needs; // GitRepository::get_git_needs(), while git is missing.
	String git_warning; // The warning shown once (so it can be cleared once git is installed).

	// The Diff panel at the bottom, and the file it shows: clicking a file here shows its diff there.
	GitDiffDock *diff_dock = nullptr;
	String diff_path;
	bool diff_staged = false;
	String diff_commit; // Set when the file shown is from a commit (History), not uncommitted.
	String diff_commit_shown; // "hash:path" already in the panel; commit diffs never change.

	// Line counts (+/-) for the two lists. Counted on a worker thread with its own GitRepository:
	// on a big change set it takes seconds (2.6 ms per file), and refresh runs after every save.
	// The lists show the last counts meanwhile, so the totals don't flicker.
	Ref<Thread> stats_thread;
	bool stats_again = false; // Refreshed while counting: count once more afterwards.
	bool stats_slow = false; // Counting for a while already: the totals are dimmed and say so.
	Timer *stats_slow_timer = nullptr;
	Dictionary file_icons; // File type -> its editor icon, for _file_icon; cleared with the theme.
	Dictionary staged_stats; // {path: Vector2i(added, removed)}; Vector2i(-1, -1) for binary.
	Dictionary unstaged_stats;

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
	void _check_git();
	String _git_missing_warning() const;
	String _needs_git(int p_op) const;
	String _to_res_path(const String &p_path) const;
	Ref<Texture2D> _file_icon(const String &p_path);
	Color _status_color(const String &p_state) const;
	Color _dim_color() const;
	void _open_path(const String &p_path);
	void _stage_paths(const PackedStringArray &p_paths, bool p_stage);
	void _confirm_discard(const PackedStringArray &p_paths);
	void _on_discard_confirmed();
	void _on_branch_selected(int p_index);
	String _addon_removed_by(const String &p_branch) const;
	void _switch_branch(const String &p_branch);
	void _on_branch_dialog_confirmed();
	void _on_commit_message_input(const Ref<InputEvent> &p_event);
	void _on_amend_toggled(bool p_on);
	void _commit();
	void _report_commit(bool p_amended, int p_files, const String &p_old_id);

	// git_dock_lists.cpp: the Staged Changes / Changes / History sections.
	void _build_lists(Control *p_parent);
	void _make_file_pane(FilePane &r_pane, Control *p_parent, const String &p_title, bool p_staged);
	Label *_make_body(Control *p_section, Tree *p_tree);
	void _fill_file_pane(FilePane &p_pane, const Array &p_status);
	void _start_line_stats();
	void _line_stats_worker(const String &p_workdir);
	void _line_stats_done(const Dictionary &p_staged, const Dictionary &p_unstaged);
	void _on_line_stats_slow();
	void _show_line_stats(FilePane &p_pane);
	void _finish_stats_thread();
	void _fill_history();
	void _fill_commit(TreeItem *p_item);
	void _fill_commit_later(uint64_t p_item);
	void _load_more_commits();
	void _on_history_item_collapsed(TreeItem *p_item);
	void _on_history_item_selected();
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
	void _on_file_multi_selected(TreeItem *p_item, int p_column, bool p_selected, Object *p_tree);
	void _show_diff(const String &p_path, bool p_staged, bool p_focus);
	void _show_commit_diff(const String &p_hash, const String &p_path, bool p_focus);
	void _update_diff();
	void _select_diff_row();

	// git_dock_status.cpp: the status strip.
	void _build_status_strip(Control *p_parent);
	void _report(Error p_err, const String &p_action);
	void _set_status(StatusKind p_kind, const String &p_text);
	void _update_status();
	void _update_status_style();
	void _on_status_button();

	// git_dock_setup.cpp: setting a repository up (Initialize, Add Remote, name and email).
	void _build_setup(Control *p_parent);
	void _show_init_dialog();
	void _on_init_confirmed();
	void _show_remote_dialog();
	void _on_remote_url_changed(const String &p_text);
	void _on_remote_confirmed();
	bool _ask_identity(NetworkOp p_then);
	void _on_identity_changed(const String &p_text);
	void _on_identity_confirmed();

	// git_dock_network.cpp: fetch / pull / push on a worker thread, and auto-fetch.
	void _start_network(int p_op);
	void _run_network(NetworkOp p_op, bool p_quiet);
	NetworkOp _shown_network_op() const;
	String _network_description(int p_op) const;
	bool _is_auto_fetch_enabled() const;
	void _on_auto_fetch_timer();
	void _network_worker(int p_op, const String &p_workdir, bool p_quiet, const String &p_text, bool p_amend);
	void _network_progress(const String &p_step, double p_fraction, bool p_cancellable);
	void _network_done(int p_op, int p_err, const String &p_message, const String &p_upstream, const String &p_notice, const Dictionary &p_pull_result);
	void _finish_network_thread();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void refresh();
	void set_diff_dock(GitDiffDock *p_dock);

	GitDock();
};
