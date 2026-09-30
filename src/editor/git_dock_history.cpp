// History: 50 commits at a time; expanding a commit shows its details and files (built deferred,
// never inside the Tree's mouse handling: gotcha 41).

#include "editor/git_dock.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/margin_container.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/timer.hpp>
#include <godot_cpp/core/math.hpp>

#include "editor/git_diff_dock.h"
#include "editor/git_dock_util.h"
#include "editor/ui_text.h"

using namespace godot_git;

// History: the latest commits, each expandable to its details and changed files (loaded when
// first expanded). Rebuilt only when the commits changed, so expanded commits, the selection and
// the scroll position survive the refresh that every save triggers.
void GitDock::_fill_history() {
	Array commits = repo->get_commits(history_limit + 1, history_path, history_query);
	const bool more = commits.size() > history_limit;
	if (more) {
		commits.resize(history_limit);
	}
	// For Amend and Undo Last Commit, whatever History shows. "unpushed" means on no
	// remote-tracking branch; without remotes it's never set.
	const Dictionary last = repo->get_commit("HEAD");
	has_commits = !last.is_empty();
	last_commit_id = last.get("id", String());
	last_commit_message = last.get("message", String());
	last_commit_pushed = has_commits && bool(sync_status.get("has_remotes", false)) && !bool(last.get("unpushed", false));

	TreeItem *root = history_tree->get_root();
	if (root && commits == history_shown && more == history_more) {
		// Same commits: only the ages ("5m") move on.
		for (TreeItem *item = root->get_first_child(); item; item = item->get_next()) {
			if (row_kind(item) == "commit") {
				item->set_text(1, relative_time((int64_t)Dictionary(item->get_metadata(0))["time"]));
			}
		}
		return;
	}
	history_shown = commits;
	history_more = more;

	history_tree->clear();
	root = history_tree->create_item();
	history_tree->set_visible(!commits.is_empty());
	history_empty->get_parent_control()->set_visible(commits.is_empty());
	if (!history_path.is_empty()) {
		history_empty->set_text(history_query.is_empty() ? vformat("No commits changed %s.", history_path.get_file()) : vformat("No commits that changed %s match \"%s\".", history_path.get_file(), history_query));
	} else if (!history_query.is_empty()) {
		history_empty->set_text(vformat("No commits match \"%s\".", history_query));
	} else {
		history_empty->set_text("No commits yet.");
	}
	if (commits.is_empty()) {
		return;
	}

	const Ref<Texture2D> icon = get_theme_icon("VCSCommit", "EditorIcons");
	const Color accent = get_theme_color("accent_color", "Editor");
	const Color dim = _dim_color();

	for (int i = 0; i < commits.size(); i++) {
		const Dictionary commit = commits[i];
		const int64_t time = commit["time"];
		const bool unpushed = commit["unpushed"];
		const String date = local_date_time(time);

		TreeItem *item = history_tree->create_item(root);
		item->set_meta("git_row", "commit");
		item->set_metadata(0, commit);
		item->set_icon(0, icon);
		if (unpushed) {
			item->set_icon_modulate(0, accent);
		}
		item->set_text(0, commit["summary"]);
		item->set_tooltip_text(0, vformat(String::utf8("%s\n\n%s · %s · %s%s"), commit["message"], commit["id"], commit["author"], date, unpushed ? "\nNot pushed yet" : ""));
		item->set_text(1, relative_time(time));
		item->set_text_overrun_behavior(1, TextServer::OVERRUN_NO_TRIMMING);
		item->set_custom_color(1, dim);
		item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
		item->set_tooltip_text(1, date);

		// Collapsed with a placeholder child, so it shows the arrow; the details load on expand.
		TreeItem *placeholder = history_tree->create_item(item);
		placeholder->set_meta("git_row", "placeholder");
		placeholder->set_selectable(0, false);
		placeholder->set_selectable(1, false);
		// Read before collapsing: collapsing emits item_collapsed, whose handler forgets the
		// commit was expanded (so Show Commit, and expanded commits across a rebuild, stayed shut).
		const bool expanded = history_expanded.has(commit["hash"]);
		item->set_collapsed(true);
		if (expanded) {
			item->set_collapsed(false); // Loads it (item_collapsed).
		}
	}

	if (more) {
		TreeItem *item = history_tree->create_item(root);
		item->set_meta("git_row", "more");
		item->set_text(0, "Load More Commits");
		item->set_custom_color(0, accent);
		item->set_tooltip_text(0, "Show 50 more commits.");
		item->set_selectable(1, false);
	}
}

// Fills an expanded commit: who and when, the rest of its message, and the files it changed.
void GitDock::_fill_commit(TreeItem *p_item) {
	while (p_item->get_first_child()) {
		memdelete(p_item->get_first_child());
	}
	const Dictionary commit = p_item->get_metadata(0);
	const String hash = commit["hash"];
	const Color dim = _dim_color();
	const String date = local_date_time(commit["time"]);

	// Notes are single lines, trimmed to the width with the whole text in the tooltip. Not
	// wrapped: the tree measures its height before wrapping, so rows below got cut off.
	const String message = commit["message"];
	// Notes are information, not buttons. The Tree highlights every row under the mouse, but draws
	// a row's own background above that highlight, so the section's color covers it.
	const Ref<StyleBoxFlat> section = history_pane->get_theme_stylebox("panel");
	const Color background = section.is_valid() ? section->get_bg_color() : Color(0, 0, 0, 0);
	auto add_note = [&](const String &p_text, const String &p_tooltip) {
		TreeItem *note = history_tree->create_item(p_item);
		note->set_meta("git_row", "note");
		note->set_text(0, p_text);
		note->set_custom_color(0, dim);
		note->set_tooltip_text(0, p_tooltip);
		for (int column = 0; column < 2; column++) {
			note->set_selectable(column, false);
			if (background.a > 0) {
				note->set_custom_bg_color(column, background);
			}
		}
	};

	// The time for today's commits, the date for older ones; both, and the full hash, in the tooltip.
	const String when = date.left(10) == local_date_time(Time::get_singleton()->get_unix_time_from_system()).left(10) ? date.substr(11, 5) : date.left(10);
	add_note(vformat(String::utf8("%s · %s"), commit["author"], when), vformat(String::utf8("%s · %s · %s"), commit["author"], date, commit["hash"]));
	// The rest of the message, a few lines of it.
	const PackedStringArray body = message.substr(String(commit["summary"]).length()).strip_edges().split("\n", false);
	for (int i = 0; i < MIN(body.size(), 6); i++) {
		add_note(i == 5 && body.size() > 6 ? String("...") : body[i].strip_edges(), message);
	}
	if (bool(commit.get("merge", false))) {
		add_note("Merge: what it brought into this branch", "A merge commit. Its files and diffs show what it brought into this branch (compared with its first parent).");
	}

	if (!commit_files.has(hash)) {
		commit_files[hash] = repo->get_commit_files(hash);
	}
	const Array files = commit_files[hash];
	if (files.is_empty()) {
		add_note("No file changes.", String());
	}
	// 500 rows is enough for any real commit; beyond that the tree would only get slow.
	const int left_out = _add_commit_file_rows(history_tree, p_item, files, hash, 500);
	if (left_out > 0) {
		add_note(vformat("...and %d more files, not listed.", left_out), String());
	}
}

// A commit's or stash's files under p_parent, with companions (player.gd.uid) on their file's row
// like the change lists (see _fill_file_pane). Returns how many files didn't fit in p_max_rows.
int GitDock::_add_commit_file_rows(Tree *p_tree, TreeItem *p_parent, const Array &p_files, const String &p_hash, int p_max_rows) {
	PackedStringArray paths;
	for (int i = 0; i < p_files.size(); i++) {
		paths.push_back(Dictionary(p_files[i])["path"]);
	}
	const Dictionary companions = grouped_companions(paths);
	int rows = 0;
	for (int i = 0; i < p_files.size(); i++) {
		if (is_grouped(companions, paths[i])) {
			continue;
		}
		if (rows == p_max_rows) {
			return p_files.size() - i;
		}
		TreeItem *item = _add_commit_file_row(p_tree, p_parent, p_files[i], p_hash);
		if (companions.has(paths[i])) {
			const PackedStringArray with = companions[paths[i]];
			item->set_meta("git_companions", with);
			item->set_tooltip_text(0, vformat("%s\nWith %s, which Godot keeps next to it.", item->get_tooltip_text(0), String(", ").join(with)));
		}
		rows++;
	}
	return 0;
}

// A row for one file of a commit or stash (p_hash; see GitRepository::get_commit_files), drawn
// like the change lists' rows, with the status letter in the second (ages') column.
TreeItem *GitDock::_add_commit_file_row(Tree *p_tree, TreeItem *p_parent, const Dictionary &p_file, const String &p_hash) {
	const String path = p_file["path"];
	const String state = p_file["status"];
	TreeItem *item = p_tree->create_item(p_parent);
	item->set_meta("git_row", "file");
	item->set_meta("git_path", path);
	item->set_meta("git_state", state);
	item->set_meta("git_hash", p_hash);
	item->set_meta("git_icon", _file_icon(path));
	item->set_cell_mode(0, TreeItem::CELL_MODE_CUSTOM);
	item->set_custom_draw_callback(0, callable_mp(this, &GitDock::_draw_file_row));
	item->set_text(0, path.get_file());
	item->set_custom_color(0, Color(0, 0, 0, 0));
	const int added = p_file["added"];
	const int removed = p_file["removed"];
	String what = status_name(state);
	if (String(p_file["old_path"]) != path) {
		what += vformat(" from %s", p_file["old_path"]);
	}
	if (added > 0 || removed > 0) {
		what += vformat(String::utf8(" · +%d %s%d"), added, minus(), removed);
	}
	item->set_tooltip_text(0, vformat("%s\n%s", path, what));
	item->set_selectable(1, false);
	// The status letter in the ages' column, so it lines up with the letters of the lists
	// above (at the right edge) instead of stopping short of this column.
	item->set_text(1, status_letter(state));
	item->set_custom_color(1, _status_color(state));
	item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
	item->set_tooltip_text(1, what);
	return item;
}

void GitDock::_on_history_item_collapsed(TreeItem *p_item) {
	if (row_kind(p_item) != "commit") {
		return;
	}
	const String hash = Dictionary(p_item->get_metadata(0))["hash"];
	if (p_item->is_collapsed()) {
		history_expanded.erase(hash);
		return;
	}
	history_expanded[hash] = true;
	// Not now: a click (on the row or its arrow) expands it while the Tree is handling that click,
	// and then the Tree refuses to create rows (create_item() returns null) and crashed us.
	callable_mp(this, &GitDock::_fill_commit_later).call_deferred(p_item->get_instance_id());
}

void GitDock::_fill_commit_later(uint64_t p_item) {
	TreeItem *item = Object::cast_to<TreeItem>(ObjectDB::get_instance(ObjectID(p_item)));
	TreeItem *first = item ? item->get_first_child() : nullptr;
	if (!item || item->is_collapsed() || !first || row_kind(first) != "placeholder") {
		return; // Gone (rebuilt), collapsed again, or already filled.
	}
	_fill_commit(item);
	_select_diff_row();
}

void GitDock::_load_more_commits() {
	history_limit += 50;
	_fill_history();
}

// Selecting a commit's file (by mouse or keyboard) shows its change in the Diff panel.
void GitDock::_on_history_item_selected() {
	TreeItem *item = history_tree->get_selected();
	if (row_kind(item) != "file") {
		return;
	}
	staged_pane.tree->deselect_all();
	changes_pane.tree->deselect_all();
	stashes_tree->deselect_all();
	_show_commit_diff(item->get_meta("git_hash"), item->get_meta("git_path"), false);
}

// History's filters: a search field (opened from the magnifier in its header) and, while History
// shows one file's commits, a bar naming the file (icon and name, like a filter chip; "History of
// ..." got cut off in a narrow dock) with a button back to all commits. Both look
// through the whole history, not just the commits already listed.
void GitDock::_build_history_filters() {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	Control *body = history_tree->get_parent_control();

	history_search_button = memnew(Button);
	history_search_button->set_tooltip_text("Search commits by message, author or hash.");
	history_search_button->set_toggle_mode(true);
	history_search_button->set_v_size_flags(SIZE_SHRINK_CENTER);
	history_search_button->set_icon_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	history_search_button->connect("pressed", callable_mp(this, &GitDock::_on_history_search_pressed));
	history_pane->add_title_bar_control(history_search_button);

	MarginContainer *search_margin = memnew(MarginContainer);
	search_margin->add_theme_constant_override("margin_bottom", Math::round(4 * scale));
	search_margin->hide();
	history_search_row = search_margin;
	history_search = memnew(LineEdit);
	history_search->set_placeholder("Search commits");
	history_search->set_clear_button_enabled(true);
	history_search->connect("text_changed", callable_mp(this, &GitDock::_on_history_search_changed));
	history_search->connect("text_submitted", callable_mp(this, &GitDock::_apply_history_search).unbind(1));
	history_search->connect("gui_input", callable_mp(this, &GitDock::_on_history_search_input));
	search_margin->add_child(history_search);
	body->add_child(search_margin);
	body->move_child(search_margin, 0);

	HBoxContainer *file_bar = memnew(HBoxContainer);
	file_bar->hide();
	history_file_bar = file_bar;
	history_file_icon = memnew(TextureRect);
	history_file_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	file_bar->add_child(history_file_icon);
	history_file_label = memnew(Label);
	history_file_label->set_h_size_flags(SIZE_EXPAND_FILL);
	history_file_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	history_file_label->set_mouse_filter(MOUSE_FILTER_PASS); // For its tooltip (the full path).
	file_bar->add_child(history_file_label);
	Button *all = memnew(Button);
	all->set_flat(true);
	all->set_tooltip_text("Show every commit again.");
	history_file_close = all;
	all->connect("pressed", callable_mp(this, &GitDock::_set_history_filter).bind(String(), String()));
	file_bar->add_child(all);
	body->add_child(file_bar);
	body->move_child(file_bar, 0);

	history_search_timer = memnew(Timer);
	history_search_timer->set_one_shot(true);
	history_search_timer->set_wait_time(0.4);
	history_search_timer->connect("timeout", callable_mp(this, &GitDock::_apply_history_search));
	add_child(history_search_timer);
}

void GitDock::_on_history_search_pressed() {
	if (history_search_row->is_visible()) {
		// Closing the search ends it.
		history_search_row->hide();
		history_search->clear();
		_set_history_filter(history_path, String());
		return;
	}
	history_pane->set_folded(false);
	history_search_row->show();
	history_search_button->set_pressed_no_signal(true);
	history_search->grab_focus();
}

void GitDock::_on_history_search_changed(const String &p_text) {
	history_search_timer->start();
}

void GitDock::_on_history_search_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && key->get_keycode() == KEY_ESCAPE) {
		history_search->accept_event();
		_on_history_search_pressed(); // Closes it.
	}
}

void GitDock::_apply_history_search() {
	history_search_timer->stop();
	if (history_search->get_text().strip_edges() != history_query) {
		_set_history_filter(history_path, history_search->get_text().strip_edges());
	}
}

// Shows p_path's commits (or all with ""), matching p_query (or all with "").
void GitDock::_set_history_filter(const String &p_path, const String &p_query) {
	history_path = p_path;
	history_query = p_query;
	history_limit = 50;
	history_file_bar->set_visible(!p_path.is_empty());
	history_file_label->set_text(p_path.get_file());
	history_file_icon->set_texture(p_path.is_empty() ? Ref<Texture2D>() : _file_icon(p_path));
	history_file_label->set_tooltip_text(vformat("History shows the commits that changed %s.", p_path));
	history_search_button->set_pressed_no_signal(history_search_row->is_visible());
	_fill_history();
}

// The Git dock up front, History unfolded and scrolled into view.
void GitDock::_scroll_to_history() {
	make_visible();
	history_pane->set_folded(false);
	for (Node *parent = history_pane->get_parent(); parent; parent = parent->get_parent()) {
		if (ScrollContainer *scroll = Object::cast_to<ScrollContainer>(parent)) {
			callable_mp(scroll, &ScrollContainer::ensure_control_visible).call_deferred(history_pane);
			break;
		}
	}
}

// The repository path of a res:// path, or "" outside the repository.
String GitDock::get_repo_path(const String &p_res_path) const {
	if (repo.is_null() || !repo->is_open()) {
		return String();
	}
	const String workdir = repo->get_workdir().simplify_path().trim_suffix("/") + "/";
	const String absolute = ProjectSettings::get_singleton()->globalize_path(p_res_path).simplify_path();
	return absolute.begins_with(workdir) ? absolute.substr(workdir.length()) : String();
}

// History narrowed to one file's commits (from the lists' and the FileSystem dock's right-click
// menus, and the script editor's).
void GitDock::show_file_history(const String &p_path) {
	if (p_path.is_empty()) {
		return;
	}
	history_search->clear();
	history_search_row->hide();
	_set_history_filter(p_path, String());
	_scroll_to_history();
}

// One commit in History, expanded: found by searching for its hash.
void GitDock::show_commit(const String &p_hash) {
	history_expanded[p_hash] = true;
	history_search_row->show();
	history_search->set_text(p_hash.left(10));
	_set_history_filter(String(), p_hash.left(10));
	_scroll_to_history();
	TreeItem *root = history_tree->get_root();
	for (TreeItem *item = root ? root->get_first_child() : nullptr; item; item = item->get_next()) {
		if (row_kind(item) == "commit" && String(Dictionary(item->get_metadata(0))["hash"]) == p_hash) {
			item->select(0);
			break;
		}
	}
}

// Show Commit for This Line, from the script editor: p_text is the editor's text (unsaved edits
// included), p_line 0-based.
void GitDock::show_line_commit(const String &p_path, const String &p_text, int p_line) {
	const Dictionary commit = repo->get_line_commit(p_path, p_text, p_line);
	if (commit.is_empty()) {
		_set_status(STATUS_NEUTRAL, vformat("Line %d of %s isn't committed yet: it's new or changed since the last commit.", p_line + 1, p_path.get_file()));
		return;
	}
	show_commit(commit["hash"]);
}

// Undo Last Commit: its changes go back to Staged Changes, and its message into the box (unless
// you're typing another one) so committing again is one click.
void GitDock::_undo_last_commit() {
	const Dictionary last = repo->get_commit("HEAD");
	const Error err = repo->undo_last_commit();
	_report(err, "Undo commit");
	if (err != OK) {
		return;
	}
	if (commit_message->get_text().strip_edges().is_empty() && !amend_check->is_pressed()) {
		commit_message->set_text(last.get("message", String()));
	}
	refresh();
	_set_status(STATUS_SUCCESS, vformat("Undid \"%s\": its changes are staged again", last.get("summary", String())));
}

void GitDock::_confirm_revert(const String &p_hash, const String &p_summary) {
	pending_revert = p_hash;
	pending_revert_summary = p_summary;
	revert_confirm->set_text(vformat("Revert \"%s\"?\n\nThis adds a new commit that undoes its changes. The commit itself stays in the history.", p_summary));
	revert_confirm->popup_centered();
}

// Like a pull, a revert rewrites files: unsaved scenes and scripts are offered to be saved first,
// and it runs in the background (a hook may run on its commit).
void GitDock::_on_revert_confirmed() {
	if (pending_revert.is_empty()) {
		return;
	}
	if (_ask_to_save("Revert", callable_mp(this, &GitDock::_on_revert_confirmed))) {
		return;
	}
	_start_network(NETWORK_REVERT);
}

// Restore This Version / Restore Version Before This Commit: the file as it was, as an uncommitted
// change. p_what finishes the status line ("as it was in abc1234").
void GitDock::_restore_version(const String &p_revision, const String &p_path, const String &p_what) {
	_remember_open_scenes();
	const Error err = repo->restore_file_version(p_revision, p_path);
	_report(err, "Restore");
	if (err != OK) {
		return;
	}
	EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	refresh();
	_set_status(STATUS_SUCCESS, vformat("Restored %s %s", p_path.get_file(), p_what));
	_reload_changed_scenes();
}

// Create Branch Here: a branch at p_hash, staying on the current branch.
void GitDock::_show_branch_here(const String &p_hash) {
	branch_here = p_hash;
	branch_dialog->set_title("Create Branch Here");
	branch_dialog_label->set_text(vformat("Create a branch at commit %s. You stay on %s.", p_hash.left(7), repo->get_current_branch()));
	branch_name_edit->clear();
	branch_dialog->popup_centered(Vector2i(360, 0) * EditorInterface::get_singleton()->get_editor_scale());
	branch_name_edit->grab_focus();
}
