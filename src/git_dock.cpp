#include "git_dock.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/text_line.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2i.hpp>

namespace {

// File rows are a single cell (status letter, icon, name, folder, hover buttons) so they highlight as one unit.
// The letter leads, so the buttons end flush right and line up with the section header's button.
enum FileColumn {
	COLUMN_NAME,
	FILE_COLUMN_COUNT,
};

// Typographic minus, same width as "+". godot-cpp reads plain "..." literals as Latin-1, hence String::utf8.
// (A function, not a global: Strings can't be built before the extension is initialized.)
String minus() {
	return String::utf8("−");
}

String status_letter(const String &p_state) {
	if (p_state == "new") {
		return "A";
	}
	if (p_state == "modified") {
		return "M";
	}
	if (p_state == "deleted") {
		return "D";
	}
	if (p_state == "renamed") {
		return "R";
	}
	if (p_state == "typechange") {
		return "T";
	}
	if (p_state == "untracked") {
		return "U";
	}
	if (p_state == "conflicted") {
		return "!";
	}
	return "?";
}

String status_name(const String &p_state) {
	if (p_state == "new") {
		return "Added";
	}
	return p_state.capitalize();
}

String relative_time(int64_t p_unix_time) {
	const int64_t seconds = (int64_t)Time::get_singleton()->get_unix_time_from_system() - p_unix_time;
	if (seconds < 60) {
		return "now";
	}
	if (seconds < 3600) {
		return vformat("%dm", seconds / 60);
	}
	if (seconds < 86400) {
		return vformat("%dh", seconds / 3600);
	}
	if (seconds < 86400 * 30) {
		return vformat("%dd", seconds / 86400);
	}
	if (seconds < 86400 * 365) {
		return vformat("%dmo", seconds / (86400 * 30));
	}
	return vformat("%dy", seconds / (86400 * 365));
}

String plural(int p_count, const String &p_singular, const String &p_plural) {
	return vformat("%d %s", p_count, p_count == 1 ? p_singular : p_plural);
}

// Splits a command line the way Godot splits "exec_flags": spaces separate arguments,
// double quotes group them.
PackedStringArray split_command_line(const String &p_command_line) {
	PackedStringArray args;
	String current;
	bool quoted = false;
	for (int i = 0; i < p_command_line.length(); i++) {
		const char32_t c = p_command_line[i];
		if (c == '"') {
			quoted = !quoted;
		} else if (c == ' ' && !quoted) {
			if (!current.is_empty()) {
				args.push_back(current);
				current = String();
			}
		} else {
			current += String::chr(c);
		}
	}
	if (!current.is_empty()) {
		args.push_back(current);
	}
	return args;
}

// Path to a VS Code executable, or "" if it isn't installed in a usual place.
String find_vscode() {
	OS *os = OS::get_singleton();
	PackedStringArray candidates;
	if (os->get_name() == "Windows") {
		candidates.push_back(os->get_environment("LOCALAPPDATA").replace("\\", "/").path_join("Programs/Microsoft VS Code/Code.exe"));
		candidates.push_back(os->get_environment("ProgramFiles").replace("\\", "/").path_join("Microsoft VS Code/Code.exe"));
		// The `code` command on PATH lives in <install>/bin/code.cmd.
		for (const String &dir : os->get_environment("PATH").replace("\\", "/").split(";", false)) {
			if (FileAccess::file_exists(dir.path_join("code.cmd"))) {
				candidates.push_back(dir.path_join("../Code.exe").simplify_path());
			}
		}
	} else if (os->get_name() == "macOS") {
		candidates.push_back("/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code");
		candidates.push_back(os->get_environment("HOME").path_join("Applications/Visual Studio Code.app/Contents/Resources/app/bin/code"));
	} else {
		for (const String &dir : os->get_environment("PATH").split(":", false)) {
			candidates.push_back(dir.path_join("code"));
		}
		candidates.push_back("/snap/bin/code");
	}
	for (const String &candidate : candidates) {
		if (FileAccess::file_exists(candidate)) {
			return candidate;
		}
	}
	return String();
}

} // namespace

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

	// Branch picker, fetch, more.
	HBoxContainer *toolbar = memnew(HBoxContainer);
	repo_vb->add_child(toolbar);

	branch_select = memnew(OptionButton);
	branch_select->set_h_size_flags(SIZE_EXPAND_FILL);
	branch_select->set_clip_text(true);
	branch_select->set_fit_to_longest_item(false);
	branch_select->set_tooltip_text("Current branch. Pick another to switch to it.");
	branch_select->connect("item_selected", callable_mp(this, &GitDock::_on_branch_selected));
	toolbar->add_child(branch_select);

	fetch_button = memnew(Button);
	fetch_button->set_theme_type_variation("FlatButton");
	fetch_button->set_tooltip_text("Fetch: check the remote for new commits.");
	fetch_button->connect("pressed", callable_mp(this, &GitDock::_start_network).bind(NETWORK_FETCH));
	toolbar->add_child(fetch_button);

	more_menu = memnew(MenuButton);
	more_menu->set_flat(true);
	more_menu->set_tooltip_text("More actions");
	more_menu->get_popup()->connect("about_to_popup", callable_mp(this, &GitDock::_build_more_menu));
	more_menu->get_popup()->connect("id_pressed", callable_mp(this, &GitDock::_on_more_menu_id));
	toolbar->add_child(more_menu);

	// Commit message, then all the actions in one row: Commit (wide, the main thing you do),
	// then Pull / Push, labeled with how many commits each would move.
	commit_message = memnew(TextEdit);
	commit_message->set_placeholder("Message (Ctrl+Enter to commit)");
	commit_message->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	commit_message->set_custom_minimum_size(Vector2(0, 64 * scale));
	commit_message->connect("text_changed", callable_mp(this, &GitDock::_update_actions));
	commit_message->connect("gui_input", callable_mp(this, &GitDock::_on_commit_message_input));
	repo_vb->add_child(commit_message);

	HBoxContainer *action_hb = memnew(HBoxContainer);
	repo_vb->add_child(action_hb);

	commit_button = memnew(Button);
	commit_button->set_text("Commit");
	commit_button->set_h_size_flags(SIZE_EXPAND_FILL);
	commit_button->connect("pressed", callable_mp(this, &GitDock::_commit));
	action_hb->add_child(commit_button);

	pull_button = memnew(Button);
	pull_button->connect("pressed", callable_mp(this, &GitDock::_start_network).bind(NETWORK_PULL));
	action_hb->add_child(pull_button);

	push_button = memnew(Button);
	push_button->connect("pressed", callable_mp(this, &GitDock::_start_network).bind(NETWORK_PUSH));
	action_hb->add_child(push_button);

	// Staged changes, changes, history: collapsible sections, each exactly as tall as its
	// content, in one shared scroll area.
	ScrollContainer *scroll = memnew(ScrollContainer);
	scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	repo_vb->add_child(scroll);

	VBoxContainer *panes = memnew(VBoxContainer);
	panes->set_h_size_flags(SIZE_EXPAND_FILL);
	scroll->add_child(panes);

	_make_file_pane(staged_pane, panes, "Staged Changes", true);
	_make_file_pane(changes_pane, panes, "Changes", false);

	history_pane = memnew(FoldableContainer);
	history_pane->set_title("History");
	panes->add_child(history_pane);

	history_tree = memnew(Tree);
	history_tree->set_hide_root(true);
	history_tree->set_select_mode(Tree::SELECT_ROW);
	history_tree->set_allow_rmb_select(true);
	history_tree->set_v_scroll_enabled(false); // Grow to fit; the outer ScrollContainer scrolls.
	history_tree->set_h_scroll_enabled(false);
	history_tree->set_columns(2);
	history_tree->set_column_expand(0, true);
	history_tree->set_column_clip_content(0, true);
	history_tree->set_column_expand(1, false);
	history_tree->connect("item_mouse_selected", callable_mp(this, &GitDock::_on_tree_mouse_selected).bind(history_tree));
	history_empty = _make_body(history_pane, history_tree);

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
}

void GitDock::_make_file_pane(FilePane &r_pane, Control *p_parent, const String &p_title, bool p_staged) {
	r_pane.staged = p_staged;

	r_pane.container = memnew(FoldableContainer);
	r_pane.container->set_title(p_title);
	p_parent->add_child(r_pane.container);

	// Header: file count, line totals and the "all" actions. The actions sit in the same order
	// and exact positions as the row buttons (discard, then stage/unstage); see _align_header_buttons.
	r_pane.count = memnew(Label);
	r_pane.added = memnew(Label);
	r_pane.removed = memnew(Label);

	r_pane.buttons_margin = memnew(MarginContainer);
	r_pane.buttons = memnew(HBoxContainer);
	r_pane.buttons_margin->add_child(r_pane.buttons);
	if (!p_staged) {
		r_pane.discard = memnew(Button);
		r_pane.discard->set_tooltip_text("Discard all changes");
		r_pane.discard->connect("pressed", callable_mp(this, &GitDock::_on_more_menu_id).bind(MORE_DISCARD_ALL));
		r_pane.buttons->add_child(r_pane.discard);
	}
	r_pane.action = memnew(Button);
	r_pane.action->set_tooltip_text(p_staged ? "Unstage all" : "Stage all");
	r_pane.action->connect("pressed", callable_mp(this, &GitDock::_on_more_menu_id).bind(p_staged ? MORE_UNSTAGE_ALL : MORE_STAGE_ALL));
	r_pane.buttons->add_child(r_pane.action);

	for (Control *control : { (Control *)r_pane.count, (Control *)r_pane.added, (Control *)r_pane.removed, (Control *)r_pane.buttons_margin }) {
		control->set_v_size_flags(SIZE_SHRINK_CENTER);
		r_pane.container->add_title_bar_control(control);
	}
	r_pane.container->connect("sort_children", callable_mp(this, &GitDock::_queue_align_header_buttons));

	Tree *tree = memnew(Tree);
	tree->set_hide_root(true);
	tree->set_select_mode(Tree::SELECT_MULTI);
	tree->set_allow_rmb_select(true);
	tree->set_columns(FILE_COLUMN_COUNT);
	tree->set_v_scroll_enabled(false); // Grow to fit; the outer ScrollContainer scrolls.
	tree->set_h_scroll_enabled(false);
	tree->set_column_expand(COLUMN_NAME, true);
	tree->set_column_clip_content(COLUMN_NAME, true);
	tree->add_theme_constant_override("item_margin", 0); // Flat list: no hierarchy indent.
	tree->connect("button_clicked", callable_mp(this, &GitDock::_on_tree_button_clicked));
	tree->connect("item_activated", callable_mp(this, &GitDock::_on_file_activated).bind(tree));
	tree->connect("item_mouse_selected", callable_mp(this, &GitDock::_on_tree_mouse_selected).bind(tree));
	tree->connect("gui_input", callable_mp(this, &GitDock::_on_tree_gui_input).bind(tree));
	tree->connect("mouse_exited", callable_mp(this, &GitDock::_on_tree_mouse_exited).bind(tree));
	r_pane.tree = tree;
	r_pane.empty_label = _make_body(r_pane.container, tree);
}

// A section's content: the tree, and a plain label shown instead when the tree is empty.
Label *GitDock::_make_body(Control *p_section, Tree *p_tree) {
	VBoxContainer *body = memnew(VBoxContainer);
	body->add_theme_constant_override("separation", 0);
	p_section->add_child(body);
	body->add_child(p_tree);

	MarginContainer *empty_margin = memnew(MarginContainer);
	empty_margin->hide();
	body->add_child(empty_margin);
	Label *label = memnew(Label);
	empty_margin->add_child(label);
	return label;
}

void GitDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			_update_icons();
		} break;

		case NOTIFICATION_READY: {
			EditorFileSystem *fs = EditorInterface::get_singleton()->get_resource_filesystem();
			fs->connect("filesystem_changed", callable_mp(this, &GitDock::refresh));
			refresh();
		} break;

		case NOTIFICATION_APPLICATION_FOCUS_IN: {
			// Pick up changes made outside the editor (terminal, other git tools).
			if (is_node_ready()) {
				refresh();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			// Don't let a login window nobody finishes keep the editor from closing.
			GitRepository::cancel_pending_login();
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

	sync_status = repo->get_sync_status();
	_fill_branches();

	const Array status = repo->get_status();
	_fill_file_pane(staged_pane, status, repo->get_line_stats(true));
	_fill_file_pane(changes_pane, status, repo->get_line_stats(false));

	staged_count = staged_pane.file_count;
	set_title(status.is_empty() ? String("Git") : vformat("Git (%d)", status.size()));

	_fill_history();
	_update_actions();
	_queue_align_header_buttons();
}

void GitDock::_fill_file_pane(FilePane &p_pane, const Array &p_status, const Dictionary &p_stats) {
	Tree *tree = p_pane.tree;
	tree->clear();
	p_pane.hovered_item = 0;
	TreeItem *root = tree->create_item();

	const String key = p_pane.staged ? "index" : "worktree";
	const Color dim = _dim_color();
	const Callable draw_row = callable_mp(this, &GitDock::_draw_file_row);

	int total_added = 0, total_removed = 0, files = 0;
	if (!p_pane.staged) {
		unstaged_paths.clear();
	}

	for (int i = 0; i < p_status.size(); i++) {
		const Dictionary entry = p_status[i];
		const String state = entry[key];
		if (state.is_empty()) {
			continue;
		}
		const String path = entry["path"];
		files++;
		if (!p_pane.staged) {
			unstaged_paths.push_back(path);
		}

		// Line counts only show as totals in the section header.
		const Vector2i stats = p_stats.get(path, Vector2i());
		if (stats.x > 0) {
			total_added += stats.x;
		}
		if (stats.y > 0) {
			total_removed += stats.y;
		}

		TreeItem *item = tree->create_item(root);
		item->set_metadata(COLUMN_NAME, path);
		item->set_meta("git_state", state);
		item->set_meta("git_icon", _file_icon(path));

		// The whole row is one cell that _draw_file_row paints (status letter, icon, name, folder),
		// so hover and selection highlight it as one unit. The name stays as the cell's text, made
		// invisible, so row height, type-to-search and accessibility still work.
		item->set_cell_mode(COLUMN_NAME, TreeItem::CELL_MODE_CUSTOM);
		item->set_custom_draw_callback(COLUMN_NAME, draw_row);
		item->set_text(COLUMN_NAME, path.get_file());
		item->set_custom_color(COLUMN_NAME, Color(0, 0, 0, 0));
		item->set_tooltip_text(COLUMN_NAME, vformat("%s\n%s", path, status_name(state)));
	}

	tree->set_visible(files > 0);
	p_pane.empty_label->get_parent_control()->set_visible(files == 0);
	p_pane.empty_label->set_text(p_pane.staged ? "Nothing staged." : "No changes.");

	p_pane.file_count = files;

	// Header: "27  +1204 −35  [⊖]".
	p_pane.count->set_text(files > 0 ? itos(files) : String());
	p_pane.added->set_text(files > 0 ? vformat("+%d", total_added) : String());
	p_pane.removed->set_text(files > 0 ? vformat("%s%d", minus(), total_removed) : String());
	p_pane.added->set_tooltip_text(plural(total_added, "line added", "lines added"));
	p_pane.removed->set_tooltip_text(plural(total_removed, "line removed", "lines removed"));
	p_pane.action->set_disabled(files == 0);
	if (p_pane.discard) {
		p_pane.discard->set_disabled(files == 0);
	}
}

void GitDock::_queue_align_header_buttons() {
	if (!align_queued) {
		align_queued = true;
		callable_mp(this, &GitDock::_align_header_buttons).call_deferred();
	}
}

// Shifts each header's "all" buttons so they end exactly where the Tree draws the rows' buttons
// (the right edge of the file column), whatever the theme margins and editor scale are.
void GitDock::_align_header_buttons() {
	align_queued = false;
	for (FilePane *pane : { &staged_pane, &changes_pane }) {
		if (!pane->container->is_visible_in_tree()) {
			continue;
		}
		Tree *tree = pane->tree;
		TreeItem *first = tree->get_root() ? tree->get_root()->get_first_child() : nullptr;
		if (!first || !tree->is_visible()) {
			continue; // Nothing to line up with while the section is empty.
		}
		const float rows_right = tree->get_global_position().x + tree->get_item_area_rect(first, COLUMN_NAME).get_end().x;
		const float buttons_right = pane->buttons->get_global_position().x + pane->buttons->get_size().x;
		const int margin = pane->buttons_margin->get_theme_constant("margin_right");
		const int aligned = MAX(0, (int)Math::round(margin + buttons_right - rows_right));
		if (aligned != margin) {
			pane->buttons_margin->add_theme_constant_override("margin_right", aligned);
		}
	}
}

// Paints a file row: status letter, file icon, name, and the folder dimmed after the name
// (like VS Code), each trimmed with an ellipsis when space runs out. p_rect is the cell's
// content area, which the Tree has already shrunk to leave room for the hover buttons.
void GitDock::_draw_file_row(TreeItem *p_item, const Rect2 &p_rect) {
	Tree *tree = p_item->get_tree();
	const FilePane *pane = _pane_for_tree(tree);
	if (!pane) {
		return;
	}
	const Ref<Font> font = tree->get_theme_font("font");
	const int font_size = tree->get_theme_font_size("font_size");
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	// The Tree's layer for custom drawing: above the hover/selection background, clipped to the rows.
	const RID canvas = tree->get_custom_drawing_canvas_item();
	const String path = p_item->get_metadata(COLUMN_NAME);
	const String state = p_item->get_meta("git_state", String());
	const float right = p_rect.get_end().x;

	// Draws one piece of text starting at p_x, vertically centered; returns where it ends.
	auto draw_text = [&](const String &p_text, float p_x, const Color &p_color) -> float {
		if (right - p_x < font_size) {
			return right; // Not enough room to show anything useful.
		}
		Ref<TextLine> line;
		line.instantiate();
		line->add_string(p_text, font, font_size);
		line->set_width(right - p_x);
		line->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		line->draw(canvas, Vector2(p_x, p_rect.position.y + (p_rect.size.y - line->get_size().y) / 2), p_color);
		return p_x + MIN(line->get_size().x, right - p_x);
	};

	// Status letter in a fixed-width slot, so icons and names line up down the list.
	float x = p_rect.position.x;
	const float letter_width = font->get_string_size("M", HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
	const String letter = status_letter(state);
	const float letter_x = x + (letter_width - font->get_string_size(letter, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x) / 2;
	draw_text(letter, letter_x, _status_color(state));
	x += letter_width + 6 * scale;

	const bool deleted = state == "deleted";
	const Ref<Texture2D> icon = p_item->get_meta("git_icon", Variant());
	if (icon.is_valid()) {
		icon->draw(canvas, Vector2(x, p_rect.position.y + (p_rect.size.y - icon->get_height()) / 2), Color(1, 1, 1, deleted ? 0.5 : 1));
		x += icon->get_width() + tree->get_theme_constant("icon_h_separation");
	}

	// Name in the Tree's own text colors, so selection and hover look native.
	Color name_color = tree->get_theme_color("font_color");
	if (p_item->is_selected(COLUMN_NAME)) {
		name_color = tree->get_theme_color("font_selected_color");
	} else if (p_item->get_instance_id() == pane->hovered_item) {
		name_color = tree->get_theme_color("font_hovered_color");
	}
	if (deleted) {
		name_color.a *= 0.5;
	}
	x = draw_text(path.get_file(), x, name_color);

	const String folder = path.get_base_dir();
	if (!folder.is_empty()) {
		draw_text(folder, x + 6 * scale, _dim_color());
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
	// A pull or push rewrites the repository from the worker thread; don't commit meanwhile.
	const bool syncing = network_op == NETWORK_PULL || network_op == NETWORK_PUSH;
	const bool busy = network_op != NETWORK_NONE;

	fetch_button->set_visible(has_remotes);
	fetch_button->set_disabled(busy);
	fetch_button->set_tooltip_text(network_op == NETWORK_FETCH ? String("Fetching...") : String("Fetch: check the remote for new commits, without changing your files."));

	commit_button->set_disabled(syncing || staged_count == 0 || !has_message);
	if (staged_count == 0) {
		commit_button->set_tooltip_text("Stage some changes first.");
	} else if (!has_message) {
		commit_button->set_tooltip_text("Write a commit message first.");
	} else {
		commit_button->set_tooltip_text(vformat("Commit %s to %s.", plural(staged_count, "staged file", "staged files"), branch));
	}

	// Pull: only when the branch tracks a remote branch. The count is as of the last fetch;
	// pulling always fetches first, so it stays enabled at 0.
	pull_button->set_visible(has_remotes && !upstream.is_empty());
	pull_button->set_disabled(busy);
	pull_button->set_text(network_op == NETWORK_PULL ? String("Pulling...") : (behind > 0 ? vformat("Pull %d", behind) : String("Pull")));
	pull_button->set_tooltip_text(behind > 0
					? vformat("Pull: get %s from %s.", plural(behind, "new commit", "new commits"), upstream)
					: vformat("Pull from %s. Nothing new as of the last fetch.", upstream));

	// Push: publishes the branch first time, then sends new commits. Disabled with nothing to send.
	push_button->set_visible(has_remotes && has_commits);
	if (upstream.is_empty()) {
		push_button->set_text(network_op == NETWORK_PUSH ? String("Publishing...") : String("Publish"));
		push_button->set_tooltip_text(vformat("Publish: push %s to the remote and start tracking it.", branch));
		push_button->set_disabled(busy);
	} else {
		push_button->set_text(network_op == NETWORK_PUSH ? String("Pushing...") : (ahead > 0 ? vformat("Push %d", ahead) : String("Push")));
		push_button->set_tooltip_text(ahead > 0 ? vformat("Push: send %s to %s.", plural(ahead, "commit", "commits"), upstream) : vformat("Nothing to push; %s has all your commits.", upstream));
		push_button->set_disabled(busy || ahead == 0);
	}
}

void GitDock::_fill_history() {
	history_tree->clear();
	TreeItem *root = history_tree->create_item();

	const Array commits = repo->get_commits(50);
	const Color dim = _dim_color();
	has_commits = !commits.is_empty();

	history_tree->set_visible(!commits.is_empty());
	history_empty->get_parent_control()->set_visible(commits.is_empty());
	history_empty->set_text("No commits yet.");
	if (commits.is_empty()) {
		return;
	}

	const int64_t bias_minutes = Dictionary(Time::get_singleton()->get_time_zone_from_system()).get("bias", 0);
	const Ref<Texture2D> icon = get_theme_icon("VCSCommit", "EditorIcons");
	const Color accent = get_theme_color("accent_color", "Editor");

	for (int i = 0; i < commits.size(); i++) {
		const Dictionary commit = commits[i];
		const int64_t time = commit["time"];
		const bool unpushed = commit["unpushed"];
		const String date = Time::get_singleton()->get_datetime_string_from_unix_time(time + bias_minutes * 60, true);

		TreeItem *item = history_tree->create_item(root);
		item->set_metadata(0, commit);
		item->set_icon(0, icon);
		if (unpushed) {
			item->set_icon_modulate(0, accent);
		}
		item->set_text(0, commit["summary"]);
		item->set_tooltip_text(0, vformat(String::utf8("%s\n\n%s · %s · %s%s"), commit["message"], commit["id"], commit["author"], date, unpushed ? "\nNot pushed yet" : ""));
		const String when = relative_time(time);
		item->set_text(1, when);
		item->set_text_overrun_behavior(1, TextServer::OVERRUN_NO_TRIMMING);
		item->set_custom_color(1, dim);
		item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
		item->set_tooltip_text(1, date);
	}
}

void GitDock::_report(Error p_err, const String &p_action) {
	if (p_err == OK) {
		return;
	}
	String message = GitRepository::get_last_error();
	if (message.is_empty()) {
		message = UtilityFunctions::error_string(p_err);
	}
	EditorInterface::get_singleton()->get_editor_toaster()->push_toast(vformat("Git: %s failed. %s", p_action, message), EditorToaster::SEVERITY_ERROR);
}

// --- Tree interaction -------------------------------------------------------

GitDock::FilePane *GitDock::_pane_for_tree(Object *p_tree) {
	if (p_tree == staged_pane.tree) {
		return &staged_pane;
	}
	if (p_tree == changes_pane.tree) {
		return &changes_pane;
	}
	return nullptr;
}

PackedStringArray GitDock::_selected_paths(Tree *p_tree) const {
	PackedStringArray paths;
	for (TreeItem *item = p_tree->get_next_selected(nullptr); item; item = p_tree->get_next_selected(item)) {
		const String path = item->get_metadata(COLUMN_NAME);
		if (!path.is_empty()) {
			paths.push_back(path);
		}
	}
	return paths;
}

// Row buttons only show on the row under the mouse, to keep the list calm.
void GitDock::_set_hovered(FilePane &p_pane, TreeItem *p_item) {
	TreeItem *previous = Object::cast_to<TreeItem>(ObjectDB::get_instance(p_pane.hovered_item));
	if (previous == p_item) {
		return;
	}
	if (previous) {
		previous->clear_buttons();
	}
	p_pane.hovered_item = 0;
	if (!p_item || p_item->get_metadata(COLUMN_NAME).get_type() != Variant::STRING) {
		return;
	}

	if (p_pane.staged) {
		p_item->add_button(COLUMN_NAME, get_theme_icon("ZoomLess", "EditorIcons"), BUTTON_UNSTAGE, false, "Unstage");
	} else {
		p_item->add_button(COLUMN_NAME, get_theme_icon("UndoRedo", "EditorIcons"), BUTTON_DISCARD, false, "Discard changes");
		p_item->add_button(COLUMN_NAME, get_theme_icon("ZoomMore", "EditorIcons"), BUTTON_STAGE, false, "Stage");
	}
	p_pane.hovered_item = p_item->get_instance_id();
}

void GitDock::_on_tree_gui_input(const Ref<InputEvent> &p_event, Object *p_tree) {
	Ref<InputEventMouseMotion> motion = p_event;
	FilePane *pane = _pane_for_tree(p_tree);
	if (motion.is_valid() && pane) {
		_set_hovered(*pane, pane->tree->get_item_at_position(motion->get_position()));
	}
}

void GitDock::_on_tree_mouse_exited(Object *p_tree) {
	FilePane *pane = _pane_for_tree(p_tree);
	if (pane) {
		_set_hovered(*pane, nullptr);
	}
}

void GitDock::_on_tree_button_clicked(TreeItem *p_item, int p_column, int p_id, int p_mouse_button) {
	const String path = p_item->get_metadata(COLUMN_NAME);
	PackedStringArray paths;
	paths.push_back(path);

	switch (p_id) {
		case BUTTON_STAGE: {
			_stage_paths(paths, true);
		} break;
		case BUTTON_UNSTAGE: {
			_stage_paths(paths, false);
		} break;
		case BUTTON_DISCARD: {
			_confirm_discard(paths);
		} break;
	}
}

void GitDock::_on_file_activated(Object *p_tree) {
	Tree *tree = Object::cast_to<Tree>(p_tree);
	TreeItem *item = tree ? tree->get_selected() : nullptr;
	if (item) {
		_open_path(item->get_metadata(COLUMN_NAME));
	}
}

void GitDock::_on_tree_mouse_selected(const Vector2 &p_position, int p_mouse_button, Object *p_tree) {
	if (p_mouse_button != MOUSE_BUTTON_RIGHT) {
		return;
	}
	Tree *tree = Object::cast_to<Tree>(p_tree);
	context_tree = tree;
	context_menu->clear();

	if (tree == history_tree) {
		if (!tree->get_selected() || tree->get_selected()->get_metadata(0).get_type() != Variant::DICTIONARY) {
			return;
		}
		context_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), "Copy Commit Hash", MENU_COPY_HASH);
		context_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), "Copy Commit Message", MENU_COPY_MESSAGE);
	} else {
		const FilePane *pane = _pane_for_tree(tree);
		const PackedStringArray paths = _selected_paths(tree);
		if (!pane || paths.is_empty()) {
			return;
		}
		const bool single = paths.size() == 1;
		const String count = single ? String() : vformat(" (%d)", paths.size());

		if (single) {
			context_menu->add_icon_item(get_theme_icon("Load", "EditorIcons"), "Open", MENU_OPEN);
			context_menu->add_separator();
		}
		if (pane->staged) {
			context_menu->add_icon_item(get_theme_icon("ZoomLess", "EditorIcons"), "Unstage" + count, MENU_UNSTAGE);
		} else {
			context_menu->add_icon_item(get_theme_icon("ZoomMore", "EditorIcons"), "Stage" + count, MENU_STAGE);
			context_menu->add_icon_item(get_theme_icon("UndoRedo", "EditorIcons"), "Discard Changes..." + count, MENU_DISCARD);
		}
		if (single) {
			context_menu->add_separator();
			if (_to_res_path(paths[0]).begins_with("res://")) {
				context_menu->add_icon_item(get_theme_icon("Filesystem", "EditorIcons"), "Show in FileSystem", MENU_SHOW_IN_FILESYSTEM);
			}
			context_menu->add_icon_item(get_theme_icon("Folder", "EditorIcons"), "Show in File Manager", MENU_SHOW_IN_FILE_MANAGER);
		}
		context_menu->add_separator();
		context_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), single ? "Copy Path" : "Copy Paths", MENU_COPY_PATH);
		context_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), single ? "Copy Relative Path" : "Copy Relative Paths", MENU_COPY_RELATIVE_PATH);
	}

	context_menu->set_position(Vector2i(tree->get_screen_position() + p_position));
	context_menu->reset_size();
	context_menu->popup();
}

void GitDock::_on_context_menu_id(int p_id) {
	if (!context_tree) {
		return;
	}
	const PackedStringArray paths = context_tree == history_tree ? PackedStringArray() : _selected_paths(context_tree);

	switch (p_id) {
		case MENU_OPEN: {
			if (!paths.is_empty()) {
				_open_path(paths[0]);
			}
		} break;
		case MENU_STAGE: {
			_stage_paths(paths, true);
		} break;
		case MENU_UNSTAGE: {
			_stage_paths(paths, false);
		} break;
		case MENU_DISCARD: {
			_confirm_discard(paths);
		} break;
		case MENU_SHOW_IN_FILESYSTEM: {
			EditorInterface::get_singleton()->select_file(_to_res_path(paths[0]));
		} break;
		case MENU_SHOW_IN_FILE_MANAGER: {
			OS::get_singleton()->shell_show_in_file_manager(repo->get_workdir().path_join(paths[0]), false);
		} break;
		case MENU_COPY_PATH:
		case MENU_COPY_RELATIVE_PATH: {
			PackedStringArray lines;
			for (const String &path : paths) {
				lines.push_back(p_id == MENU_COPY_PATH ? repo->get_workdir().path_join(path) : path);
			}
			DisplayServer::get_singleton()->clipboard_set(String("\n").join(lines));
		} break;
		case MENU_COPY_HASH:
		case MENU_COPY_MESSAGE: {
			const Dictionary commit = history_tree->get_selected()->get_metadata(0);
			DisplayServer::get_singleton()->clipboard_set(commit[p_id == MENU_COPY_HASH ? "hash" : "message"]);
		} break;
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
		case MORE_OPEN_FOLDER: {
			OS::get_singleton()->shell_show_in_file_manager(repo->get_workdir(), true);
		} break;
	}
}

// --- Actions ----------------------------------------------------------------

// Opens a file in Godot when Godot can edit it (scenes, scripts, other resources; scripts follow
// Godot's own "use external editor" setting), and otherwise in the user's code editor.
void GitDock::_open_path(const String &p_path) {
	if (p_path.is_empty()) {
		return;
	}
	const String absolute = repo->get_workdir().path_join(p_path);
	const String local = _to_res_path(p_path);
	EditorToaster *toaster = EditorInterface::get_singleton()->get_editor_toaster();

	if (!FileAccess::file_exists(absolute)) {
		toaster->push_toast(vformat("Git: \"%s\" was deleted, so there's nothing to open.", p_path.get_file()), EditorToaster::SEVERITY_INFO);
		return;
	}

	if (local.begins_with("res://")) {
		EditorInterface *editor = EditorInterface::get_singleton();
		const String type = editor->get_resource_filesystem()->get_file_type(local);
		if (type == "PackedScene") {
			editor->open_scene_from_path(local);
			return;
		}
		if (!type.is_empty() && ResourceLoader::get_singleton()->exists(local)) {
			Ref<Resource> res = ResourceLoader::get_singleton()->load(local);
			Ref<Script> script = res;
			if (script.is_valid()) {
				editor->edit_script(script);
				return;
			}
			if (res.is_valid()) {
				editor->edit_resource(res);
				return;
			}
		}
	}

	// C++ sources, README, project.godot, .uid files, ...: Godot's script editor can't be asked
	// to open those, so they go to the code editor.
	if (!_open_in_code_editor(absolute)) {
		toaster->push_toast(vformat("Git: No code editor found to open \"%s\". Set one in Editor Settings > Text Editor > External, or install VS Code.", p_path.get_file()), EditorToaster::SEVERITY_WARNING);
	}
}

// Opens a file in the external editor configured in Godot (Editor Settings > Text Editor >
// External), or in VS Code if none is configured. Returns false if neither is available.
bool GitDock::_open_in_code_editor(const String &p_absolute_path) {
	Ref<EditorSettings> settings = EditorInterface::get_singleton()->get_editor_settings();
	String program = settings->get_setting("text_editor/external/exec_path");
	PackedStringArray args;

	if (!program.is_empty()) {
		// Same placeholders Godot itself fills in when it opens scripts externally.
		bool has_file = false;
		for (String arg : split_command_line(settings->get_setting("text_editor/external/exec_flags"))) {
			has_file = has_file || arg.contains("{file}");
			arg = arg.replace("{project}", ProjectSettings::get_singleton()->globalize_path("res://"));
			arg = arg.replace("{file}", p_absolute_path).replace("{line}", "1").replace("{col}", "1");
			args.push_back(arg);
		}
		if (!has_file) {
			args.push_back(p_absolute_path);
		}
	} else {
		program = find_vscode();
		if (program.is_empty()) {
			return false;
		}
		// Passing the repository folder makes VS Code reuse (or open) the window for this repo.
		args.push_back(repo->get_workdir());
		args.push_back(p_absolute_path);
	}
	return OS::get_singleton()->create_process(program, args) != -1;
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
	const Error err = repo->checkout_branch(target);
	_report(err, "Switch branch");
	if (err == OK) {
		EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	}
	refresh();
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

void GitDock::_commit() {
	const String message = commit_message->get_text().strip_edges();
	if (message.is_empty() || staged_count == 0) {
		return;
	}
	const Error err = repo->commit(message);
	_report(err, "Commit");
	if (err == OK) {
		commit_message->clear();
	}
	refresh();
}

// --- Network (fetch / pull / push run on a worker thread) ---------------------

void GitDock::_start_network(int p_op) {
	if (network_op != NETWORK_NONE || !repo->is_open()) {
		return;
	}
	network_op = (NetworkOp)p_op;
	_update_actions();

	network_thread.instantiate();
	network_thread->start(callable_mp(this, &GitDock::_network_worker).bind(p_op, repo->get_workdir()));
}

// Runs on the worker thread with its own repository handle; reports back via call_deferred.
void GitDock::_network_worker(int p_op, const String &p_workdir) {
	Ref<GitRepository> worker_repo;
	worker_repo.instantiate();
	Error err = worker_repo->open(p_workdir);
	if (err == OK) {
		switch (p_op) {
			case NETWORK_FETCH:
				err = worker_repo->fetch();
				break;
			case NETWORK_PULL:
				err = worker_repo->pull();
				break;
			case NETWORK_PUSH:
				err = worker_repo->push();
				break;
		}
	}
	const String message = err == OK ? String() : GitRepository::get_last_error();
	const String upstream = err == OK ? String(worker_repo->get_sync_status().get("upstream", String())) : String();
	callable_mp(this, &GitDock::_network_done).call_deferred(p_op, (int)err, message, upstream, worker_repo->get_notice());
}

void GitDock::_network_done(int p_op, int p_err, const String &p_message, const String &p_upstream, const String &p_notice) {
	_finish_network_thread();
	network_op = NETWORK_NONE;

	static const char *names[] = { "", "Fetch", "Pull", "Push" };
	EditorToaster *toaster = EditorInterface::get_singleton()->get_editor_toaster();
	if (p_err != OK) {
		const String message = p_message.is_empty() ? UtilityFunctions::error_string(p_err) : p_message;
		toaster->push_toast(vformat("Git: %s failed. %s", names[p_op], message), EditorToaster::SEVERITY_ERROR);
	} else if (p_op == NETWORK_PULL) {
		toaster->push_toast(vformat("Git: Pulled from %s.", p_upstream), EditorToaster::SEVERITY_INFO);
	} else if (p_op == NETWORK_PUSH) {
		toaster->push_toast(vformat("Git: Pushed to %s.", p_upstream), EditorToaster::SEVERITY_INFO);
	}
	if (!p_notice.is_empty()) {
		toaster->push_toast("Git: " + p_notice, EditorToaster::SEVERITY_WARNING);
	}
	// A pull can change files on disk.
	if (p_op == NETWORK_PULL) {
		EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	}
	refresh();
}

void GitDock::_finish_network_thread() {
	if (network_thread.is_valid() && network_thread->is_started()) {
		network_thread->wait_to_finish();
	}
	network_thread.unref();
}
