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
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/progress_bar.hpp>
#include <godot_cpp/classes/rich_text_label.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/thread.hpp>
#include <godot_cpp/classes/timer.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/tree_item.hpp>

#include "editor/git_dock_util.h"
#include "git/git_repository.h"

class GitDiffDock;
class GitFileSystemColors;
class GitScriptMarks;

using namespace godot;

// The "Git" panel shown on the right side of the editor. Its implementation is split by area over
// git_dock.cpp and the git_dock_*.cpp files (see the method list below).
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
		MENU_OPEN_ON_WEB,
		MENU_RESTORE_STASH,
		MENU_DELETE_STASH,
		MENU_SHOW_HISTORY,
		MENU_RESTORE_VERSION,
		MENU_RESTORE_BEFORE,
		MENU_UNDO_COMMIT,
		MENU_REVERT_COMMIT,
		MENU_BRANCH_HERE,
		MENU_IGNORE,
		MENU_LOCK,
		MENU_UNLOCK,
		// "More" (⋮) menu.
		MORE_REFRESH,
		MORE_STAGE_ALL,
		MORE_UNSTAGE_ALL,
		MORE_DISCARD_ALL,
		MORE_OPEN_FOLDER,
		MORE_ADD_REMOTE,
		MORE_BUILD_INFO,
		MORE_STASH_ALL,
		MORE_SETTINGS,
		MORE_ABORT_MERGE,
	};

	// The buttons on a hovered stash row.
	enum StashButton {
		STASH_RESTORE,
		STASH_DELETE,
	};

	enum NetworkOp {
		NETWORK_NONE,
		NETWORK_FETCH,
		NETWORK_PULL,
		NETWORK_PUSH,
		NETWORK_SWITCH, // Switching branches in a repository with LFS files, which may download them.
		NETWORK_COMMIT, // Committing through git, because hooks run or commits get signed.
		NETWORK_ABORT, // Aborting an operation left in progress (a merge, rebase, ...), through git.
		NETWORK_CONTINUE, // Continuing one once its conflicts are resolved (hooks may run).
		NETWORK_REVERT, // Reverting a commit: a new commit, so hooks may run.
		NETWORK_PULL_MERGE, // Pulling into conflicts on purpose (Start Merge; GitRepository::pull(true)).
		NETWORK_LOCK, // Locking a file on the LFS server (git lfs lock).
		NETWORK_UNLOCK,
		NETWORK_MERGE, // Merging a branch into the current one (GitRepository::merge_branch).
		NETWORK_MERGE_START, // The same, stopping at its conflicts for the resolver.
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
		String title; // "Changes"; shown as "Changes (9)" while it lists files.
		Label *added = nullptr;
		Label *removed = nullptr;
		MarginContainer *buttons_margin = nullptr; // Its right margin aligns the buttons with the rows'.
		HBoxContainer *buttons = nullptr;
		Button *action = nullptr; // Stage all / unstage all.
		Button *discard = nullptr; // Discard all (unstaged pane only).
		Button *stash = nullptr; // Stash what's staged (staged pane only).
		bool staged = false;
		int file_count = 0;
		uint64_t hovered_item = 0;
		int hovered_button = -1; // ItemButton under the mouse on the hovered row, or -1.
	};

	Ref<GitRepository> repo;
	Timer *project_file_timer = nullptr; // Watches project.godot, which Godot saves without telling anyone.
	uint64_t project_file_time = 0;

	// The branch picker (git_dock_branches.cpp): a button naming the current branch, opening a
	// panel with a search field and the branches.
	Button *branch_button = nullptr;
	TextureRect *branch_arrow = nullptr;
	OptionButton *branch_style_source = nullptr; // Hidden: where the button's look comes from.
	PopupPanel *branch_popup = nullptr;
	LineEdit *branch_search = nullptr;
	Tree *branch_tree = nullptr;
	Array branch_list; // GitRepository::get_branch_list(), as of the last refresh.
	uint64_t branch_hovered = 0; // The row showing Rename and Delete.
	MenuButton *more_menu = nullptr;
	Button *merge_button = nullptr; // Opens the Merge dialog; between the branch picker and the menu.

	// Status strip under the toolbar: what the panel is doing, or what it last did.
	PanelContainer *status_strip = nullptr;
	TextureRect *status_icon = nullptr;
	RichTextLabel *status_label = nullptr; // The text, then dimmed: the current step, or "5m ago".
	Button *status_button = nullptr; // Cancel (busy) or dismiss (warning, error).
	// Earlier results, newest last ([{ "kind", "text", "time" }], this session, up to 50): what
	// the panel did this afternoon, behind the clock button on the strip.
	ScrollContainer *lists_scroll = nullptr; // The sections' shared scroll area.
	Array status_log;
	// Git LFS locks, from the last fetch in a repository that uses LFS ({ path: { "owner", "mine" } }).
	Dictionary lfs_locks;
	bool lfs_locks_read = false; // Whether lfs_locks came from the server (some don't do locking).
	Button *status_log_button = nullptr;
	PopupPanel *status_log_popup = nullptr;
	RichTextLabel *status_log_label = nullptr;
	ProgressBar *status_progress = nullptr;
	Timer *status_timer = nullptr; // Keeps "5m ago" current.

	// Under the strip while the repository is in the middle of an operation (a merge, rebase, ...
	// usually left by a terminal): what it is, its conflicts, Abort and Continue.
	PanelContainer *operation_banner = nullptr;
	Label *operation_label = nullptr;
	PanelContainer *export_banner = nullptr; // Godot 4.7: offers to keep the addon out of exports.
	// Copies of your edits left by a pull that didn't finish (git_dock_leftovers.cpp).
	PanelContainer *leftovers_banner = nullptr;
	Label *leftovers_label = nullptr;
	Button *leftovers_put_back = nullptr;
	Button *leftovers_delete = nullptr;
	ConfirmationDialog *leftovers_confirm = nullptr;
	Array leftovers;
	Label *export_label = nullptr;
	uint64_t export_presets_time = 0; // export_presets.cfg's modified time when last read.
	PackedStringArray export_presets_missing; // Presets without the filter.
	Callable reload_export_presets; // GitEditorPlugin's; makes the editor read the presets again.
	Button *operation_abort = nullptr;
	Button *operation_continue = nullptr;
	ConfirmationDialog *abort_confirm = nullptr;
	Dictionary operation; // GitRepository::get_operation(), as of the last refresh.
	String network_operation; // _operation_name() when a NETWORK_ABORT / NETWORK_CONTINUE started.
	String network_operation_kind; // Its kind ("pull" shows as a merge, but finishes differently).
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
	// The merge's message put in the commit box while a merge waits for its commit ("Merge branch
	// 'art-pass'"), once per merge: taken out again if the merge ends some other way, untouched.
	String merge_prefill;
	bool commit_was_merge = false; // The commit being made finishes a merge (for its report).
	bool last_commit_pushed = false;
	bool push_after_commit = false; // Ctrl+Shift+Enter: commit, then push.
	// Up/Down in an empty message box goes through your recent commit messages.
	PackedStringArray message_history;
	int message_history_index = -1;
	ConfirmationDialog *large_confirm = nullptr; // Staged files big enough to regret committing.
	bool large_checked = false;
	Button *large_lfs_button = nullptr;
	Button *large_ignore_button = nullptr;
	PackedStringArray large_paths; // The files the question was about.
	PackedStringArray pull_blockers; // Uncommitted files the new commits change too (Pull refuses).
	PackedStringArray pull_conflict_paths; // Uncommitted edits on the same lines (Pull asks to merge).
	ConfirmationDialog *pull_merge_confirm = nullptr; // "2 files conflict with origin/main".
	Label *pull_merge_question = nullptr;
	Label *pull_merge_files = nullptr;
	CheckBox *pull_merge_dont_ask = nullptr;
	// "Merge into main": pick a branch, see what merging it would do, merge.
	ConfirmationDialog *merge_dialog = nullptr;
	LineEdit *merge_search = nullptr;
	Tree *merge_tree = nullptr;
	Label *merge_summary = nullptr;
	Label *merge_detail = nullptr;
	Array merge_branches; // GitRepository::get_merge_branches, read when the dialog opens.
	String merge_selected; // The branch the preview is for.
	Dictionary merge_preview;

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
	// History's filters (git_dock_history.cpp): a search, and one file's commits.
	Button *history_search_button = nullptr; // In History's header; opens the search field.
	Control *history_search_row = nullptr;
	LineEdit *history_search = nullptr;
	Timer *history_search_timer = nullptr; // Searches a moment after typing stops.
	String history_query;
	Control *history_file_bar = nullptr; // "History of player.gd", with a button that ends it.
	TextureRect *history_file_icon = nullptr;
	Label *history_file_label = nullptr;
	Button *history_file_close = nullptr;
	String history_path; // The file whose commits History shows (repository path), or "" for all.
	ConfirmationDialog *revert_confirm = nullptr;
	String pending_revert; // The commit a NETWORK_REVERT reverts.
	String pending_revert_summary;
	String branch_here; // The commit Create Branch Here creates the branch at.
	Label *branch_dialog_label = nullptr;

	// Stashes: a section that only shows while there are stashes (git_dock_stashes.cpp).
	FoldableContainer *conflicts_pane = nullptr; // Only while files are conflicted.
	Control *conflicts_header_strut = nullptr;
	Button *conflicts_abort = nullptr; // Abort Merge, on the Conflicts header.
	Tree *conflicts_tree = nullptr;
	PackedStringArray conflicted_paths;
	FoldableContainer *stashes_pane = nullptr;
	Control *stashes_header_strut = nullptr;
	Tree *stashes_tree = nullptr;
	Label *stashes_empty = nullptr;
	Array stashes_shown; // GitRepository::get_stashes(), as the section shows them.
	Dictionary stashes_expanded; // Hashes of expanded stashes, kept across rebuilds.
	Dictionary stash_files; // Hash -> get_stash_files(). A stash never changes.
	uint64_t stash_hovered = 0; // The stash row showing Restore and Delete.
	ConfirmationDialog *stash_delete_confirm = nullptr;
	// "1 file conflicts with newer commits": restore anyway and resolve (restore_stash with merge).
	ConfirmationDialog *stash_merge_confirm = nullptr;
	Label *stash_merge_question = nullptr;
	Label *stash_merge_files = nullptr;
	String pending_stash_merge;
	String pending_stash_delete;
	ConfirmationDialog *stash_switch_confirm = nullptr; // Offered when changes are in a switch's way.
	ConfirmationDialog *stash_dialog = nullptr; // What a stash takes, and an optional name.
	Label *stash_files_label = nullptr;
	LineEdit *stash_name_edit = nullptr;
	bool stash_dialog_staged = false;

	ConfirmationDialog *ignore_dialog = nullptr; // Ignore...: this file, its type, or a folder.
	CheckBox *ignore_options[3] = {};
	OptionButton *ignore_folder_select = nullptr;
	Label *ignore_effect_label = nullptr;
	String ignore_path; // The file the dialog is for (repository path).
	String ignore_file; // The .gitignore the rule goes into.
	PackedStringArray ignore_untracked; // Untracked files when the dialog opened.
	PackedStringArray ignore_hidden; // Of those, what the chosen rule hides.

	Control *no_repo_ui = nullptr; // "Not a git repository yet", with Initialize Repository.
	Label *no_repo_hint = nullptr;
	Control *repo_ui = nullptr;

	PopupMenu *context_menu = nullptr;
	Tree *context_tree = nullptr;
	ConfirmationDialog *discard_confirm = nullptr;
	PackedStringArray pending_discard;
	ConfirmationDialog *branch_dialog = nullptr;
	LineEdit *branch_name_edit = nullptr;
	ConfirmationDialog *delete_branch_dialog = nullptr;
	String pending_delete;
	Label *delete_branch_label = nullptr;
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
	NetworkOp identity_then = NETWORK_NONE; // What to do once saved: NETWORK_COMMIT, NETWORK_PULL or a merge.

	Dictionary sync_status;
	int staged_count = 0;
	bool has_commits = false;
	bool align_queued = false;
	PackedStringArray staged_paths; // Every file in Staged Changes / Changes, companions too.
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

	// Keeping the editor in step: unsaved files are offered to be saved before a pull or switch
	// (unsaved_then runs once answered), and open scenes it rewrote are reloaded afterwards.
	ConfirmationDialog *unsaved_confirm = nullptr;
	Button *unsaved_skip = nullptr;
	Callable unsaved_then;
	bool unsaved_checked = false; // Set just before unsaved_then runs, so it doesn't ask again.
	Dictionary open_scene_hashes; // res:// path -> MD5 of the file, before the operation.
	GitFileSystemColors *filesystem_colors = nullptr;
	GitScriptMarks *script_marks = nullptr; // Changed lines marked in the script editor.
	AcceptDialog *settings_dialog = nullptr; // Git Settings (the menu's Settings...).
	CheckBox *settings_checks[godot_git::SETTING_COUNT] = {}; // In the order of SETTINGS in git_dock_editor.cpp.

	// The Diff panel at the bottom, and the file it shows: clicking a file here shows its diff there.
	GitDiffDock *diff_dock = nullptr;
	String diff_path;
	bool diff_staged = false;
	String diff_commit; // Set when the file shown is from a commit (History), not uncommitted.
	bool diff_stash = false; // diff_commit is a stash (Stashes), not a commit.
	bool diff_conflict = false; // diff_path is conflicted: the panel shows the resolver.
	String diff_commit_shown; // "hash:path" already in the panel; commit diffs never change.

	// Line counts (+/-) for the two lists. Counted on a worker thread with its own GitRepository:
	// on a big change set it takes seconds (2.6 ms per file), and refresh runs after every save.
	// The lists show the last counts meanwhile, so the totals don't flicker.
	Ref<Thread> stats_thread;
	bool stats_again = false; // Refreshed while counting: count once more afterwards.
	bool stats_slow = false; // Counting for a while already: the totals are dimmed and say so.
	Timer *stats_slow_timer = nullptr;
	Dictionary file_icons; // File type -> its editor icon, for _file_icon; cleared with the theme.
	Dictionary own_icons; // Our drawn icons ("GitMinus", ...), for _icon; redrawn with the theme.
	Dictionary folder_types; // res:// folder -> { file name: type }, for _file_type; cleared on refresh.
	Dictionary staged_stats; // {path: Vector2i(added, removed)}; Vector2i(-1, -1) for binary.
	Dictionary unstaged_stats;

	Timer *auto_fetch_timer = nullptr;
	int64_t last_auto_fetch_attempt = 0;
	bool auto_fetch_failed = false; // The failure is shown once, not every few minutes.

	// git_dock.cpp: building, refreshing, the toolbar and action rows, file actions.
	void _update_icons();
	void _update_actions();
	void _update_commit_row(bool p_syncing);
	void _update_sync_row(bool p_busy);
	void _build_more_menu();
	void _on_more_menu_id(int p_id);
	void _check_project_file();
	void _check_git();
	String _git_missing_warning() const;
	String _needs_git(int p_op) const;
	String _to_res_path(const String &p_path) const;
	Ref<Texture2D> _file_icon(const String &p_path);
	Ref<Texture2D> _icon(const String &p_name) const;
	void _draw_own_icons();
	String _file_type(const String &p_res_path);
	Color _status_color(const String &p_state) const;
	Color _dim_color() const;
	ConfirmationDialog *_make_confirm(const String &p_title, const String &p_ok_text, const Callable &p_on_confirmed);
	void _open_path(const String &p_path);
	void _stage_paths(const PackedStringArray &p_paths, bool p_stage);
	void _confirm_discard(const PackedStringArray &p_paths);
	void _on_discard_confirmed();

	// git_dock_branches.cpp: the branch picker.
	void _build_branch_picker(Control *p_parent);
	void _style_branch_button();
	void _fill_branches();
	void _show_branch_popup();
	void _fill_branch_tree();
	void _fit_branch_popup();
	void _on_branch_search_changed(const String &p_text);
	void _on_branch_search_input(const Ref<InputEvent> &p_event);
	void _on_branch_tree_input(const Ref<InputEvent> &p_event);
	void _set_branch_hovered(TreeItem *p_item);
	void _on_branch_row_button(TreeItem *p_item, int p_column, int p_id, int p_mouse_button);
	void _on_branch_row_clicked(const Vector2 &p_position, int p_mouse_button);
	void _on_branch_row_activated();
	void _pick_branch_row(uint64_t p_item);
	void _pick_branch(const String &p_branch);
	String _addon_removed_by(const String &p_branch) const;
	void _switch_branch(const String &p_branch);
	void _on_branch_dialog_confirmed();
	void _build_branch_dialogs();
	void _show_delete_branch_dialog(const String &p_branch);
	void _on_delete_branch_confirmed();

	// git_dock_commit.cpp: the commit box.
	void _on_commit_message_input(const Ref<InputEvent> &p_event);
	void _recall_message(int p_step);
	void _on_amend_toggled(bool p_on);
	void _commit();
	bool _ask_about_large_files();
	void _on_large_confirmed();
	void _on_large_custom_action(const StringName &p_action);
	void _after_commit();
	String _web_commit_url(const String &p_hash) const;
	void _report_commit(bool p_amended, int p_files, const String &p_old_id);

	// git_dock_lists.cpp: the Staged Changes / Changes sections and how rows are drawn.
	void _build_lists(Control *p_parent);
	void _make_file_pane(FilePane &r_pane, Control *p_parent, const String &p_title, bool p_staged);
	void _round_section(FoldableContainer *p_section);
	Label *_make_body(Control *p_section, Tree *p_tree);
	void _fill_file_pane(FilePane &p_pane, const Array &p_status);
	void _start_line_stats();
	void _line_stats_worker(const String &p_workdir);
	void _line_stats_done(const Dictionary &p_staged, const Dictionary &p_unstaged);
	void _on_line_stats_slow();
	void _show_line_stats(FilePane &p_pane);
	void _finish_stats_thread();
	void _queue_align_header_buttons();
	void _align_header_buttons();
	void _draw_file_row(TreeItem *p_item, const Rect2 &p_rect);
	float _draw_row_buttons(TreeItem *p_item, const FilePane &p_pane, const Rect2 &p_rect, float p_right);

	// git_dock_rows.cpp: what rows do (hover buttons, clicks, selection, right-click menus).
	FilePane *_pane_for_tree(Object *p_tree);
	PackedStringArray _selected_paths(Tree *p_tree, bool p_companions = false) const;
	PackedStringArray _row_paths(TreeItem *p_item) const;
	PackedStringArray _blocking_paths(TreeItem *p_item) const;
	void _forward_wheel(const Ref<InputEvent> &p_event, Tree *p_tree);
	String _lock_note(const String &p_path) const;
	Array _row_button_list(const FilePane &p_pane) const;
	void _set_hovered(FilePane &p_pane, TreeItem *p_item);
	Dictionary _row_button_at(const FilePane &p_pane, const Vector2 &p_position) const;
	void _on_tree_gui_input(const Ref<InputEvent> &p_event, Object *p_tree);
	void _on_tree_mouse_exited(Object *p_tree);
	void _click_row_button(uint64_t p_item, int p_id);
	void _on_file_activated(Object *p_tree);
	void _on_tree_mouse_selected(const Vector2 &p_position, int p_mouse_button, Object *p_tree);
	bool _build_commit_menu();
	bool _build_file_menu(Tree *p_tree);
	void _on_context_menu_id(int p_id);
	void _on_file_multi_selected(TreeItem *p_item, int p_column, bool p_selected, Object *p_tree);

	// git_dock_history.cpp: History.
	void _fill_history();
	void _fill_commit(TreeItem *p_item);
	int _add_commit_file_rows(Tree *p_tree, TreeItem *p_parent, const Array &p_files, const String &p_hash, int p_max_rows);
	TreeItem *_add_commit_file_row(Tree *p_tree, TreeItem *p_parent, const Dictionary &p_file, const String &p_hash);
	void _on_history_item_collapsed(TreeItem *p_item);
	void _fill_commit_later(uint64_t p_item);
	void _load_more_commits();
	void _on_history_item_selected();
	void _build_history_filters();
	void _on_history_search_pressed();
	void _on_history_search_changed(const String &p_text);
	void _on_history_search_input(const Ref<InputEvent> &p_event);
	void _apply_history_search();
	void _set_history_filter(const String &p_path, const String &p_query);
	void _scroll_to_history();
	void _undo_last_commit();
	void _confirm_revert(const String &p_hash, const String &p_summary);
	void _on_revert_confirmed();
	void _restore_version(const String &p_revision, const String &p_path, const String &p_what);
	void _show_branch_here(const String &p_hash);

	// git_dock_stashes.cpp: stashing, and the Stashes section.
	void _build_stashes(Control *p_parent);
	void _fill_stashes();
	void _fill_stash(TreeItem *p_item);
	void _fill_stash_later(uint64_t p_item);
	void _on_stash_item_collapsed(TreeItem *p_item);
	void _on_stash_item_selected();
	void _on_stashes_gui_input(const Ref<InputEvent> &p_event);
	void _set_stash_hovered(TreeItem *p_item);
	void _on_stash_button_clicked(TreeItem *p_item, int p_column, int p_id, int p_mouse_button);
	bool _build_stash_menu();
	void _stash(bool p_staged);
	void _on_stash_dialog_confirmed();
	Error _run_stash(bool p_staged, const String &p_name = String());
	void _stash_and_switch();
	void _restore_stash(const String &p_hash);
	void _confirm_delete_stash(const String &p_hash);
	void _on_stash_delete_confirmed();
	void _restore_stash_merging(const String &p_hash);

	// git_dock_ignore.cpp: Ignore..., for new files.
	void _build_ignore_dialog();
	String _ignore_file_for(const String &p_path) const;
	PackedStringArray _file_ignore_lines(const String &p_path, const String &p_ignore_file, const PackedStringArray &p_untracked) const;
	void _on_ignore_folder_selected(int p_index);
	PackedStringArray _ignore_lines() const;
	void _update_ignore_effect();
	void _on_ignore_confirmed();

	// git_dock_export.cpp: keeping the addon out of exports (Godot 4.7).
	void _build_leftovers_banner(Control *p_parent);
	void _update_leftovers_banner();
	void _resolve_leftovers(bool p_put_back);
	void _resolve_leftovers_confirmed(bool p_put_back);
	void _build_export_banner(Control *p_parent);
	void _update_export_banner();
	void _leave_out_of_exports();
	void _dismiss_export_offer();

	// git_dock_conflicts.cpp: the Conflicts section, and resolving.
	void _build_conflicts(Control *p_parent);
	void _fill_conflicts(const Array &p_status);
	void _on_conflict_selected();
	void _show_conflict(const String &p_path);
	void _on_diff_options_changed();
	void _resolve_conflict(const String &p_path, const String &p_text, const String &p_side, const Dictionary &p_choices = Dictionary());
	void _on_conflict_text(const String &p_path, const String &p_text);
	void _on_conflict_side(const String &p_path, const String &p_side);
	void _on_conflict_settings(const String &p_path, const Dictionary &p_choices);

	// git_dock_diff.cpp: which file the Diff panel shows.
	void _show_diff(const String &p_path, bool p_staged, bool p_focus);
	void _show_commit_diff(const String &p_hash, const String &p_path, bool p_focus, bool p_stash = false);
	void _update_diff();
	void _add_image_versions(Dictionary &r_diff, const String &p_old_version, const String &p_new_version);
	void _select_diff_row();

	// git_dock_status.cpp: the status strip.
	void _build_status_strip(Control *p_parent);
	void _report(Error p_err, const String &p_action);
	void _set_status(StatusKind p_kind, const String &p_text);
	void _update_status();
	void _update_status_style();
	void _on_status_button();
	void _show_status_log();
	Ref<StyleBoxFlat> _tinted_panel(const Color &p_tint) const;
	void _build_operation_banner(Control *p_parent);
	void _update_operation_banner();
	bool _in_operation() const;
	bool _in_merge() const;
	void _confirm_abort_merge();
	void _finish_resolved_merge();
	String _operation_name() const;
	void _on_operation_abort();

	// git_dock_editor.cpp: keeping the rest of the editor in step (saving first, reloading
	// scenes, FileSystem dock colors).
	bool _ask_to_save(const String &p_verb, const Callable &p_then);
	void _on_unsaved_confirmed();
	void _on_unsaved_custom_action(const StringName &p_action);
	void _remember_open_scenes();
	void _reload_changed_scenes();
	bool _is_filesystem_colors_enabled() const;
	void _update_filesystem_colors(const Array &p_status);
	void _register_settings();
	bool _is_change_marks_enabled() const;
	void _on_editor_settings_changed();
	void _build_settings_dialog();
	void _show_settings_dialog();
	void _on_setting_toggled(bool p_on, int p_index);

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

	// git_dock_merge.cpp: merging a branch into the current one.
	void _build_merge_button(Control *p_parent);
	void _update_merge_button(bool p_busy, const String &p_in_operation);
	void _show_merge_dialog();
	void _fill_merge_tree();
	void _on_merge_row_selected();
	void _update_merge_preview();
	void _on_merge_confirmed();
	void _on_merge_search_submitted(const String &p_text);
	void _on_merge_search_input(const Ref<InputEvent> &p_event);
	String _merge_branch_label() const;

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
	void _ask_to_start_merge(const PackedStringArray &p_conflicts, const String &p_upstream);
	void _on_pull_merge_canceled();
	void _on_pull_merge_confirmed();
	void _on_pull_pressed();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void refresh();
	void set_diff_dock(GitDiffDock *p_dock);

	// For the FileSystem dock's right-click menu (GitFileSystemMenu; in git_dock_editor.cpp).
	PackedStringArray get_changed_paths(const PackedStringArray &p_res_paths, bool p_staged) const;
	void show_change(const String &p_path, int p_line = 0);
	void discard_changes(const PackedStringArray &p_paths);
	GitScriptMarks *get_script_marks() const { return script_marks; }
	String get_repo_path(const String &p_res_path) const;
	void show_file_history(const String &p_path);
	void show_commit(const String &p_hash);
	void show_line_commit(const String &p_path, const String &p_text, int p_line);
	bool can_ignore(const String &p_path) const;
	void set_reload_export_presets(const Callable &p_reload);
	void show_ignore(const String &p_path);
	// Git LFS locks, for the FileSystem menu: "lock", "unlock" or "" (neither can be done).
	String get_lock_action(const String &p_path) const;
	void lock_file(const String &p_path, bool p_lock);

	GitDock();
};
