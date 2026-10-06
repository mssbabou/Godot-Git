// The commit box: the message (Ctrl+Enter, Ctrl+Shift+Enter, Up for recent messages), Amend,
// Commit, the large-file question, and reporting the new commit.

#include "editor/git_dock.h"

#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/core/math.hpp>

#include "addon_files.h"
#include "build_info.h"
#include "editor/file_opener.h"
#include "editor/filesystem_colors.h"
#include "editor/ui_text.h"

using namespace godot_git;

void GitDock::_on_commit_message_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed()) {
		return;
	}
	if (!key->is_echo() && key->is_command_or_control_pressed() && (key->get_keycode() == KEY_ENTER || key->get_keycode() == KEY_KP_ENTER)) {
		commit_message->accept_event();
		if (!commit_button->is_disabled()) {
			// With Shift: push right after, if there's a remote to push to.
			push_after_commit = key->is_shift_pressed() && sync_status.get("has_remotes", false);
			_commit();
		}
		return;
	}
	// Up in an empty box (or on a message it put there) brings back your earlier messages.
	const bool plain = !key->is_command_or_control_pressed() && !key->is_shift_pressed() && !key->is_alt_pressed();
	const bool recalled = message_history_index >= 0 && message_history_index < message_history.size() && commit_message->get_text() == message_history[message_history_index];
	if (plain && key->get_keycode() == KEY_UP && (commit_message->get_text().is_empty() || recalled) && commit_message->get_caret_line() == 0) {
		commit_message->accept_event();
		if (!recalled) {
			message_history_index = -1; // An empty box starts again from the newest.
		}
		_recall_message(1);
	} else if (plain && key->get_keycode() == KEY_DOWN && recalled && commit_message->get_caret_line() == commit_message->get_line_count() - 1) {
		commit_message->accept_event();
		_recall_message(-1);
	}
}

// p_step 1 goes one message further back, -1 one newer (and past the newest, back to empty).
void GitDock::_recall_message(int p_step) {
	if (message_history_index < 0 && p_step > 0) {
		// Your own recent messages, newest first, without repeats. Not merges: git makes up their
		// messages ("Merge branch 'main'"), which aren't worth bringing back.
		message_history.clear();
		const String me = repo->get_identity().get("name", String());
		const Array commits = repo->get_commits(100);
		for (int i = 0; i < commits.size() && message_history.size() < 20; i++) {
			const Dictionary commit = commits[i];
			const String message = commit["message"];
			if (String(commit["author"]) == me && !bool(commit["merge"]) && !message.is_empty() && !message_history.has(message)) {
				message_history.push_back(message);
			}
		}
	}
	const int index = message_history_index + p_step;
	if (index >= message_history.size()) {
		return;
	}
	message_history_index = MAX(index, -1);
	commit_message->set_text(message_history_index >= 0 ? message_history[message_history_index] : String());
	commit_message->set_caret_line(0);
	commit_message->set_caret_column(0);
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
	const bool merging = String(operation.get("kind", String())) == "merge";
	if (message.is_empty() || (staged_count == 0 && !amending && !merging)) {
		return;
	}
	if (_ask_identity(NETWORK_COMMIT)) {
		return; // Commits once the name and email are saved.
	}
	if (_ask_about_large_files()) {
		return; // Commits if confirmed.
	}
	const int files = staged_count;
	const String old_id = last_commit_id;
	commit_was_merge = merging;
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
		_after_commit();
	} else {
		push_after_commit = false;
	}
}

// Staged files over 50 MB (where GitHub starts warning; it refuses 100 MB) get a question first:
// once committed, a file stays in the history for good and every clone downloads it. Returns true
// if it asked; the commit then happens on "Commit Anyway".
bool GitDock::_ask_about_large_files() {
	if (large_checked) {
		large_checked = false;
		return false;
	}
	const Array large = repo->get_large_staged_files(50 * 1024 * 1024);
	if (large.is_empty()) {
		return false;
	}
	PackedStringArray lines;
	for (int i = 0; i < MIN(large.size(), 8); i++) {
		const Dictionary file = large[i];
		lines.push_back(vformat(String::utf8("• %s (%s)"), file["path"], String::humanize_size(file["size"])));
	}
	if (large.size() > 8) {
		lines.push_back(vformat("...and %d more", large.size() - 8));
	}
	// The other ways out: store their types with Git LFS, or (new files only: ignoring a file git
	// already tracks changes nothing) take them out and keep them out.
	large_paths.clear();
	PackedStringArray patterns;
	bool all_new = true;
	const Array status = repo->get_status();
	for (int i = 0; i < large.size(); i++) {
		const String path = Dictionary(large[i])["path"];
		large_paths.push_back(path);
		const String pattern = path.get_extension().is_empty() ? String() : vformat("*.%s", path.get_extension());
		if (!pattern.is_empty() && !patterns.has(pattern)) {
			patterns.push_back(pattern);
		}
		for (const Variant &entry : status) {
			if (String(Dictionary(entry)["path"]) == path) {
				all_new = all_new && String(Dictionary(entry)["index"]) == "new";
			}
		}
	}
	const bool lfs = GitRepository::is_lfs_installed();
	large_lfs_button->set_visible(lfs && !patterns.is_empty());
	large_lfs_button->set_tooltip_text(vformat("Store %s files with Git LFS from now on: added to .gitattributes (staged), and these files staged again as LFS files. Your remote must support LFS; GitHub, GitLab and Bitbucket do.", join_list(patterns)));
	large_ignore_button->set_visible(all_new);
	large_ignore_button->set_tooltip_text(large.size() == 1 ? String("Unstage it and add it to .gitignore: it stays on your disk, out of git.") : String("Unstage them and add them to .gitignore: they stay on your disk, out of git."));
	large_confirm->set_text(vformat("%s:\n%s\n\nOnce committed, a file stays in the history for good, and everyone who clones the repository downloads it. GitHub refuses files over 100 MB. Git LFS keeps big files like these out of the history%s.",
			large.size() == 1 ? String("This staged file is over 50 MB") : String("These staged files are over 50 MB"), String("\n").join(lines), lfs ? String() : String(" (install it from git-lfs.com, then restart the editor)")));
	large_confirm->popup_centered();
	push_after_commit = false; // Pushing big files is exactly what to think twice about.
	return true;
}

void GitDock::_on_large_confirmed() {
	large_checked = true;
	_commit();
}

// Track with Git LFS, or Unstage and Ignore. Neither commits: the files get another look first,
// and Commit goes on from there.
void GitDock::_on_large_custom_action(const StringName &p_action) {
	large_confirm->hide();
	if (p_action == StringName("lfs")) {
		PackedStringArray patterns;
		for (const String &path : large_paths) {
			const String pattern = vformat("*.%s", path.get_extension());
			if (!path.get_extension().is_empty() && !patterns.has(pattern)) {
				patterns.push_back(pattern);
			}
		}
		const Error err = repo->track_with_lfs(patterns, large_paths);
		_report(err, "Track with Git LFS");
		refresh();
		if (err == OK) {
			_set_status(STATUS_SUCCESS, vformat("%s files are stored with Git LFS from now on (.gitattributes is staged). Commit when ready", join_list(patterns)));
		}
	} else if (p_action == StringName("ignore")) {
		PackedStringArray untracked;
		for (const Variant &entry : repo->get_status()) {
			if (String(Dictionary(entry)["worktree"]) == "untracked") {
				untracked.push_back(Dictionary(entry)["path"]);
			}
		}
		Error err = OK;
		for (int i = 0; err == OK && i < large_paths.size(); i++) {
			const String file = _ignore_file_for(large_paths[i]);
			err = repo->unstage(large_paths[i]);
			if (err == OK) {
				err = repo->add_ignore_lines(file, _file_ignore_lines(large_paths[i], file, untracked));
			}
		}
		_report(err, "Unstage and Ignore");
		refresh();
		if (err == OK) {
			PackedStringArray names;
			for (const String &path : large_paths) {
				names.push_back(path.get_file());
			}
			_set_status(STATUS_SUCCESS, vformat("Unstaged and ignored %s: on your disk, out of git", join_list(names, 3)));
		}
	}
}

// After a successful commit: Ctrl+Shift+Enter pushes it right away.
void GitDock::_after_commit() {
	message_history_index = -1;
	if (push_after_commit) {
		push_after_commit = false;
		_start_network(NETWORK_PUSH);
	}
}

// The commit's page on GitHub, GitLab or Bitbucket, from the upstream's remote (else origin, else
// the only remote). Empty for other hosts: no guessing at URLs that may not exist.
String GitDock::_web_commit_url(const String &p_hash) const {
	String remote = String(sync_status.get("upstream", String())).get_slice("/", 0);
	const PackedStringArray remotes = repo->get_remotes();
	if (remote.is_empty() || !remotes.has(remote)) {
		remote = remotes.has("origin") || remotes.is_empty() ? String("origin") : remotes[0];
	}
	const String site = web_repository_url(repo->get_remote_url(remote));
	if (site.is_empty()) {
		return String();
	}
	if (site.contains("://gitlab.com/")) {
		return vformat("%s/-/commit/%s", site, p_hash);
	}
	if (site.contains("://bitbucket.org/")) {
		return vformat("%s/commits/%s", site, p_hash);
	}
	return vformat("%s/commit/%s", site, p_hash);
}

// After refresh(), so last_commit_id is the new commit.
void GitDock::_report_commit(bool p_amended, int p_files, const String &p_old_id) {
	if (commit_was_merge) {
		commit_was_merge = false;
		merge_prefill = String();
		_set_status(STATUS_SUCCESS, p_files > 0 ? vformat("Committed the merge, %s (%s)", last_commit_id, plural(p_files, "file", "files")) : vformat("Committed the merge, %s: no file changes, the branch is recorded as merged", last_commit_id));
	} else if (!p_amended) {
		_set_status(STATUS_SUCCESS, vformat("Committed %s (%s)", last_commit_id, plural(p_files, "file", "files")));
	} else if (p_files > 0) {
		_set_status(STATUS_SUCCESS, vformat("Amended %s, now %s (%s added)", p_old_id, last_commit_id, plural(p_files, "file", "files")));
	} else {
		_set_status(STATUS_SUCCESS, vformat("Amended %s, now %s", p_old_id, last_commit_id));
	}
	_offer_undo();
}
