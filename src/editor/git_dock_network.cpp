// Fetch / pull / push on a worker thread, and automatic background fetching.

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

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
	network_ahead = sync_status.get("ahead", 0);
	network_publish = p_op == NETWORK_PUSH && String(sync_status.get("upstream", String())).is_empty();
	_update_actions();

	if (!p_quiet) {
		_set_status(STATUS_BUSY, _network_description(p_op));
		status_cancellable = true;
		_update_status();
	}

	network_thread.instantiate();
	const String text = p_op == NETWORK_COMMIT ? network_commit_message : network_branch;
	network_thread->start(callable_mp(this, &GitDock::_network_worker).bind(p_op, repo->get_workdir(), p_quiet, text, network_amend));
}

// The operation the buttons should reflect: a background fetch doesn't count, a queued one does.
GitDock::NetworkOp GitDock::_shown_network_op() const {
	if (queued_op != NETWORK_NONE) {
		return queued_op;
	}
	return network_quiet ? NETWORK_NONE : network_op;
}

// Per project, on unless turned off in the ⋮ menu.
bool GitDock::_is_auto_fetch_enabled() const {
	return EditorInterface::get_singleton()->get_editor_settings()->get_project_metadata("godot_git", "auto_fetch", true);
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
			return vformat("Pulling from %s", upstream);
		case NETWORK_PUSH:
			return network_publish ? vformat("Publishing %s", branch) : vformat("Pushing to %s", upstream);
		case NETWORK_SWITCH:
			return vformat("Switching to %s", network_branch);
		case NETWORK_COMMIT:
			return network_amend ? String("Amending the last commit") : String("Committing");
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
		}
	}
	const String message = err == OK ? String() : GitRepository::get_last_error();
	const String upstream = err == OK ? String(worker_repo->get_sync_status().get("upstream", String())) : String();
	callable_mp(this, &GitDock::_network_done).call_deferred(p_op, (int)err, message, upstream, worker_repo->get_notice(), worker_repo->get_pull_result());
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

	// A pull or switch can change files on disk. Refresh first: the result below uses the new counts.
	if ((p_op == NETWORK_PULL || p_op == NETWORK_SWITCH) && p_err == OK) {
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

	static const char *names[] = { "", "Fetch", "Pull", "Push", "Switch branch", "Commit" };
	if (p_err == ERR_SKIP && p_op == NETWORK_COMMIT) {
		// A post-commit hook may have been running: don't claim nothing happened. History shows it.
		_set_status(STATUS_NEUTRAL, "Commit canceled.");
		return;
	}
	if (p_err == ERR_SKIP) {
		_set_status(STATUS_NEUTRAL, vformat("%s canceled. Nothing was changed.", names[p_op]));
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
		case NETWORK_PULL: {
			const int commits = p_pull_result.get("commits", 0);
			if (commits == 0) {
				_set_status(STATUS_SUCCESS, vformat("Already up to date with %s", p_upstream));
			} else if (p_pull_result.get("merged", false)) {
				_set_status(STATUS_SUCCESS, vformat("Pulled and merged %s from %s", plural(commits, "commit", "commits"), p_upstream));
			} else {
				_set_status(STATUS_SUCCESS, vformat("Pulled %s from %s", plural(commits, "commit", "commits"), p_upstream));
			}
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
		} break;
		case NETWORK_COMMIT: {
			amend_check->set_pressed_no_signal(false);
			amend_saved_draft = String();
			commit_message->clear();
			_update_actions();
			_report_commit(network_amend, network_commit_files, network_amended_id);
		} break;
	}
}

void GitDock::_finish_network_thread() {
	if (network_thread.is_valid() && network_thread->is_started()) {
		network_thread->wait_to_finish();
	}
	network_thread.unref();
}
