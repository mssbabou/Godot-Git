// History: 50 commits at a time; expanding a commit shows its details and files (built deferred,
// never inside the Tree's mouse handling: gotcha 41).

#include "editor/git_dock.h"

#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/math.hpp>

#include "editor/git_diff_dock.h"
#include "editor/git_dock_util.h"
#include "editor/ui_text.h"

using namespace godot_git;

// History: the latest commits, each expandable to its details and changed files (loaded when
// first expanded). Rebuilt only when the commits changed, so expanded commits, the selection and
// the scroll position survive the refresh that every save triggers.
void GitDock::_fill_history() {
	Array commits = repo->get_commits(history_limit + 1);
	const bool more = commits.size() > history_limit;
	if (more) {
		commits.resize(history_limit);
	}
	has_commits = !commits.is_empty();
	// For Amend. "unpushed" means on no remote-tracking branch; without remotes it's never set.
	const Dictionary last = commits.is_empty() ? Dictionary() : Dictionary(commits[0]);
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
	history_empty->set_text("No commits yet.");
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
		item->set_collapsed(true);
		if (history_expanded.has(commit["hash"])) {
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
	// Enough for any real commit; beyond that the tree would only get slow.
	constexpr int MAX_ROWS = 500;
	const Callable draw_row = callable_mp(this, &GitDock::_draw_file_row);
	for (int i = 0; i < MIN((int)files.size(), MAX_ROWS); i++) {
		const Dictionary file = files[i];
		const String path = file["path"];
		const String state = file["status"];
		TreeItem *item = history_tree->create_item(p_item);
		item->set_meta("git_row", "file");
		item->set_meta("git_path", path);
		item->set_meta("git_state", state);
		item->set_meta("git_hash", hash);
		item->set_meta("git_icon", _file_icon(path));
		item->set_cell_mode(0, TreeItem::CELL_MODE_CUSTOM);
		item->set_custom_draw_callback(0, draw_row);
		item->set_text(0, path.get_file());
		item->set_custom_color(0, Color(0, 0, 0, 0));
		const int added = file["added"];
		const int removed = file["removed"];
		String what = status_name(state);
		if (String(file["old_path"]) != path) {
			what += vformat(" from %s", file["old_path"]);
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
	}
	if (files.size() > MAX_ROWS) {
		add_note(vformat("...and %d more files, not listed.", files.size() - MAX_ROWS), String());
	}
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
	_show_commit_diff(item->get_meta("git_hash"), item->get_meta("git_path"), false);
}
