// The Git dock: building it, keeping it up to date (refresh), the toolbar and action row, and
// the local actions (stage, discard, commit, branches). The rest lives in git_dock_lists.cpp,
// git_dock_status.cpp and git_dock_network.cpp.

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/core/math.hpp>

#include "editor/file_opener.h"
#include "editor/ui_text.h"

using namespace godot_git;

void GitDock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("refresh"), &GitDock::refresh);
}

GitDock::GitDock() {
	set_name("Git");
	set_title("Git");
	set_layout_key("GodotGit");
	set_icon_name("VcsBranches");
	set_default_slot(DOCK_SLOT_RIGHT_UL);

	const float scale = EditorInterface::get_singleton()->get_editor_scale();

	VBoxContainer *main_vb = memnew(VBoxContainer);
	add_child(main_vb);

	no_repo_label = memnew(Label);
	no_repo_label->set_text("This project isn't inside a git repository.");
	no_repo_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	no_repo_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	no_repo_label->hide();
	main_vb->add_child(no_repo_label);

	VBoxContainer *repo_vb = memnew(VBoxContainer);
	repo_vb->set_v_size_flags(SIZE_EXPAND_FILL);
	main_vb->add_child(repo_vb);
	repo_ui = repo_vb;

	// Branch picker, more.
	HBoxContainer *toolbar = memnew(HBoxContainer);
	repo_vb->add_child(toolbar);

	branch_select = memnew(OptionButton);
	branch_select->set_h_size_flags(SIZE_EXPAND_FILL);
	branch_select->set_clip_text(true);
	branch_select->set_fit_to_longest_item(false);
	branch_select->set_tooltip_text("Current branch. Pick another to switch to it.");
	branch_select->connect("item_selected", callable_mp(this, &GitDock::_on_branch_selected));
	toolbar->add_child(branch_select);

	more_menu = memnew(MenuButton);
	more_menu->set_flat(true);
	more_menu->set_tooltip_text("More actions");
	more_menu->get_popup()->connect("about_to_popup", callable_mp(this, &GitDock::_build_more_menu));
	more_menu->get_popup()->connect("id_pressed", callable_mp(this, &GitDock::_on_more_menu_id));
	toolbar->add_child(more_menu);

	_build_status_strip(repo_vb);

	// Syncing with the remote, in one row sharing the width: Fetch, then Pull / Push labeled with
	// how many commits each would move.
	sync_row = memnew(HBoxContainer);
	repo_vb->add_child(sync_row);

	fetch_button = memnew(Button);
	fetch_button->set_h_size_flags(SIZE_EXPAND_FILL);
	fetch_button->connect("pressed", callable_mp(this, &GitDock::_start_network).bind(NETWORK_FETCH));
	sync_row->add_child(fetch_button);

	pull_button = memnew(Button);
	pull_button->set_h_size_flags(SIZE_EXPAND_FILL);
	pull_button->connect("pressed", callable_mp(this, &GitDock::_start_network).bind(NETWORK_PULL));
	sync_row->add_child(pull_button);

	push_button = memnew(Button);
	push_button->set_h_size_flags(SIZE_EXPAND_FILL);
	push_button->connect("pressed", callable_mp(this, &GitDock::_start_network).bind(NETWORK_PUSH));
	sync_row->add_child(push_button);

	// The commit message, then Amend and Commit right under it.
	commit_message = memnew(TextEdit);
	commit_message->set_placeholder("Message (Ctrl+Enter to commit)");
	commit_message->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	commit_message->set_custom_minimum_size(Vector2(0, 64 * scale));
	commit_message->connect("text_changed", callable_mp(this, &GitDock::_update_actions));
	commit_message->connect("gui_input", callable_mp(this, &GitDock::_on_commit_message_input));
	repo_vb->add_child(commit_message);

	HBoxContainer *commit_row = memnew(HBoxContainer);
	repo_vb->add_child(commit_row);

	amend_check = memnew(CheckBox);
	amend_check->set_text("Amend");
	amend_check->connect("toggled", callable_mp(this, &GitDock::_on_amend_toggled));
	commit_row->add_child(amend_check);

	commit_button = memnew(Button);
	commit_button->set_text("Commit");
	commit_button->set_h_size_flags(SIZE_EXPAND_FILL);
	commit_button->connect("pressed", callable_mp(this, &GitDock::_commit));
	commit_row->add_child(commit_button);

	_build_lists(repo_vb);

	// Menus and dialogs.
	context_menu = memnew(PopupMenu);
	context_menu->connect("id_pressed", callable_mp(this, &GitDock::_on_context_menu_id));
	add_child(context_menu);

	discard_confirm = memnew(ConfirmationDialog);
	discard_confirm->set_title("Discard Changes");
	discard_confirm->set_ok_button_text("Discard");
	discard_confirm->connect("confirmed", callable_mp(this, &GitDock::_on_discard_confirmed));
	add_child(discard_confirm);

	branch_dialog = memnew(ConfirmationDialog);
	branch_dialog->set_title("New Branch");
	branch_dialog->set_ok_button_text("Create");
	branch_dialog->connect("confirmed", callable_mp(this, &GitDock::_on_branch_dialog_confirmed));
	add_child(branch_dialog);

	VBoxContainer *branch_vb = memnew(VBoxContainer);
	branch_dialog->add_child(branch_vb);
	Label *branch_label = memnew(Label);
	branch_label->set_text("Create a branch from the current commit and switch to it:");
	branch_vb->add_child(branch_label);
	branch_name_edit = memnew(LineEdit);
	branch_name_edit->set_placeholder("Branch name");
	branch_vb->add_child(branch_name_edit);
	branch_dialog->register_text_enter(branch_name_edit);

	// Timers. Saves of scenes, scripts and resources reach us through "filesystem_changed", but
	// Project Settings writes project.godot directly; checking its modified time is one cheap stat.
	project_file_timer = memnew(Timer);
	project_file_timer->set_wait_time(1.0);
	project_file_timer->set_autostart(true);
	project_file_timer->connect("timeout", callable_mp(this, &GitDock::_check_project_file));
	add_child(project_file_timer);

	auto_fetch_timer = memnew(Timer); // Started on READY; see _on_auto_fetch_timer.
	auto_fetch_timer->connect("timeout", callable_mp(this, &GitDock::_on_auto_fetch_timer));
	add_child(auto_fetch_timer);
}

void GitDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			_update_icons();
		} break;

		case NOTIFICATION_READY: {
			EditorFileSystem *fs = EditorInterface::get_singleton()->get_resource_filesystem();
			fs->connect("filesystem_changed", callable_mp(this, &GitDock::refresh));
			_update_status_style(); // Needs the commit box's theme, which isn't final at THEME_CHANGED.
			refresh();
			auto_fetch_timer->start(10.0); // First check soon after opening, then every minute.
		} break;

		case NOTIFICATION_APPLICATION_FOCUS_IN: {
			// Pick up changes made outside the editor (terminal, other git tools).
			if (is_node_ready()) {
				refresh();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			// Don't let a login window nobody finishes keep the editor from closing.
			GitRepository::cancel_network();
			_finish_network_thread();
		} break;
	}
}

void GitDock::_update_icons() {
	fetch_button->set_button_icon(get_theme_icon("Reload", "EditorIcons"));
	more_menu->set_button_icon(get_theme_icon("GuiTabMenuHl", "EditorIcons"));
	pull_button->set_button_icon(get_theme_icon("MoveDown", "EditorIcons"));
	push_button->set_button_icon(get_theme_icon("MoveUp", "EditorIcons"));
	staged_pane.action->set_button_icon(get_theme_icon("ZoomLess", "EditorIcons"));
	changes_pane.action->set_button_icon(get_theme_icon("ZoomMore", "EditorIcons"));
	changes_pane.discard->set_button_icon(get_theme_icon("UndoRedo", "EditorIcons"));

	// The trees sit inside the panes' own panels; drop their frames so it's one surface.
	Ref<StyleBoxEmpty> empty;
	empty.instantiate();
	for (Tree *tree : { staged_pane.tree, changes_pane.tree, history_tree }) {
		tree->add_theme_stylebox_override("panel", empty);
		tree->add_theme_stylebox_override("focus", empty);
	}

	// Header "all" buttons get the exact look and size of the rows' buttons: same styleboxes as
	// the Tree's cell buttons, same spacing.
	const Ref<StyleBox> tree_pressed = changes_pane.tree->get_theme_stylebox("button_pressed");
	const Ref<StyleBox> tree_hover = changes_pane.tree->get_theme_stylebox("button_hover");
	Ref<StyleBoxEmpty> flat;
	flat.instantiate();
	for (Side side : { SIDE_LEFT, SIDE_TOP, SIDE_RIGHT, SIDE_BOTTOM }) {
		flat->set_content_margin(side, tree_pressed->get_margin(side));
	}
	for (FilePane *pane : { &staged_pane, &changes_pane }) {
		pane->buttons->add_theme_constant_override("separation", pane->tree->get_theme_constant("button_margin"));
		for (Button *button : { pane->discard, pane->action }) {
			if (!button) {
				continue;
			}
			button->add_theme_stylebox_override("normal", flat);
			button->add_theme_stylebox_override("disabled", flat);
			button->add_theme_stylebox_override("focus", empty);
			button->add_theme_stylebox_override("hover", tree_hover);
			button->add_theme_stylebox_override("pressed", tree_pressed);
			button->add_theme_stylebox_override("hover_pressed", tree_pressed);
		}
	}
	_queue_align_header_buttons();

	const Color dim = _dim_color();
	const int text_inset = changes_pane.tree->get_theme_constant("inner_item_margin_left");
	for (Label *label : { staged_pane.empty_label, changes_pane.empty_label, history_empty }) {
		label->add_theme_color_override("font_color", dim);
		Object::cast_to<MarginContainer>(label->get_parent())->add_theme_constant_override("margin_left", text_inset);
	}
	status_button->set_button_icon(get_theme_icon("Close", "EditorIcons"));
	status_label->add_theme_stylebox_override("normal", empty);
	status_label->add_theme_stylebox_override("focus", empty);

	// A thin bar in the accent color on a faint track, rather than the chunky default ProgressBar.
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	Ref<StyleBoxFlat> track;
	track.instantiate();
	track->set_bg_color(get_theme_color("font_color", "Editor") * Color(1, 1, 1, 0.1));
	track->set_corner_radius_all(Math::round(2 * scale));
	Ref<StyleBoxFlat> fill = track->duplicate();
	fill->set_bg_color(get_theme_color("accent_color", "Editor"));
	status_progress->add_theme_stylebox_override("background", track);
	status_progress->add_theme_stylebox_override("fill", fill);
	_update_status_style();

	staged_pane.count->add_theme_color_override("font_color", dim);
	changes_pane.count->add_theme_color_override("font_color", dim);
	for (FilePane *pane : { &staged_pane, &changes_pane }) {
		pane->added->add_theme_color_override("font_color", get_theme_color("success_color", "Editor"));
		pane->removed->add_theme_color_override("font_color", get_theme_color("error_color", "Editor"));
	}

	if (is_node_ready()) {
		refresh();
	}
}

// Built each time it opens, so it only lists what applies right now.
void GitDock::_build_more_menu() {
	PopupMenu *more = more_menu->get_popup();
	more->clear();

	more->add_icon_item(get_theme_icon("ZoomMore", "EditorIcons"), "Stage All Changes", MORE_STAGE_ALL);
	more->set_item_disabled(more->get_item_count() - 1, unstaged_paths.is_empty());
	more->add_icon_item(get_theme_icon("ZoomLess", "EditorIcons"), "Unstage All Changes", MORE_UNSTAGE_ALL);
	more->set_item_disabled(more->get_item_count() - 1, staged_count == 0);
	more->add_icon_item(get_theme_icon("UndoRedo", "EditorIcons"), "Discard All Changes...", MORE_DISCARD_ALL);
	more->set_item_disabled(more->get_item_count() - 1, unstaged_paths.is_empty());
	more->add_separator();
	if (has_commits) {
		more->add_icon_item(get_theme_icon("VcsBranches", "EditorIcons"), "New Branch...", MORE_NEW_BRANCH);
		more->add_separator();
	}
	if (sync_status.get("has_remotes", false)) {
		more->add_check_item("Fetch Automatically", MORE_AUTO_FETCH);
		more->set_item_checked(more->get_item_count() - 1, _is_auto_fetch_enabled());
		more->set_item_tooltip(more->get_item_count() - 1, "Check the remote for new commits every few minutes, in the background. It never changes your files and never asks you to sign in.");
		more->add_separator();
	}
	more->add_icon_item(get_theme_icon("Reload", "EditorIcons"), "Refresh", MORE_REFRESH);
	more->add_icon_item(get_theme_icon("Folder", "EditorIcons"), "Open Repository Folder", MORE_OPEN_FOLDER);
}

String GitDock::_to_res_path(const String &p_path) const {
	return ProjectSettings::get_singleton()->localize_path(repo->get_workdir().path_join(p_path));
}

Ref<Texture2D> GitDock::_file_icon(const String &p_path) const {
	// Files inside the Godot project get the same icon the FileSystem dock shows.
	const String local = _to_res_path(p_path);
	if (local.begins_with("res://")) {
		const String type = EditorInterface::get_singleton()->get_resource_filesystem()->get_file_type(local);
		if (!type.is_empty() && has_theme_icon(type, "EditorIcons")) {
			return get_theme_icon(type, "EditorIcons");
		}
	}
	return get_theme_icon("File", "EditorIcons");
}

Color GitDock::_status_color(const String &p_state) const {
	if (p_state == "new" || p_state == "untracked") {
		return get_theme_color("success_color", "Editor");
	}
	if (p_state == "deleted" || p_state == "conflicted") {
		return get_theme_color("error_color", "Editor");
	}
	return get_theme_color("warning_color", "Editor");
}

Color GitDock::_dim_color() const {
	return get_theme_color("font_color", "Tree") * Color(1, 1, 1, 0.5);
}

void GitDock::refresh() {
	if (repo.is_null()) {
		repo.instantiate();
	}
	if (!repo->is_open() && repo->open("res://") != OK) {
		repo_ui->hide();
		no_repo_label->show();
		set_title("Git");
		return;
	}
	no_repo_label->hide();
	repo_ui->show();

	project_file_time = FileAccess::get_modified_time("res://project.godot");
	sync_status = repo->get_sync_status();
	_fill_branches();

	const Array status = repo->get_status();
	_fill_file_pane(staged_pane, status, repo->get_line_stats(true));
	_fill_file_pane(changes_pane, status, repo->get_line_stats(false));

	staged_count = staged_pane.file_count;
	set_title(status.is_empty() ? String("Git") : vformat("Git (%d)", status.size()));

	_fill_history();
	_update_actions();
	_update_status();
	_queue_align_header_buttons();
}

void GitDock::_check_project_file() {
	if (repo.is_valid() && repo->is_open() && FileAccess::get_modified_time("res://project.godot") != project_file_time) {
		refresh();
	}
}

void GitDock::_fill_branches() {
	const String current = sync_status.get("branch", String());
	PackedStringArray local = repo->get_branches();
	if (!current.is_empty() && !local.has(current)) {
		// Unborn branch (no commits yet) or detached HEAD.
		local.push_back(current);
	}
	local.sort();

	// Remote branches that don't already have a local branch of the same name.
	PackedStringArray remote;
	for (const String &name : repo->get_remote_branches()) {
		const int slash = name.find("/");
		if (!local.has(slash >= 0 ? name.substr(slash + 1) : name)) {
			remote.push_back(name);
		}
	}
	remote.sort();

	const Ref<Texture2D> branch_icon = get_theme_icon("VcsBranches", "EditorIcons");
	branch_select->clear();
	for (const String &name : local) {
		branch_select->add_icon_item(branch_icon, name);
		const int index = branch_select->get_item_count() - 1;
		branch_select->set_item_metadata(index, name);
		if (name == current) {
			branch_select->select(index);
		}
	}
	if (!remote.is_empty()) {
		branch_select->add_separator("Remote branches");
		const Ref<Texture2D> remote_icon = get_theme_icon("ArrowDown", "EditorIcons");
		for (const String &name : remote) {
			branch_select->add_icon_item(remote_icon, name);
			branch_select->set_item_metadata(branch_select->get_item_count() - 1, name);
			branch_select->set_item_tooltip(branch_select->get_item_count() - 1, "Check out as a local branch tracking " + name);
		}
	}
	branch_select->add_separator();
	branch_select->add_icon_item(get_theme_icon("Add", "EditorIcons"), "New Branch...");
	branch_select->set_item_metadata(branch_select->get_item_count() - 1, Variant());
}

// Updates Commit / Pull / Push. Pull and Push only appear when there's a remote to talk to,
// show how many commits they'd move, and their tooltips say exactly what they'll do.
void GitDock::_update_actions() {
	const bool has_remotes = sync_status.get("has_remotes", false);
	const String upstream = sync_status.get("upstream", String());
	const String branch = sync_status.get("branch", String());
	const int ahead = sync_status.get("ahead", 0);
	const int behind = sync_status.get("behind", 0);
	const bool has_message = !commit_message->get_text().strip_edges().is_empty();
	// A background fetch doesn't count as busy: it only updates remote-tracking refs.
	const NetworkOp shown = _shown_network_op();
	// A pull, push or switch rewrites the repository from the worker thread; don't commit meanwhile.
	const bool syncing = shown == NETWORK_PULL || shown == NETWORK_PUSH || shown == NETWORK_SWITCH || shown == NETWORK_COMMIT;
	const bool busy = shown != NETWORK_NONE;

	// The worker rewrites the repository during a pull or push; switching branches or bulk
	// changes meanwhile would race it.
	branch_select->set_disabled(syncing);
	more_menu->set_disabled(syncing);

	sync_row->set_visible(has_remotes);
	fetch_button->set_disabled(busy);
	fetch_button->set_text(shown == NETWORK_FETCH ? String("Fetching...") : String("Fetch"));
	fetch_button->set_tooltip_text("Fetch: check the remote for new commits, without changing your files.");

	// Amend: only while the last commit is yours alone. Once pushed, rewriting it would leave
	// teammates with a commit that no longer exists here.
	if (amend_check->is_pressed() && (!has_commits || last_commit_pushed)) {
		amend_check->set_pressed(false); // E.g. just pushed. Puts the draft back.
	}
	const bool amending = amend_check->is_pressed();
	amend_check->set_disabled(syncing || !has_commits || last_commit_pushed);
	if (!has_commits) {
		amend_check->set_tooltip_text("Nothing to amend yet: there are no commits.");
	} else if (last_commit_pushed) {
		amend_check->set_tooltip_text("The last commit is already pushed, so it can't be amended: that would change history your teammates may have.");
	} else {
		amend_check->set_tooltip_text(vformat("Redo the last commit (%s) instead of making a new one: change its message, and add what's staged.", last_commit_id));
	}

	commit_message->set_editable(shown != NETWORK_COMMIT);
	if (shown == NETWORK_COMMIT) {
		commit_button->set_text(network_amend ? "Amending..." : "Committing...");
	} else {
		commit_button->set_text(amending ? "Amend" : "Commit");
	}
	if (amending) {
		commit_button->set_disabled(syncing || !has_message);
		if (!has_message) {
			commit_button->set_tooltip_text("Write a commit message first.");
		} else if (staged_count == 0) {
			commit_button->set_tooltip_text(vformat("Replace commit %s with this message.", last_commit_id));
		} else {
			commit_button->set_tooltip_text(vformat("Replace commit %s with this message, adding %s.", last_commit_id, plural(staged_count, "staged file", "staged files")));
		}
	} else {
		commit_button->set_disabled(syncing || staged_count == 0 || !has_message);
		if (staged_count == 0) {
			commit_button->set_tooltip_text("Stage some changes first.");
		} else if (!has_message) {
			commit_button->set_tooltip_text("Write a commit message first.");
		} else {
			commit_button->set_tooltip_text(vformat("Commit %s to %s.", plural(staged_count, "staged file", "staged files"), branch));
		}
	}

	// Pull: only when the branch tracks a remote branch. The count is as of the last fetch;
	// pulling always fetches first, so it stays enabled at 0.
	pull_button->set_visible(has_remotes && !upstream.is_empty());
	pull_button->set_disabled(busy);
	pull_button->set_text(shown == NETWORK_PULL ? String("Pulling...") : (behind > 0 ? vformat("Pull %d", behind) : String("Pull")));
	pull_button->set_tooltip_text(behind > 0
					? vformat("Pull: get %s from %s.", plural(behind, "new commit", "new commits"), upstream)
					: vformat("Pull from %s. Nothing new as of the last fetch.", upstream));

	// Push: publishes the branch first time, then sends new commits. Disabled with nothing to send.
	push_button->set_visible(has_remotes && has_commits);
	if (upstream.is_empty()) {
		push_button->set_text(shown == NETWORK_PUSH ? String("Publishing...") : String("Publish"));
		push_button->set_tooltip_text(vformat("Publish: push %s to the remote and start tracking it.", branch));
		push_button->set_disabled(busy);
	} else {
		push_button->set_text(shown == NETWORK_PUSH ? String("Pushing...") : (ahead > 0 ? vformat("Push %d", ahead) : String("Push")));
		push_button->set_tooltip_text(ahead > 0 ? vformat("Push: send %s to %s.", plural(ahead, "commit", "commits"), upstream) : vformat("Nothing to push; %s has all your commits.", upstream));
		push_button->set_disabled(busy || ahead == 0);
	}
}

void GitDock::_on_more_menu_id(int p_id) {
	switch (p_id) {
		case MORE_REFRESH: {
			refresh();
		} break;
		case MORE_STAGE_ALL: {
			_report(repo->stage_all(), "Stage all");
			refresh();
		} break;
		case MORE_UNSTAGE_ALL: {
			_report(repo->unstage_all(), "Unstage all");
			refresh();
		} break;
		case MORE_DISCARD_ALL: {
			if (!unstaged_paths.is_empty()) {
				_confirm_discard(unstaged_paths);
			}
		} break;
		case MORE_NEW_BRANCH: {
			branch_name_edit->clear();
			branch_dialog->popup_centered(Vector2i(360, 0) * EditorInterface::get_singleton()->get_editor_scale());
			branch_name_edit->grab_focus();
		} break;
		case MORE_AUTO_FETCH: {
			const bool enable = !_is_auto_fetch_enabled();
			EditorInterface::get_singleton()->get_editor_settings()->set_project_metadata("godot_git", "auto_fetch", enable);
			if (!enable && auto_fetch_failed && status_kind == STATUS_WARNING) {
				_set_status(STATUS_IDLE, String()); // Its failure message is moot now.
			}
			auto_fetch_failed = false;
			last_auto_fetch_attempt = 0;
		} break;
		case MORE_OPEN_FOLDER: {
			OS::get_singleton()->shell_show_in_file_manager(repo->get_workdir(), true);
		} break;
	}
}

// --- Actions ----------------------------------------------------------------

void GitDock::_open_path(const String &p_path) {
	if (p_path.is_empty()) {
		return;
	}
	switch (open_file(repo->get_workdir().path_join(p_path), repo->get_workdir())) {
		case ERR_FILE_NOT_FOUND: {
			_set_status(STATUS_NEUTRAL, vformat("\"%s\" was deleted, so there's nothing to open.", p_path.get_file()));
		} break;
		case ERR_UNAVAILABLE: {
			_set_status(STATUS_WARNING, vformat("No code editor found to open \"%s\". Set one in Editor Settings > Text Editor > External, or install VS Code.", p_path.get_file()));
		} break;
		default:
			break;
	}
}

void GitDock::_stage_paths(const PackedStringArray &p_paths, bool p_stage) {
	for (const String &path : p_paths) {
		const Error err = p_stage ? repo->stage(path) : repo->unstage(path);
		if (err != OK) {
			_report(err, p_stage ? "Stage" : "Unstage");
			break;
		}
	}
	refresh();
}

void GitDock::_confirm_discard(const PackedStringArray &p_paths) {
	if (p_paths.is_empty()) {
		return;
	}
	pending_discard = p_paths;
	if (p_paths.size() == 1) {
		discard_confirm->set_text(vformat("Discard your changes to \"%s\"?\nNew files are deleted. This can't be undone.", p_paths[0]));
	} else {
		discard_confirm->set_text(vformat("Discard your changes to %d files?\nNew files are deleted. This can't be undone.", p_paths.size()));
	}
	discard_confirm->popup_centered();
}

void GitDock::_on_discard_confirmed() {
	for (const String &path : pending_discard) {
		const Error err = repo->discard(path);
		if (err != OK) {
			_report(err, "Discard");
			break;
		}
	}
	pending_discard.clear();
	EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	refresh();
}

void GitDock::_on_branch_selected(int p_index) {
	const Variant target = branch_select->get_item_metadata(p_index);
	if (target.get_type() != Variant::STRING) {
		// "New Branch..." item: put the picker back and ask for a name.
		refresh();
		_on_more_menu_id(MORE_NEW_BRANCH);
		return;
	}
	if (String(target) == repo->get_current_branch()) {
		return;
	}
	if (repo->uses_lfs()) {
		// May download LFS files: in the background, with progress and Cancel.
		network_branch = target;
		_fill_branches(); // Shows the current branch until the switch is done.
		_start_network(NETWORK_SWITCH);
		return;
	}
	const Error err = repo->checkout_branch(target);
	_report(err, "Switch branch");
	if (err == OK) {
		EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	}
	refresh();
	if (err == OK) {
		_set_status(STATUS_SUCCESS, vformat("Switched to %s", repo->get_current_branch()));
	}
}

void GitDock::_on_branch_dialog_confirmed() {
	const String name = branch_name_edit->get_text().strip_edges();
	if (name.is_empty()) {
		return;
	}
	_report(repo->create_branch(name), "Create branch");
	refresh();
}

void GitDock::_on_commit_message_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && !key->is_echo() && key->is_command_or_control_pressed() &&
			(key->get_keycode() == KEY_ENTER || key->get_keycode() == KEY_KP_ENTER)) {
		commit_message->accept_event();
		if (!commit_button->is_disabled()) {
			_commit();
		}
	}
}

// Ticking Amend fills in the last commit's message to edit; unticking puts back what was
// there before, unless the message was edited meanwhile.
void GitDock::_on_amend_toggled(bool p_on) {
	if (p_on) {
		amend_saved_draft = commit_message->get_text();
		commit_message->set_text(last_commit_message);
		const int last_line = commit_message->get_line_count() - 1;
		commit_message->set_caret_line(last_line);
		commit_message->set_caret_column(commit_message->get_line(last_line).length());
	} else if (commit_message->get_text() == last_commit_message) {
		commit_message->set_text(amend_saved_draft);
	}
	_update_actions();
}

void GitDock::_commit() {
	const String message = commit_message->get_text().strip_edges();
	const bool amending = amend_check->is_pressed();
	if (message.is_empty() || (staged_count == 0 && !amending)) {
		return;
	}
	const int files = staged_count;
	const String old_id = last_commit_id;
	if (repo->commit_runs_git(amending)) {
		// Hooks or signing: git does it, which may take a while (a hook can run a linter).
		network_commit_message = message;
		network_amend = amending;
		network_commit_files = files;
		network_amended_id = old_id;
		_start_network(NETWORK_COMMIT);
		return;
	}
	const Error err = amending ? repo->amend(message) : repo->commit(message);
	_report(err, amending ? "Amend" : "Commit");
	if (err == OK) {
		amend_check->set_pressed_no_signal(false);
		amend_saved_draft = String();
		commit_message->clear();
	}
	refresh();
	if (err == OK) {
		_report_commit(amending, files, old_id);
	}
}

// After refresh(), so last_commit_id is the new commit.
void GitDock::_report_commit(bool p_amended, int p_files, const String &p_old_id) {
	if (!p_amended) {
		_set_status(STATUS_SUCCESS, vformat("Committed %s (%s)", last_commit_id, plural(p_files, "file", "files")));
	} else if (p_files > 0) {
		_set_status(STATUS_SUCCESS, vformat("Amended %s, now %s (%s added)", p_old_id, last_commit_id, plural(p_files, "file", "files")));
	} else {
		_set_status(STATUS_SUCCESS, vformat("Amended %s, now %s", p_old_id, last_commit_id));
	}
}
