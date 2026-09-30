// The Git dock: building it, keeping it up to date (refresh), the toolbar, which actions are
// enabled, and the file actions (open, stage, discard). The rest is split by area into the other
// git_dock_*.cpp files (commit box, branches, lists, rows, history, diff, status, network, ...).

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_file_system_directory.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/core/math.hpp>

#include "addon_files.h"
#include "build_info.h"
#include "editor/file_opener.h"
#include "editor/filesystem_colors.h"
#include "editor/git_colors.h"
#include "editor/git_dock_util.h"
#include "editor/script_marks.h"
#include "editor/ui_text.h"

using namespace godot_git;

void GitDock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("refresh"), &GitDock::refresh);
	// For the smoke test.
	ClassDB::bind_method(D_METHOD("show_file_history", "path"), &GitDock::show_file_history);
	ClassDB::bind_method(D_METHOD("show_commit", "hash"), &GitDock::show_commit);
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

	_build_setup(main_vb);

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
	// "feature/inventory-system-re…", not cut off mid-letter.
	branch_select->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
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
	_build_operation_banner(repo_vb);

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

	switch_confirm = memnew(ConfirmationDialog);
	switch_confirm->set_title("Switch Branch");
	switch_confirm->set_ok_button_text("Switch Anyway");
	switch_confirm->connect("confirmed", callable_mp(this, &GitDock::_switch_branch).bind(String()));
	add_child(switch_confirm);

	branch_dialog = memnew(ConfirmationDialog);
	branch_dialog->set_title("New Branch");
	branch_dialog->set_ok_button_text("Create");
	branch_dialog->connect("confirmed", callable_mp(this, &GitDock::_on_branch_dialog_confirmed));
	add_child(branch_dialog);

	VBoxContainer *branch_vb = memnew(VBoxContainer);
	branch_dialog->add_child(branch_vb);
	branch_dialog_label = memnew(Label);
	branch_vb->add_child(branch_dialog_label);
	branch_name_edit = memnew(LineEdit);
	branch_name_edit->set_placeholder("Branch name");
	branch_vb->add_child(branch_name_edit);
	branch_dialog->register_text_enter(branch_name_edit);
	_build_branch_dialogs();

	unsaved_confirm = _make_confirm("Unsaved Changes", String(), callable_mp(this, &GitDock::_on_unsaved_confirmed));
	unsaved_confirm->connect("custom_action", callable_mp(this, &GitDock::_on_unsaved_custom_action));
	unsaved_skip = unsaved_confirm->add_button("", false, "skip");

	large_confirm = _make_confirm("Large Files", "Commit Anyway", callable_mp(this, &GitDock::_on_large_confirmed));
	revert_confirm = _make_confirm("Revert Commit", "Revert", callable_mp(this, &GitDock::_on_revert_confirmed));

	filesystem_colors = memnew(GitFileSystemColors);
	add_child(filesystem_colors);
	script_marks = memnew(GitScriptMarks);
	script_marks->connect("show_in_diff_requested", callable_mp(this, &GitDock::show_change));
	script_marks->connect("history_requested", callable_mp(this, &GitDock::show_file_history));
	script_marks->connect("line_commit_requested", callable_mp(this, &GitDock::show_line_commit));
	add_child(script_marks);

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
			file_icons.clear();
			// History's rows carry the theme's colors from when they were built, and it's rebuilt
			// only when the commits change: without this, a light theme kept the dark theme's
			// colors (white ages, black note bars). Expanded commits are re-expanded.
			history_shown.clear();
			_update_icons();
		} break;

		case NOTIFICATION_READY: {
			EditorFileSystem *fs = EditorInterface::get_singleton()->get_resource_filesystem();
			fs->connect("filesystem_changed", callable_mp(this, &GitDock::refresh));
			_update_status_style(); // Needs the commit box's theme, which isn't final at THEME_CHANGED.
			_check_git();
			_register_settings();
			script_marks->set_enabled(_is_change_marks_enabled());
			EditorInterface::get_singleton()->get_editor_settings()->connect("settings_changed", callable_mp(this, &GitDock::_on_editor_settings_changed));
			refresh();
			auto_fetch_timer->start(10.0); // First check soon after opening, then every minute.
		} break;

		case NOTIFICATION_APPLICATION_FOCUS_IN: {
			// Pick up changes made outside the editor (terminal, other git tools).
			if (is_node_ready()) {
				if (git_missing) {
					_check_git(); // Maybe installed meanwhile.
				}
				refresh();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			// Don't let a login window nobody finishes keep the editor from closing.
			GitRepository::cancel_network();
			_finish_network_thread();
			_finish_stats_thread();
		} break;
	}
}

void GitDock::_update_icons() {
	no_repo_hint->add_theme_color_override("font_color", _dim_color());
	fetch_button->set_button_icon(get_theme_icon("Reload", "EditorIcons"));
	more_menu->set_button_icon(get_theme_icon("GuiTabMenuHl", "EditorIcons"));
	pull_button->set_button_icon(get_theme_icon("MoveDown", "EditorIcons"));
	push_button->set_button_icon(get_theme_icon("MoveUp", "EditorIcons"));
	_draw_own_icons();
	staged_pane.action->set_button_icon(_icon("GitMinus"));
	staged_pane.stash->set_button_icon(_icon("GitStash"));
	changes_pane.action->set_button_icon(_icon("Add"));
	changes_pane.discard->set_button_icon(get_theme_icon("UndoRedo", "EditorIcons"));
	history_search_button->set_button_icon(get_theme_icon("Search", "EditorIcons"));
	history_file_close->set_button_icon(get_theme_icon("Close", "EditorIcons"));

	// The trees sit inside the panes' own panels; drop their frames so it's one surface.
	Ref<StyleBoxEmpty> empty;
	empty.instantiate();
	for (Tree *tree : { staged_pane.tree, changes_pane.tree, stashes_tree, history_tree }) {
		tree->add_theme_stylebox_override("panel", empty);
		tree->add_theme_stylebox_override("focus", empty);
	}

	// Stash rows use the Tree's own buttons, which the 4.7 "modern" theme draws in the row's own
	// hover color, so they don't show being hovered (see _draw_row_buttons, which does the same).
	stashes_tree->remove_theme_stylebox_override("button_hover");
	if (stashes_tree->get_theme_stylebox("button_hover") == stashes_tree->get_theme_stylebox("hovered")) {
		stashes_tree->add_theme_stylebox_override("button_hover", stashes_tree->get_theme_stylebox("button_pressed"));
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
		for (Button *button : { pane->discard, pane->stash, pane->action }) {
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
	// History's search button looks like the other sections' header buttons; pressed while open.
	history_search_button->add_theme_stylebox_override("normal", flat);
	history_search_button->add_theme_stylebox_override("focus", empty);
	history_search_button->add_theme_stylebox_override("hover", tree_hover);
	history_search_button->add_theme_stylebox_override("pressed", tree_pressed);
	history_search_button->add_theme_stylebox_override("hover_pressed", tree_pressed);
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

	for (FoldableContainer *section : { staged_pane.container, changes_pane.container, stashes_pane, history_pane }) {
		_round_section(section);
	}

	for (FilePane *pane : { &staged_pane, &changes_pane }) {
		pane->added->add_theme_color_override("font_color", change_color(CHANGE_ADDED));
		pane->removed->add_theme_color_override("font_color", change_color(CHANGE_REMOVED));
		// The editor theme pads every Label on both sides, which set "+195" and "−14" two
		// paddings apart. Without it they're the header's own spacing apart: about a space.
		pane->added->add_theme_stylebox_override("normal", empty);
		pane->removed->add_theme_stylebox_override("normal", empty);
	}

	if (is_node_ready()) {
		refresh();
	}
}

// Built each time it opens, so it only lists what applies right now.
void GitDock::_build_more_menu() {
	PopupMenu *more = more_menu->get_popup();
	more->clear();

	more->add_icon_item(_icon("Add"), "Stage All Changes", MORE_STAGE_ALL);
	more->set_item_disabled(more->get_item_count() - 1, unstaged_paths.is_empty());
	more->add_icon_item(_icon("GitMinus"), "Unstage All Changes", MORE_UNSTAGE_ALL);
	more->set_item_disabled(more->get_item_count() - 1, staged_count == 0);
	more->add_icon_item(get_theme_icon("UndoRedo", "EditorIcons"), "Discard All Changes...", MORE_DISCARD_ALL);
	more->set_item_disabled(more->get_item_count() - 1, unstaged_paths.is_empty());
	more->add_icon_item(_icon("GitStash"), "Stash All Changes", MORE_STASH_ALL);
	more->set_item_disabled(more->get_item_count() - 1, unstaged_paths.is_empty() && staged_count == 0);
	more->set_item_tooltip(more->get_item_count() - 1, "Set every change aside, new files included, in a stash you can restore later (under Stashes).");
	more->add_separator();
	if (has_commits) {
		more->add_icon_item(get_theme_icon("VcsBranches", "EditorIcons"), "New Branch...", MORE_NEW_BRANCH);
	}
	if (!sync_status.get("has_remotes", false)) {
		more->add_icon_item(get_theme_icon("Add", "EditorIcons"), "Add Remote...", MORE_ADD_REMOTE);
		more->set_item_tooltip(more->get_item_count() - 1, "Connect this repository to one on GitHub, GitLab or another host, so you can push and pull.");
	}
	if (more->get_item_count() > 0 && !more->is_item_separator(more->get_item_count() - 1)) {
		more->add_separator();
	}
	if (sync_status.get("has_remotes", false)) {
		more->add_check_item("Fetch Automatically", MORE_AUTO_FETCH);
		more->set_item_checked(more->get_item_count() - 1, _is_auto_fetch_enabled());
		more->set_item_tooltip(more->get_item_count() - 1, "Check the remote for new commits every few minutes, in the background. It never changes your files and never asks you to sign in.");
	}
	more->add_check_item("Mark Changed Lines in Scripts", MORE_CHANGE_MARKS);
	more->set_item_checked(more->get_item_count() - 1, _is_change_marks_enabled());
	more->set_item_tooltip(more->get_item_count() - 1, "Mark lines added, changed or deleted since the last commit next to the line numbers in the script editor, unsaved edits included. Click a mark to see what was there.");
	more->add_check_item("Color Changed Files in FileSystem", MORE_FILESYSTEM_COLORS);
	more->set_item_checked(more->get_item_count() - 1, _is_filesystem_colors_enabled());
	more->set_item_tooltip(more->get_item_count() - 1, "Show changed files in the FileSystem dock in the colors of their status letters, and the folders holding them in a softer color.");
	more->add_separator();
	more->add_icon_item(get_theme_icon("Reload", "EditorIcons"), "Refresh", MORE_REFRESH);
	more->add_icon_item(get_theme_icon("Folder", "EditorIcons"), "Open Repository Folder", MORE_OPEN_FOLDER);

	// Where this library came from, so nobody has to take a binary on faith.
	more->add_separator();
	const String version = String::utf8(build_version());
	const String commit = String::utf8(build_commit());
	if (commit.is_empty()) {
		more->add_item(vformat(String::utf8("Godot Git %s · Local Build"), version), MORE_BUILD_INFO);
		more->set_item_disabled(more->get_item_count() - 1, true);
		more->set_item_tooltip(more->get_item_count() - 1, "Built outside the project's GitHub Actions (for example on your own computer), so there's no public build log to check it against.");
	} else {
		more->add_icon_item(get_theme_icon("ExternalLink", "EditorIcons"), vformat(String::utf8("Godot Git %s · Built by GitHub from %s"), version, commit.left(7)), MORE_BUILD_INFO);
		more->set_item_tooltip(more->get_item_count() - 1, vformat("Built by the project's public GitHub Actions from commit %s. Opens that build: its log shows exactly what was compiled, and releases come with signed attestations you can verify.", commit));
	}
}

String GitDock::_to_res_path(const String &p_path) const {
	// Plain string work for the usual case: localize_path can open directories to resolve a path,
	// and this runs for every file in the lists (0.26 ms each, half a second for 2,000 files).
	const String absolute = repo->get_workdir().path_join(p_path);
	const String project = ProjectSettings::get_singleton()->globalize_path("res://").trim_suffix("/") + "/";
	if (absolute.begins_with(project)) {
		return "res://" + absolute.substr(project.length());
	}
	return ProjectSettings::get_singleton()->localize_path(absolute);
}

// An icon by name: ours (see _draw_own_icons) or the editor's own.
Ref<Texture2D> GitDock::_icon(const String &p_name) const {
	return own_icons.has(p_name) ? Ref<Texture2D>(own_icons[p_name]) : get_theme_icon(p_name, "EditorIcons");
}

// Icons the editor doesn't have, drawn in its style (16 px, 2 px strokes, its icon grey): a plain
// minus to go with its plain plus ("Add"; its zoom icons, circled, looked out of place next to
// the thin undo arrow), and a filled tray with an arrow coming out (stash, the maintainer's pick)
// or going in (restore), since nothing of Godot's says "set aside". In the color of the editor's
// own icons in this theme (read from "Add"), so they match in the light theme too.
void GitDock::_draw_own_icons() {
	static const char *const svgs[][2] = {
		{ "GitMinus", "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'><path fill='#e0e0e0' d='M1 7h14v2H1z'/></svg>" },
		{ "GitStash", "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'><path fill='#e0e0e0' d='M1 8h4l1 2h4l1-2h4v6a1 1 0 0 1-1 1H2a1 1 0 0 1-1-1zm6-0.5h2v-3.5h2L8 0.5 5 4h2z'/></svg>" },
		{ "GitRestore", "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'><path fill='#e0e0e0' d='M1 8h4l1 2h4l1-2h4v6a1 1 0 0 1-1 1H2a1 1 0 0 1-1-1zm6-7h2v4h2l-3 3.5L5 5h2z'/></svg>" },
	};
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	Color color(0.878f, 0.878f, 0.878f);
	const Ref<Texture2D> add = get_theme_icon("Add", "EditorIcons");
	const Ref<Image> add_image = add.is_valid() ? add->get_image() : Ref<Image>();
	if (add_image.is_valid() && !add_image->is_empty()) {
		color = add_image->get_pixel(add_image->get_width() / 2, add_image->get_height() / 2); // The plus's middle.
		color.a = 1;
	}
	own_icons.clear();
	for (const auto &svg : svgs) {
		Ref<Image> image;
		image.instantiate();
		if (image->load_svg_from_string(String(svg[1]).replace("#e0e0e0", "#" + color.to_html(false)), scale) == OK) {
			own_icons[svg[0]] = ImageTexture::create_from_image(image);
		}
	}
}

Ref<Texture2D> GitDock::_file_icon(const String &p_path) {
	// Files inside the Godot project get the same icon the FileSystem dock shows.
	const String local = _to_res_path(p_path);
	const String type = local.begins_with("res://") ? _file_type(local) : String();
	if (!file_icons.has(type)) {
		// Theme lookups add up over thousands of rows, so each type's icon is looked up once.
		file_icons[type] = !type.is_empty() && has_theme_icon(type, "EditorIcons") ? get_theme_icon(type, "EditorIcons") : get_theme_icon("File", "EditorIcons");
	}
	return file_icons[type];
}

// The file's type as the FileSystem dock knows it ("GDScript", "Texture2D"), or "". Read a folder
// at a time: EditorFileSystem::get_file_type finds a file by going through its folder's list from
// the start, so 2,000 changed files in one folder took 0.3 s (most of a refresh's time).
String GitDock::_file_type(const String &p_res_path) {
	const String folder = p_res_path.get_base_dir();
	if (!folder_types.has(folder)) {
		Dictionary types;
		if (EditorFileSystemDirectory *dir = EditorInterface::get_singleton()->get_resource_filesystem()->get_filesystem_path(folder)) {
			for (int i = 0; i < dir->get_file_count(); i++) {
				types[dir->get_file(i)] = dir->get_file_type(i);
			}
		}
		folder_types[folder] = types;
	}
	return Dictionary(folder_types[folder]).get(p_res_path.get_file(), String());
}

Color GitDock::_status_color(const String &p_state) const {
	return status_color(p_state); // The one palette (git_colors.h).
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
		no_repo_ui->show();
		set_title("Git");
		filesystem_colors->set_colors(Dictionary(), Dictionary());
		script_marks->set_repository(Ref<GitRepository>(), String());
		return;
	}
	no_repo_ui->hide();
	repo_ui->show();
	folder_types.clear(); // Files may have come and gone since.

	project_file_time = FileAccess::get_modified_time("res://project.godot");
	sync_status = repo->get_sync_status();
	git_needs = git_missing ? repo->get_git_needs() : Dictionary();
	if (git_missing && git_warning.is_empty()) {
		git_warning = _git_missing_warning();
		if (!git_warning.is_empty()) {
			_set_status(STATUS_WARNING, git_warning);
		}
	}
	_fill_branches();

	const Array status = repo->get_status();
	// After a fetch: the files a pull would refuse for, so their rows and Pull can say so up front.
	operation = repo->get_operation();
	// Mid-merge, Pull waits for the operation anyway: a pull warning on the rows would be noise.
	pull_blockers = (int)sync_status.get("behind", 0) > 0 && !_in_operation() ? repo->get_pull_blockers() : PackedStringArray();
	_fill_file_pane(staged_pane, status);
	_fill_file_pane(changes_pane, status);
	_start_line_stats();

	staged_count = staged_pane.file_count;
	set_title(status.is_empty() ? String("Git") : vformat("Git (%d)", status.size()));

	_update_filesystem_colors(status);
	_fill_history();
	_fill_stashes();
	_update_actions();
	_update_status();
	_update_diff();
	script_marks->set_repository(repo, sync_status.get("head", String()));
	_queue_align_header_buttons();
}

void GitDock::_check_project_file() {
	if (repo.is_valid() && repo->is_open() && FileAccess::get_modified_time("res://project.godot") != project_file_time) {
		refresh();
	}
}

// Checked when the dock opens and, while git is missing, whenever the editor gets focus back:
// the user may have just installed it. Not on every refresh (see check_git() in git_cli.h).
void GitDock::_check_git() {
	git_missing = !GitRepository::check_git_installed();
	if (!git_missing && !git_warning.is_empty()) {
		if (status_kind == STATUS_WARNING && status_text == git_warning) {
			_set_status(STATUS_IDLE, String());
		}
		git_warning = String();
	}
}

// "Git isn't installed, so these won't work:" and a list, naming what this repository needs git
// for. Empty if nothing: a repository without hooks, LFS or remotes that need git works without it.
String GitDock::_git_missing_warning() const {
	PackedStringArray items;
	const String commit_reason = git_needs.get("commit", String());
	if (!commit_reason.is_empty()) {
		items.push_back(vformat("Commit (%s)", commit_reason));
	}
	const PackedStringArray ssh = git_needs.get("ssh_remotes", PackedStringArray());
	if (!ssh.is_empty()) {
		items.push_back(vformat("Fetch, Pull and Push with %s (SSH)", join_list(ssh)));
	}
	if (git_needs.get("pre_push", false)) {
		items.push_back("Push (this repository has a pre-push hook)");
	}
	if (git_needs.get("lfs", false)) {
		items.push_back("Pull, Push and staging, for files stored with Git LFS");
	}
	const PackedStringArray https = git_needs.get("https_remotes", PackedStringArray());
	if (!https.is_empty()) {
		items.push_back(vformat("Signing in to %s (for private repositories)", join_list(https)));
	}
	if (items.is_empty()) {
		return String();
	}
	String text = "Git isn't installed (or isn't on the PATH), so these won't work:";
	for (const String &item : items) {
		text += String::utf8("\n• ") + item;
	}
	return text + String("\nInstall it from git-scm.com, then come back to the editor.");
}

// Why p_op (NETWORK_FETCH / PULL / PUSH) can't work without git, or "" if it can (or git is
// installed). Commit is handled in _update_actions.
String GitDock::_needs_git(int p_op) const {
	if (!git_missing) {
		return String();
	}
	String remote = String(sync_status.get("upstream", String())).get_slice("/", 0);
	if (remote.is_empty()) {
		const PackedStringArray remotes = repo->get_remotes();
		remote = remotes.has("origin") || remotes.is_empty() ? String("origin") : remotes[0];
	}
	String reason;
	if (PackedStringArray(git_needs.get("ssh_remotes", PackedStringArray())).has(remote)) {
		reason = vformat("%s is an SSH remote, and SSH goes through git.", remote);
	} else if (p_op != NETWORK_FETCH && git_needs.get("lfs", false)) {
		reason = "This project stores files with Git LFS, which needs git.";
	} else if (p_op == NETWORK_PUSH && git_needs.get("pre_push", false)) {
		reason = "This repository has a pre-push hook, which only git can run.";
	}
	return reason.is_empty() ? reason : vformat("%s Git isn't installed (or isn't on the PATH); install it from git-scm.com.", reason);
}

// Enables and labels everything that acts on the repository (the branch picker, the ⋮ menu, the
// commit and sync rows, the operation banner) for the current state.
void GitDock::_update_actions() {
	// A background fetch doesn't count as busy: it only updates remote-tracking refs.
	const NetworkOp shown = _shown_network_op();
	// A pull, push or switch rewrites the repository from the worker thread; don't commit meanwhile.
	const bool syncing = shown == NETWORK_PULL || shown == NETWORK_PUSH || shown == NETWORK_SWITCH || shown == NETWORK_COMMIT || shown == NETWORK_ABORT || shown == NETWORK_CONTINUE || shown == NETWORK_REVERT;
	// A merge, rebase, ... in progress: committing, pulling or switching would lose it (the
	// backend refuses too). The banner says what to do instead.
	const String in_operation = _in_operation() ? vformat("A %s is in progress: finish it or abort it first (see the banner above).", _operation_name()) : String();

	// The worker rewrites the repository during a pull or push; switching branches or bulk
	// changes meanwhile would race it.
	branch_select->set_disabled(syncing);
	more_menu->set_disabled(syncing);
	_update_commit_row(syncing);
	_update_sync_row(shown != NETWORK_NONE);

	// Stash what's staged: goes through git (git stash --staged; see GitRepository::stash).
	String stash_reason;
	if (!in_operation.is_empty()) {
		stash_reason = in_operation;
	} else if (git_missing) {
		stash_reason = "Stashing what's staged needs git, which isn't installed (or isn't on the PATH). Install it from git-scm.com, or use Stash All Changes in the menu.";
	} else if (staged_count == 0) {
		stash_reason = "Stash: stage the changes you want to set aside, then stash them here. Stash All Changes in the menu takes everything.";
	}
	staged_pane.stash->set_disabled(syncing || !stash_reason.is_empty());
	staged_pane.stash->set_tooltip_text(stash_reason.is_empty() ? vformat("Stash the %s: set %s aside, to restore later from Stashes.", plural(staged_count, "staged file", "staged files"), staged_count == 1 ? "it" : "them") : stash_reason);

	// Last, so the reason overrides the others: nothing else matters until it's finished.
	branch_select->set_tooltip_text(in_operation.is_empty() ? String("Current branch. Pick another to switch to it.") : in_operation);
	if (!in_operation.is_empty()) {
		branch_select->set_disabled(true);
		amend_check->set_disabled(true);
		amend_check->set_tooltip_text(in_operation);
		commit_button->set_disabled(true);
		commit_button->set_tooltip_text(in_operation);
		pull_button->set_disabled(true);
		pull_button->set_tooltip_text(in_operation);
	}
	_update_operation_banner(); // Its buttons follow the same busy state.
}

// Amend and Commit: enabled when there's something to commit, and the tooltip says what.
void GitDock::_update_commit_row(bool p_syncing) {
	const bool has_remotes = sync_status.get("has_remotes", false);
	const String branch = sync_status.get("branch", String());
	const bool has_message = !commit_message->get_text().strip_edges().is_empty();
	const NetworkOp shown = _shown_network_op();

	// Amend: only while the last commit is yours alone. Once pushed, rewriting it would leave
	// teammates with a commit that no longer exists here.
	if (amend_check->is_pressed() && (!has_commits || last_commit_pushed)) {
		amend_check->set_pressed(false); // E.g. just pushed. Puts the draft back.
	}
	const bool amending = amend_check->is_pressed();
	amend_check->set_disabled(p_syncing || !has_commits || last_commit_pushed);
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
		commit_button->set_disabled(p_syncing || !has_message);
		if (!has_message) {
			commit_button->set_tooltip_text("Write a commit message first.");
		} else if (staged_count == 0) {
			commit_button->set_tooltip_text(vformat("Replace commit %s with this message.", last_commit_id));
		} else {
			commit_button->set_tooltip_text(vformat("Replace commit %s with this message, adding %s.", last_commit_id, plural(staged_count, "staged file", "staged files")));
		}
	} else {
		commit_button->set_disabled(p_syncing || staged_count == 0 || !has_message);
		if (staged_count == 0) {
			commit_button->set_tooltip_text("Stage some changes first.");
		} else if (!has_message) {
			commit_button->set_tooltip_text("Write a commit message first.");
		} else {
			commit_button->set_tooltip_text(vformat("Commit %s to %s.%s", plural(staged_count, "staged file", "staged files"), branch,
					has_remotes ? String("\nCtrl+Shift+Enter in the message commits and pushes.") : String()));
		}
	}
	if (git_missing && shown != NETWORK_COMMIT && repo->commit_runs_git(amending)) {
		// Committing with libgit2 instead would silently skip the hook or the signature.
		const String reason = git_needs.get("commit", String());
		commit_button->set_disabled(true);
		commit_button->set_tooltip_text(vformat("Committing needs git here: %s. Git isn't installed (or isn't on the PATH); install it from git-scm.com.", reason.is_empty() ? String("this repository has a post-rewrite hook") : reason));
	}
}

// Fetch, Pull and Push. Pull and Push only appear when there's a remote to talk to, show how many
// commits they'd move, and their tooltips say exactly what they'll do.
void GitDock::_update_sync_row(bool p_busy) {
	const bool has_remotes = sync_status.get("has_remotes", false);
	const String upstream = sync_status.get("upstream", String());
	const String branch = sync_status.get("branch", String());
	const int ahead = sync_status.get("ahead", 0);
	const int behind = sync_status.get("behind", 0);
	const NetworkOp shown = _shown_network_op();

	sync_row->set_visible(has_remotes);
	const String fetch_needs_git = _needs_git(NETWORK_FETCH);
	fetch_button->set_disabled(p_busy || !fetch_needs_git.is_empty());
	fetch_button->set_text(shown == NETWORK_FETCH ? String("Fetching...") : String("Fetch"));
	fetch_button->set_tooltip_text(fetch_needs_git.is_empty() ? String("Fetch: check the remote for new commits, without changing your files.") : fetch_needs_git);

	// Pull: only when the branch tracks a remote branch. The count is as of the last fetch;
	// pulling always fetches first, so it stays enabled at 0.
	pull_button->set_visible(has_remotes && !upstream.is_empty());
	const String pull_needs_git = _needs_git(NETWORK_PULL);
	pull_button->set_disabled(p_busy || !pull_needs_git.is_empty());
	pull_button->set_text(shown == NETWORK_PULL ? String("Pulling...") : (behind > 0 ? vformat("Pull %d", behind) : String("Pull")));
	if (!pull_needs_git.is_empty()) {
		pull_button->set_tooltip_text(pull_needs_git);
	} else if (!pull_blockers.is_empty() && shown != NETWORK_PULL) {
		// Pull would refuse anyway (see GitRepository::pull): say why before it's pressed.
		pull_button->set_disabled(true);
		pull_button->set_tooltip_text(vformat("Pull is waiting: the new commits on %s change %s, and your uncommitted changes there can't be merged in (same lines, or staged, new, deleted or binary). Commit, stash or discard your changes to %s first (marked in Changes).",
				upstream, join_list(pull_blockers, 3), pull_blockers.size() == 1 ? "it" : "them"));
	} else {
		pull_button->set_tooltip_text(behind > 0
						? vformat("Pull: get %s from %s.", plural(behind, "new commit", "new commits"), upstream)
						: vformat("Pull from %s. Nothing new as of the last fetch.", upstream));
	}

	// Push: publishes the branch first time, then sends new commits. Disabled with nothing to send.
	push_button->set_visible(has_remotes && has_commits);
	if (upstream.is_empty()) {
		push_button->set_text(shown == NETWORK_PUSH ? String("Publishing...") : String("Publish"));
		push_button->set_tooltip_text(vformat("Publish: push %s to the remote and start tracking it.", branch));
		push_button->set_disabled(p_busy);
	} else {
		push_button->set_text(shown == NETWORK_PUSH ? String("Pushing...") : (ahead > 0 ? vformat("Push %d", ahead) : String("Push")));
		push_button->set_tooltip_text(ahead > 0 ? vformat("Push: send %s to %s.", plural(ahead, "commit", "commits"), upstream) : vformat("Nothing to push; %s has all your commits.", upstream));
		push_button->set_disabled(p_busy || ahead == 0);
	}
	const String push_needs_git = _needs_git(NETWORK_PUSH);
	if (!push_needs_git.is_empty()) {
		push_button->set_disabled(true);
		push_button->set_tooltip_text(push_needs_git);
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
		case MORE_STASH_ALL: {
			_stash(false);
		} break;
		case MORE_DISCARD_ALL: {
			if (!unstaged_paths.is_empty()) {
				_confirm_discard(unstaged_paths);
			}
		} break;
		case MORE_NEW_BRANCH: {
			branch_here = String();
			branch_dialog->set_title("New Branch");
			branch_dialog_label->set_text("Create a branch from the current commit and switch to it:");
			branch_name_edit->clear();
			branch_dialog->popup_centered(Vector2i(360, 0) * EditorInterface::get_singleton()->get_editor_scale());
			branch_name_edit->grab_focus();
		} break;
		case MORE_AUTO_FETCH: {
			const bool enable = !_is_auto_fetch_enabled();
			EditorInterface::get_singleton()->get_editor_settings()->set_setting(AUTO_FETCH_SETTING, enable);
			if (!enable && auto_fetch_failed && status_kind == STATUS_WARNING) {
				_set_status(STATUS_IDLE, String()); // Its failure message is moot now.
			}
			auto_fetch_failed = false;
			last_auto_fetch_attempt = 0;
		} break;
		case MORE_ADD_REMOTE: {
			_show_remote_dialog();
		} break;
		case MORE_OPEN_FOLDER: {
			OS::get_singleton()->shell_show_in_file_manager(repo->get_workdir(), true);
		} break;
		case MORE_CHANGE_MARKS: {
			EditorInterface::get_singleton()->get_editor_settings()->set_setting(CHANGE_MARKS_SETTING, !_is_change_marks_enabled());
			script_marks->set_enabled(_is_change_marks_enabled());
		} break;
		case MORE_FILESYSTEM_COLORS: {
			EditorInterface::get_singleton()->get_editor_settings()->set_setting(FILESYSTEM_COLORS_SETTING, !_is_filesystem_colors_enabled());
			refresh();
		} break;
		case MORE_BUILD_INFO: {
			const String url = String::utf8(build_url());
			if (!url.is_empty()) {
				OS::get_singleton()->shell_open(url);
			}
		} break;
	}
}

// --- Actions ----------------------------------------------------------------

// A confirmation dialog whose text wraps (added to the dock). A wrapping label measures its
// height at its minimum width, so the label gets one: without it the dialog opens as tall as one
// word per line (gotcha 28). An empty p_ok_text keeps "OK".
ConfirmationDialog *GitDock::_make_confirm(const String &p_title, const String &p_ok_text, const Callable &p_on_confirmed) {
	ConfirmationDialog *dialog = memnew(ConfirmationDialog);
	dialog->set_title(p_title);
	if (!p_ok_text.is_empty()) {
		dialog->set_ok_button_text(p_ok_text);
	}
	dialog->set_autowrap(true);
	dialog->get_label()->set_custom_minimum_size(Vector2(440 * EditorInterface::get_singleton()->get_editor_scale(), 0));
	dialog->connect("confirmed", p_on_confirmed);
	add_child(dialog);
	return dialog;
}

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
	_remember_open_scenes();
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
	_reload_changed_scenes();
}
