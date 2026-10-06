// Fetch / pull / push on a worker thread, and automatic background fetching.

#include "editor/git_dock.h"

#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "editor/git_dock_util.h"
#include "editor/ui_text.h"

using namespace godot_git;

// Fetch / Pull / Push pressed by the user.
void GitDock::_start_network(int p_op) {
	if (!repo->is_open() || queued_op != NETWORK_NONE) {
		return;
	}
	// With commits of your own, a pull may have to merge, which makes a commit.
	if (p_op == NETWORK_PULL && (int)sync_status.get("ahead", 0) > 0 && _ask_identity(NETWORK_PULL)) {
		return;
	}
	if ((p_op == NETWORK_PULL || p_op == NETWORK_PULL_MERGE) && _ask_to_save("Pull", callable_mp(this, &GitDock::_start_network).bind(p_op))) {
		return;
	}
	if ((p_op == NETWORK_MERGE || p_op == NETWORK_MERGE_START) && _ask_to_save("Merge", callable_mp(this, &GitDock::_start_network).bind(p_op))) {
		return;
	}
	// Both rewrite files: an unsaved scene saved afterwards would undo them.
	if ((p_op == NETWORK_ABORT || p_op == NETWORK_CONTINUE) && _ask_to_save(p_op == NETWORK_ABORT ? "Abort" : "Continue", callable_mp(this, &GitDock::_start_network).bind(p_op))) {
		return;
	}
	if (network_op == NETWORK_NONE) {
		_run_network((NetworkOp)p_op, false);
		return;
	}
	if (!network_quiet) {
		return; // The buttons are disabled; nothing to do.
	}

	// A background fetch is running.
	if (p_op == NETWORK_FETCH) {
		// Just show it: from now on it reports like a fetch you started.
		network_quiet = false;
		_set_status(STATUS_BUSY, _network_description(NETWORK_FETCH));
		status_cancellable = true;
	} else {
		// Pull/push rewrite the repository the fetch is writing to: wait for it, and say so.
		queued_op = (NetworkOp)p_op;
		network_publish = p_op == NETWORK_PUSH && String(sync_status.get("upstream", String())).is_empty();
		_set_status(STATUS_BUSY, _network_description(p_op));
		status_step = "Waiting for a background fetch...";
	}
	_update_status();
	_update_actions();
}

void GitDock::_run_network(NetworkOp p_op, bool p_quiet) {
	network_op = p_op;
	network_quiet = p_quiet;
	network_operation = _operation_name(); // The banner's operation is gone by the time it's done.
	network_operation_kind = operation.get("kind", String());
	network_ahead = sync_status.get("ahead", 0);
	network_publish = p_op == NETWORK_PUSH && String(sync_status.get("upstream", String())).is_empty();
	_update_actions();
	if (p_op == NETWORK_PULL || p_op == NETWORK_PULL_MERGE || p_op == NETWORK_SWITCH || p_op == NETWORK_ABORT || p_op == NETWORK_CONTINUE || p_op == NETWORK_REVERT || p_op == NETWORK_MERGE || p_op == NETWORK_MERGE_START) {
		_remember_open_scenes(); // To reload the ones it rewrites; see _reload_changed_scenes.
	}

	if (!p_quiet) {
		_set_status(STATUS_BUSY, _network_description(p_op));
		status_cancellable = true;
		_update_status();
	}

	network_thread.instantiate();
	// Switch: the branch; lock and unlock: the path (also in network_branch).
	const String text = p_op == NETWORK_COMMIT ? network_commit_message : (p_op == NETWORK_REVERT ? pending_revert : network_branch);
	network_thread->start(callable_mp(this, &GitDock::_network_worker).bind(p_op, repo->get_workdir(), p_quiet, text, network_amend));
}

// The operation the buttons should reflect: a background fetch doesn't count, a queued one does.
GitDock::NetworkOp GitDock::_shown_network_op() const {
	if (queued_op != NETWORK_NONE) {
		return queued_op;
	}
	return network_quiet ? NETWORK_NONE : network_op;
}

// In Editor Settings (every project), on unless turned off there or in the ⋮ menu.
bool GitDock::_is_auto_fetch_enabled() const {
	return EditorInterface::get_singleton()->get_editor_settings()->get_setting(AUTO_FETCH_SETTING);
}

// Checks every minute; fetches when the last fetch (ours or the git CLI's) is 5+ minutes old.
// After a failed attempt it also waits 5 minutes, so being offline doesn't mean constant retries.
void GitDock::_on_auto_fetch_timer() {
	auto_fetch_timer->set_wait_time(60.0); // The first check comes sooner; see READY.
	if (!_is_auto_fetch_enabled() || !repo.is_valid() || !repo->is_open() || network_op != NETWORK_NONE || !sync_status.get("has_remotes", false)) {
		return;
	}
	const int64_t now = (int64_t)Time::get_singleton()->get_unix_time_from_system();
	const int64_t last = MAX((int64_t)sync_status.get("last_fetched", 0), last_auto_fetch_attempt);
	if (now - last < 5 * 60 || !_needs_git(NETWORK_FETCH).is_empty()) {
		return;
	}
	last_auto_fetch_attempt = now;
	_run_network(NETWORK_FETCH, true);
}

// "Pulling from origin/main", for the status strip while the operation runs.
String GitDock::_network_description(int p_op) const {
	const String upstream = sync_status.get("upstream", String());
	const String branch = sync_status.get("branch", String());
	switch (p_op) {
		case NETWORK_FETCH:
			return upstream.is_empty() ? String("Fetching") : vformat("Fetching %s", upstream);
		case NETWORK_PULL:
		case NETWORK_PULL_MERGE:
			return vformat("Pulling from %s", upstream);
		case NETWORK_PUSH:
			return network_publish ? vformat("Publishing %s", branch) : vformat("Pushing to %s", upstream);
		case NETWORK_SWITCH:
			return vformat("Switching to %s", network_branch);
		case NETWORK_COMMIT:
			return network_amend ? String("Amending the last commit") : String("Committing");
		case NETWORK_ABORT:
			return String(operation.get("kind", String())) == "bisect" ? String("Ending the bisect") : vformat("Aborting the %s", _operation_name());
		case NETWORK_CONTINUE:
			if (String(operation.get("kind", String())) == "pull") {
				return "Finishing the merge";
			}
			return String(operation.get("kind", String())) == "merge" ? String("Committing the merge") : vformat("Continuing the %s", _operation_name());
		case NETWORK_REVERT:
			return vformat("Reverting \"%s\"", pending_revert_summary);
		case NETWORK_LOCK:
			return vformat("Locking %s", network_branch.get_file());
		case NETWORK_UNLOCK:
			return vformat("Unlocking %s", network_branch.get_file());
		case NETWORK_MERGE:
		case NETWORK_MERGE_START:
			return vformat("Merging %s into %s", network_branch, branch);
	}
	return String();
}

// Runs on the worker thread with its own repository handle; reports back via call_deferred.
void GitDock::_network_worker(int p_op, const String &p_workdir, bool p_quiet, const String &p_text, bool p_amend) {
	Ref<GitRepository> worker_repo;
	worker_repo.instantiate();
	worker_repo->set_progress_callback(callable_mp(this, &GitDock::_network_progress));
	// Nobody asked for a background fetch, so it must never pop up a sign-in window.
	worker_repo->set_login_prompts_allowed(!p_quiet);
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
			case NETWORK_SWITCH:
				err = worker_repo->checkout_branch(p_text);
				break;
			case NETWORK_COMMIT:
				err = p_amend ? worker_repo->amend(p_text) : worker_repo->commit(p_text);
				break;
			case NETWORK_ABORT:
				err = worker_repo->abort_operation();
				break;
			case NETWORK_CONTINUE:
				err = worker_repo->continue_operation();
				break;
			case NETWORK_REVERT:
				err = worker_repo->revert_commit(p_text);
				break;
			case NETWORK_PULL_MERGE:
				err = worker_repo->pull(true);
				break;
			case NETWORK_LOCK:
				err = worker_repo->lock_file(p_text);
				break;
			case NETWORK_UNLOCK:
				err = worker_repo->unlock_file(p_text);
				break;
			case NETWORK_MERGE:
				err = worker_repo->merge_branch(p_text);
				break;
			case NETWORK_MERGE_START:
				err = worker_repo->merge_branch(p_text, true);
				break;
		}
	}
	const String message = err == OK ? String() : GitRepository::get_last_error();
	const String upstream = err == OK ? String(worker_repo->get_sync_status().get("upstream", String())) : String();
	Dictionary result = worker_repo->get_pull_result();
	// Locks are read along with every fetch (and after locking), in repositories that use LFS.
	if ((p_op == NETWORK_FETCH || p_op == NETWORK_LOCK || p_op == NETWORK_UNLOCK) && worker_repo->is_open() && worker_repo->uses_lfs() && GitRepository::is_lfs_installed()) {
		const Array locks = worker_repo->get_lfs_locks();
		if (GitRepository::get_last_error().is_empty()) {
			result["lfs_locks"] = locks;
		}
	}
	callable_mp(this, &GitDock::_network_done).call_deferred(p_op, (int)err, message, upstream, worker_repo->get_notice(), result);
}

// Progress from the worker (already deferred to the main thread by GitRepository).
void GitDock::_network_progress(const String &p_step, double p_fraction, bool p_cancellable) {
	if (network_op == NETWORK_NONE || network_quiet || status_kind != STATUS_BUSY || status_step == "Canceling...") {
		return;
	}
	status_step = p_step;
	status_cancellable = p_cancellable;
	status_progress->set_indeterminate(p_fraction < 0);
	status_progress->set_value(MAX(p_fraction, 0.0));
	_update_status();
}

void GitDock::_network_done(int p_op, int p_err, const String &p_message, const String &p_upstream, const String &p_notice, const Dictionary &p_pull_result) {
	_finish_network_thread();
	network_op = NETWORK_NONE;
	const bool quiet = network_quiet;
	network_quiet = false;
	if (p_pull_result.has("lfs_locks")) {
		lfs_locks.clear();
		const Array locks = p_pull_result["lfs_locks"];
		for (int i = 0; i < locks.size(); i++) {
			const Dictionary lock = locks[i];
			lfs_locks[lock["path"]] = lock;
		}
		lfs_locks_read = true;
	}

	// A pull or switch can change files on disk. Refresh first: the result below uses the new counts.
	if ((p_op == NETWORK_PULL || p_op == NETWORK_PULL_MERGE || p_op == NETWORK_SWITCH || p_op == NETWORK_ABORT || p_op == NETWORK_CONTINUE || p_op == NETWORK_REVERT || p_op == NETWORK_MERGE || p_op == NETWORK_MERGE_START) && p_err == OK) {
		EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	}
	refresh();

	if (quiet) {
		// Background fetch: the refresh above already updated the counts and "Last fetched".
		// A failure is shown once (it may be offline for hours), and cleared by the next success.
		const String failure_prefix = "Automatic fetch failed.";
		if (p_err == OK && auto_fetch_failed) {
			auto_fetch_failed = false;
			if (status_kind == STATUS_WARNING && status_text.begins_with(failure_prefix)) {
				_set_status(STATUS_IDLE, String());
			}
		} else if (p_err != OK && p_err != ERR_SKIP && !auto_fetch_failed && queued_op == NETWORK_NONE) {
			auto_fetch_failed = true;
			_set_status(STATUS_WARNING, vformat("%s %s You can turn automatic fetching off in the %s menu.", failure_prefix, p_message, String::utf8("⋮")));
		}
		if (queued_op != NETWORK_NONE) {
			const NetworkOp next = queued_op;
			queued_op = NETWORK_NONE;
			_run_network(next, false);
		}
		return;
	}

	if (p_op == NETWORK_COMMIT && p_err != OK) {
		push_after_commit = false;
	}
	static const char *names[] = { "", "Fetch", "Pull", "Push", "Switch branch", "Commit", "Abort", "Continue", "Revert", "Pull", "Lock", "Unlock", "Merge", "Merge" };
	if (p_err == ERR_SKIP && p_op == NETWORK_COMMIT) {
		// A post-commit hook may have been running: don't claim nothing happened. History shows it.
		_set_status(STATUS_NEUTRAL, "Commit canceled.");
		return;
	}
	if (p_err == ERR_SKIP) {
		_set_status(STATUS_NEUTRAL, vformat("%s canceled. Nothing was changed.", names[p_op]));
		return;
	}
	if (p_op == NETWORK_SWITCH && p_err == ERR_BUSY) {
		// Changes in the way (see _switch_branch): offer to stash them.
		pending_switch = network_branch;
		stash_switch_confirm->set_text(vformat("%s\n\nStash them and switch to %s? They'll wait under Stashes, where you can restore them when you come back.", p_message, network_branch));
		stash_switch_confirm->popup_centered();
		return;
	}
	// The pull would conflict: ask whether to start a merge rather than just refuse.
	const PackedStringArray pull_conflicts = p_pull_result.get("conflicts", PackedStringArray());
	if (p_op == NETWORK_PULL && p_err != OK && !pull_conflicts.is_empty()) {
		_ask_to_start_merge(pull_conflicts, p_upstream);
		return;
	}
	if (p_err != OK) {
		const String message = p_message.is_empty() ? UtilityFunctions::error_string((Error)p_err) : p_message;
		_set_status(STATUS_ERROR, vformat("%s failed. %s", names[p_op], message));
		return;
	}
	if (!p_notice.is_empty()) {
		_set_status(STATUS_WARNING, p_notice);
		return;
	}

	const int behind = sync_status.get("behind", 0);
	switch (p_op) {
		case NETWORK_FETCH: {
			if (p_upstream.is_empty()) {
				_set_status(STATUS_SUCCESS, "Fetched");
			} else if (behind > 0) {
				_set_status(STATUS_SUCCESS, vformat("Fetched: %s to pull", plural(behind, "new commit", "new commits")));
			} else {
				_set_status(STATUS_SUCCESS, vformat("Fetched: up to date with %s", p_upstream));
			}
		} break;
		case NETWORK_PULL_MERGE:
			if (!pull_conflicts.is_empty()) {
				// Stopped at the conflicts, as asked: on to the first one.
				_set_status(STATUS_SUCCESS, vformat("Pulled from %s: resolve %s under Conflicts%s", p_upstream, plural(pull_conflicts.size(), "file", "files"), String(operation.get("kind", String())) == "merge" ? String(", then commit the merge") : String()));
				_reload_changed_scenes();
				_show_conflict(pull_conflicts[0]);
				break;
			}
			[[fallthrough]];
		case NETWORK_PULL: {
			const int commits = p_pull_result.get("commits", 0);
			// Files whose uncommitted edits were merged into the new versions: say so, since
			// those files changed under the user's edits.
			const PackedStringArray carried = p_pull_result.get("carried", PackedStringArray());
			const String kept = carried.is_empty() ? String() : vformat(". Your uncommitted edits to %s are kept on top", join_list(carried, 3));
			if (commits == 0) {
				_set_status(STATUS_SUCCESS, vformat("Already up to date with %s", p_upstream));
			} else if (p_pull_result.get("merged", false)) {
				_set_status(STATUS_SUCCESS, vformat("Pulled and merged %s from %s%s", plural(commits, "commit", "commits"), p_upstream, kept));
			} else {
				_set_status(STATUS_SUCCESS, vformat("Pulled %s from %s%s", plural(commits, "commit", "commits"), p_upstream, kept));
			}
			if (commits > 0) {
				_offer_undo();
			}
			_reload_changed_scenes(); // After the status: a scene it couldn't reload warns there.
		} break;
		case NETWORK_PUSH: {
			if (network_publish) {
				_set_status(STATUS_SUCCESS, vformat("Published to %s", p_upstream));
			} else {
				_set_status(STATUS_SUCCESS, vformat("Pushed %s to %s", plural(network_ahead, "commit", "commits"), p_upstream));
			}
		} break;
		case NETWORK_SWITCH: {
			_set_status(STATUS_SUCCESS, vformat("Switched to %s", repo->get_current_branch()));
			_offer_undo();
			_reload_changed_scenes();
		} break;
		case NETWORK_ABORT: {
			if (network_operation_kind == "stash") {
				_set_status(STATUS_SUCCESS, "Aborted the stash restore: the files are back as they were, and the stash is kept");
			} else {
				_set_status(STATUS_SUCCESS, network_operation == "bisect" ? String("Ended the bisect") : vformat("Aborted the %s", network_operation));
			}
			_reload_changed_scenes();
		} break;
		case NETWORK_CONTINUE: {
			// A rebase (or a cherry-pick of several commits) may stop at the next conflicts.
			if (_in_operation()) {
				_set_status(STATUS_WARNING, vformat("The %s went on and stopped again: see above.", _operation_name()));
			} else {
				if (network_operation_kind == "stash") {
					_set_status(STATUS_SUCCESS, "Restored the stash and removed it: its changes are uncommitted changes now");
				} else {
					_set_status(STATUS_SUCCESS, network_operation_kind == "pull" ? String("Finished the merge: your resolved changes are uncommitted, as they were before") : (network_operation == "merge" ? String("Committed the merge") : vformat("Finished the %s", network_operation)));
				}
			}
			_reload_changed_scenes();
		} break;
		case NETWORK_MERGE_START:
			if (!pull_conflicts.is_empty()) {
				// Stopped at the conflicts, as the dialog said: on to the first one.
				_set_status(STATUS_SUCCESS, vformat("Merging %s into %s: resolve %s under Conflicts%s", network_branch, repo->get_current_branch(), plural(pull_conflicts.size(), "file", "files"), String(operation.get("kind", String())) == "merge" ? String(", then commit the merge") : String()));
				_reload_changed_scenes();
				_show_conflict(pull_conflicts[0]);
				break;
			}
			[[fallthrough]];
		case NETWORK_MERGE: {
			const int commits = p_pull_result.get("commits", 0);
			const PackedStringArray carried = p_pull_result.get("carried", PackedStringArray());
			const String kept = carried.is_empty() ? String() : vformat(". Your uncommitted edits to %s are kept on top", join_list(carried, 3));
			const String current = repo->get_current_branch();
			if (commits == 0) {
				_set_status(STATUS_SUCCESS, vformat("%s has everything on %s already", current, network_branch));
			} else if (p_pull_result.get("merged", false)) {
				_set_status(STATUS_SUCCESS, vformat("Merged %s into %s: %s%s", network_branch, current, plural(commits, "commit", "commits"), kept));
			} else {
				_set_status(STATUS_SUCCESS, vformat("Merged %s into %s: %s, no merge commit needed%s", network_branch, current, plural(commits, "commit", "commits"), kept));
			}
			if (commits > 0) {
				_offer_undo();
			}
			_reload_changed_scenes();
		} break;
		case NETWORK_LOCK: {
			_set_status(STATUS_SUCCESS, vformat("Locked %s: nobody else can push changes to it until you unlock it", network_branch.get_file()));
		} break;
		case NETWORK_UNLOCK: {
			_set_status(STATUS_SUCCESS, vformat("Unlocked %s", network_branch.get_file()));
		} break;
		case NETWORK_REVERT: {
			_set_status(STATUS_SUCCESS, vformat("Reverted \"%s\" in a new commit, %s", pending_revert_summary, repo->get_commit("HEAD").get("id", String())));
			_offer_undo();
			pending_revert = String();
			_reload_changed_scenes();
		} break;
		case NETWORK_COMMIT: {
			amend_check->set_pressed_no_signal(false);
			amend_saved_draft = String();
			commit_message->clear();
			_update_actions();
			_report_commit(network_amend, network_commit_files, network_amended_id);
			_after_commit();
		} break;
	}
}

void GitDock::_finish_network_thread() {
	if (network_thread.is_valid() && network_thread->is_started()) {
		network_thread->wait_to_finish();
	}
	network_thread.unref();
}

// After a pull refused because it would conflict: "2 files conflict with origin/main", Start Merge
// or Cancel. Short on purpose; the files are listed small under it, and the resolver shows the
// rest right after.
void GitDock::_ask_to_start_merge(const PackedStringArray &p_conflicts, const String &p_upstream) {
	if (!pull_merge_confirm) {
		const float scale = EditorInterface::get_singleton()->get_editor_scale();
		pull_merge_confirm = _make_confirm("Pull", "Pull and Merge", callable_mp(this, &GitDock::_on_pull_merge_confirmed));
		pull_merge_confirm->get_cancel_button()->connect("pressed", callable_mp(this, &GitDock::_on_pull_merge_canceled));
		pull_merge_confirm->connect("canceled", callable_mp(this, &GitDock::_on_pull_merge_canceled));
		// One box for both lines: the dialog's own text label would sit on top of a child.
		VBoxContainer *box = memnew(VBoxContainer);
		pull_merge_confirm->add_child(box);
		pull_merge_question = memnew(Label);
		box->add_child(pull_merge_question);
		pull_merge_files = memnew(Label);
		pull_merge_files->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		pull_merge_files->set_custom_minimum_size(Vector2(300 * scale, 0)); // Gotcha 28.
		pull_merge_files->set_modulate(Color(1, 1, 1, 0.6));
		box->add_child(pull_merge_files);
		// Undone in Git Settings ("Ask before a pull stops at conflicts").
		pull_merge_dont_ask = memnew(CheckBox);
		// Tied to Pull and Merge: Cancel with it ticked changes nothing (remembering "cancel" would
		// make every conflicting pull silently do nothing).
		pull_merge_dont_ask->set_text("Always pull and merge");
		pull_merge_dont_ask->set_tooltip_text("From now on, pull and stop at the conflicts without asking. Turn asking back on in Git Settings.");
		box->add_child(pull_merge_dont_ask);
	}
	// A refused pull doesn't name its upstream; the branch's is the one.
	const String upstream = p_upstream.is_empty() ? String(sync_status.get("upstream", String())) : p_upstream;
	const String question = vformat("%s with %s", p_conflicts.size() == 1 ? String("1 file conflicts") : vformat("%d files conflict", p_conflicts.size()), upstream);
	pull_merge_confirm->set_text(String());
	pull_merge_question->set_text(question);
	pull_merge_files->set_text(join_list(p_conflicts, 5));
	pull_merge_dont_ask->set_pressed(false);
	pull_merge_confirm->get_ok_button()->set_tooltip_text("Pull anyway and stop at the conflicts, to resolve them under Conflicts. Abort Merge puts everything back as it was.");
	_set_status(STATUS_NEUTRAL, vformat("Pull is waiting: %s", question));
	pull_merge_confirm->reset_size();
	pull_merge_confirm->popup_centered();
}

void GitDock::_on_pull_merge_confirmed() {
	if (pull_merge_dont_ask->is_pressed()) {
		EditorInterface::get_singleton()->get_editor_settings()->set_setting(ASK_PULL_MERGE_SETTING, false);
	}
	_start_network(NETWORK_PULL_MERGE);
}

void GitDock::_on_pull_merge_canceled() {
	if (status_kind == STATUS_NEUTRAL && status_text.begins_with("Pull is waiting")) {
		_set_status(STATUS_NEUTRAL, "Pull canceled. Nothing was changed.");
	}
}
