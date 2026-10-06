// Which file the Diff panel shows, and keeping it up to date (the panel itself is GitDiffDock).

#include "editor/git_dock.h"

#include "editor/git_diff_dock.h"
#include "editor/git_dock_util.h"
#include "editor/ui_text.h"

using namespace godot_git;

void GitDock::set_diff_dock(GitDiffDock *p_dock) {
	diff_dock = p_dock;
	diff_dock->connect("open_requested", callable_mp(this, &GitDock::_open_path));
	diff_dock->get_conflict_view()->connect("resolve_requested", callable_mp(this, &GitDock::_on_conflict_text));
	diff_dock->connect("options_changed", callable_mp(this, &GitDock::_on_diff_options_changed));
	diff_dock->connect("line_changes_requested", callable_mp(this, &GitDock::_on_diff_line_changes));
	line_discard_confirm = _make_confirm("Discard Lines", "Discard", callable_mp(this, &GitDock::_on_line_discard_confirmed));
	diff_dock->get_conflict_view()->connect("side_requested", callable_mp(this, &GitDock::_on_conflict_side));
	diff_dock->get_conflict_view()->connect("settings_requested", callable_mp(this, &GitDock::_on_conflict_settings));
}

// p_focus: also bring the Diff panel up (a click), not just update it (keyboard, right-click).
void GitDock::_show_diff(const String &p_path, bool p_staged, bool p_focus) {
	diff_path = p_path;
	diff_staged = p_staged;
	diff_conflict = false;
	diff_commit = String();
	diff_commit_shown = String();
	diff_stash = false;
	_update_diff();
	if (p_focus && diff_dock) {
		diff_dock->make_visible();
	}
}

// A commit's file (History), or with p_stash a stash's file (Stashes).
void GitDock::_show_commit_diff(const String &p_hash, const String &p_path, bool p_focus, bool p_stash) {
	diff_path = p_path;
	diff_commit = p_hash;
	diff_stash = p_stash;
	diff_conflict = false;
	_update_diff();
	if (p_focus && diff_dock) {
		diff_dock->make_visible();
	}
}

// Brings the Diff panel up to date after a refresh. A file that was staged or unstaged meanwhile
// is followed to its other list, so staging the file you're looking at keeps it on screen.
void GitDock::_update_diff() {
	if (!diff_dock || diff_path.is_empty() || repo.is_null() || !repo->is_open()) {
		return;
	}
	// The panel's context lines and whitespace setting (saved per project, read when it's ready).
	repo->set_diff_options(diff_dock->get_context_lines(), diff_dock->is_ignoring_whitespace());
	// A conflicted file opens the resolver. The same conflict handed over again changes nothing
	// in the panel (set_diff skips what's already shown), so a refresh keeps what's been chosen.
	if (diff_conflict) {
		if (conflicted_paths.has(diff_path)) {
			Dictionary conflict = repo->get_conflict(diff_path);
			conflict["conflict"] = true;
			conflict["status"] = "conflicted";
			if (GitDiffDock::is_image_path(diff_path)) {
				conflict["image_old"] = repo->get_file_bytes("mine", diff_path);
				conflict["image_new"] = repo->get_file_bytes("theirs", diff_path);
			} else if (GitDiffDock::is_audio_path(diff_path)) {
				conflict["audio_old"] = repo->get_file_bytes("mine", diff_path);
				conflict["audio_new"] = repo->get_file_bytes("theirs", diff_path);
			}
			diff_dock->set_diff(conflict, "Conflict", _file_icon(diff_path));
			return;
		}
		diff_conflict = false; // Resolved elsewhere (a terminal): show what's left of it.
	}
	if (!diff_commit.is_empty()) {
		const String key = diff_commit + ":" + diff_path;
		if (key != diff_commit_shown) {
			diff_commit_shown = key;
			Dictionary diff;
			if (diff_stash) {
				// A new file the stash took along is in its third parent (see get_stash_files).
				diff = repo->get_stash_diff(diff_commit, diff_path);
				_add_image_versions(diff, diff_commit + "^1", repo->has_file_at(diff_commit, diff_path) ? diff_commit : diff_commit + "^3");
			} else {
				diff = repo->get_commit_diff(diff_commit, diff_path);
				_add_image_versions(diff, diff_commit + "^1", diff_commit);
				diff["commit"] = diff_commit; // The panel shows its hash, and copies it on a click.
			}
			// The companions the file's row stands for (see _add_commit_file_rows): in the same commit.
			const Array files = (diff_stash ? stash_files : commit_files).get(diff_commit, Array());
			Array companions;
			if (companion_owner(diff_path).is_empty()) {
				for (const char *suffix : { ".import", ".uid" }) {
					const String path = vformat("%s%s", diff_path, suffix);
					for (int i = 0; i < files.size(); i++) {
						if (String(Dictionary(files[i])["path"]) == path) {
							companions.push_back(diff_stash ? repo->get_stash_diff(diff_commit, path) : repo->get_commit_diff(diff_commit, path));
							break;
						}
					}
				}
			}
			if (!companions.is_empty()) {
				diff["companions"] = companions;
			}
			diff_dock->set_diff(diff, diff_stash ? String("Stash") : vformat("Commit %s", diff_commit.left(7)), _file_icon(diff_path));
		}
		_select_diff_row();
		return;
	}
	Dictionary diff = repo->get_diff(diff_path, diff_staged);
	if (diff.get("kind", String()) == "unchanged") {
		const Dictionary other = repo->get_diff(diff_path, !diff_staged);
		if (other.get("kind", String()) != "unchanged") {
			diff_staged = !diff_staged;
			diff = other;
		}
	}
	if (diff.get("kind", String()) != "unchanged") {
		_add_image_versions(diff, diff_staged ? "HEAD" : "index", diff_staged ? "index" : "workdir");
		// The companions the file's row stands for (see _fill_file_pane): changed in the same list.
		Array companions;
		if (companion_owner(diff_path).is_empty()) {
			for (const char *suffix : { ".import", ".uid" }) {
				const Dictionary companion = repo->get_diff(vformat("%s%s", diff_path, suffix), diff_staged);
				if (companion.get("kind", String()) != "unchanged" && !companion.is_empty()) {
					companions.push_back(companion);
				}
			}
		}
		if (!companions.is_empty()) {
			diff["companions"] = companions;
		}
	}
	diff_dock->set_diff(diff, diff_staged ? "Staged" : "Unstaged", _file_icon(diff_path));
	_select_diff_row();
}

// An image's two versions, for the Diff panel's before | after (the old one under its old name,
// for a rename). Versions as in GitRepository::get_file_bytes.
void GitDock::_add_image_versions(Dictionary &r_diff, const String &p_old_version, const String &p_new_version) {
	const bool image = GitDiffDock::is_image_path(diff_path);
	if (!image && !GitDiffDock::is_audio_path(diff_path)) {
		return;
	}
	r_diff[image ? "image_old" : "audio_old"] = repo->get_file_bytes(p_old_version, r_diff.get("old_path", diff_path));
	r_diff[image ? "image_new" : "audio_new"] = repo->get_file_bytes(p_new_version, diff_path);
}

// Marks the file the Diff panel shows in its list (the lists are rebuilt on every refresh).
void GitDock::_select_diff_row() {
	if (!diff_commit.is_empty()) {
		Tree *tree = diff_stash ? stashes_tree : history_tree; // Both: parent rows with file rows under them.
		TreeItem *root = tree->get_root();
		for (TreeItem *parent = root ? root->get_first_child() : nullptr; parent; parent = parent->get_next()) {
			for (TreeItem *item = parent->get_first_child(); item; item = item->get_next()) {
				if (item->has_meta("git_hash") && String(item->get_meta("git_hash")) == diff_commit && String(item->get_meta("git_path")) == diff_path) {
					if (!tree->get_selected()) {
						item->select(0);
					}
					return;
				}
			}
		}
		return;
	}
	FilePane &pane = diff_staged ? staged_pane : changes_pane;
	TreeItem *root = pane.tree->get_root();
	for (TreeItem *item = root ? root->get_first_child() : nullptr; item; item = item->get_next()) {
		if (item->get_metadata(COLUMN_NAME).get_type() == Variant::STRING && String(item->get_metadata(COLUMN_NAME)) == diff_path) {
			if (!pane.tree->get_next_selected(nullptr)) {
				item->select(COLUMN_NAME); // Doesn't emit multi_selected, so it can't loop back here.
			}
			return;
		}
	}
}

// The Diff panel's context lines or whitespace setting changed: read the shown diff again with them.
void GitDock::_on_diff_options_changed() {
	if (!diff_dock || repo.is_null()) {
		return;
	}
	repo->set_diff_options(diff_dock->get_context_lines(), diff_dock->is_ignoring_whitespace());
	diff_commit_shown = String(); // A commit's diff is otherwise never read twice.
	_update_diff();
}

// Staging, unstaging or discarding part of the file shown (a hunk or chosen lines, from the Diff
// panel). Discarding asks first: those edits are gone from the file afterwards.
void GitDock::_on_diff_line_changes(const String &p_action, const Array &p_lines) {
	if (!diff_commit.is_empty() || diff_path.is_empty() || p_lines.is_empty()) {
		return;
	}
	if (p_action == "discard") {
		pending_line_discard = p_lines;
		line_discard_confirm->set_text(vformat("Discard %s in %s?\n\nThe file goes back to how it was there. The rest of your changes stay. This can't be undone.", plural(p_lines.size(), "changed line", "changed lines"), diff_path.get_file()));
		line_discard_confirm->popup_centered();
		return;
	}
	_apply_line_changes(p_action, p_lines);
}

void GitDock::_on_line_discard_confirmed() {
	_apply_line_changes("discard", pending_line_discard);
	pending_line_discard.clear();
}

void GitDock::_apply_line_changes(const String &p_action, const Array &p_lines) {
	const bool discard = p_action == "discard";
	if (discard) {
		_remember_open_scenes();
	}
	const Error err = repo->apply_line_changes(diff_path, diff_staged, p_action, p_lines);
	_report(err, discard ? "Discard" : (p_action == "stage" ? "Stage" : "Unstage"));
	refresh();
	if (err == OK && discard) {
		_set_status(STATUS_SUCCESS, vformat("Discarded %s in %s", plural(p_lines.size(), "changed line", "changed lines"), diff_path.get_file()));
		_reload_changed_scenes(); // An open script gets the file's new text (or asks, per Godot's setting).
	}
}
