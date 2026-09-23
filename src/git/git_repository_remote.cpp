// GitRepository's network half: fetch, pull and push. They run on a worker thread in the dock,
// report progress through the progress callback, and can be canceled from any thread.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include "git/git_remote_callbacks.h"
#include "git/git_util.h"

using namespace godot_git;

Error GitRepository::_fetch_remote(const String &p_remote) {
	git_remote *remote = nullptr;
	int err = git_remote_lookup(&remote, repo, p_remote.utf8().get_data());
	if (err < 0) {
		return to_error(err);
	}

	RemoteContext ctx;
	ctx.workdir = get_workdir();
	ctx.login_prompts_allowed = login_prompts_allowed;
	ctx.progress = progress_callback;
	git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
	set_remote_callbacks(opts.callbacks, ctx);

	report_progress(&ctx, "Connecting...", String(), -1);
	err = git_remote_fetch(remote, nullptr, &opts, "fetch");
	git_remote_free(remote);
	return remote_error(err);
}

// Fetches the current branch's remote, or every remote if the branch has no upstream.
Error GitRepository::fetch() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	begin_network_operation();

	const PackedStringArray remotes = get_remotes();
	if (remotes.is_empty()) {
		return fail("This repository has no remote.");
	}

	git_reference *head = nullptr;
	if (git_repository_head(&head, repo) == 0) {
		git_buf remote_name = GIT_BUF_INIT;
		const int err = git_branch_upstream_remote(&remote_name, repo, git_reference_name(head));
		git_reference_free(head);
		if (err == 0) {
			return _fetch_remote(buf_to_string(remote_name));
		}
		git_buf_dispose(&remote_name);
	}

	for (const String &remote : remotes) {
		const Error err = _fetch_remote(remote);
		if (err != OK) {
			return err;
		}
	}
	return OK;
}

namespace {

// Paths with uncommitted changes (staged, unstaged, or a new file) that the incoming commits
// would change, sorted. A pull that touches any of them is refused before anything is done.
PackedStringArray paths_blocking_pull(git_repository *p_repo, const git_oid *p_head, const git_oid *p_theirs) {
	PackedStringArray blocking;
	git_oid base;
	if (git_merge_base(&base, p_repo, p_head, p_theirs) != 0) {
		return blocking;
	}
	const HashSet<String> incoming = changed_paths(p_repo, &base, p_theirs);
	for (const String &path : uncommitted_paths(p_repo)) {
		if (incoming.has(path) && !blocking.has(path)) {
			blocking.push_back(path);
		}
	}
	blocking.sort();
	return blocking;
}

Error fast_forward(git_repository *p_repo, git_reference *p_head, const git_annotated_commit *p_theirs, RemoteContext &p_ctx) {
	const git_oid *target_oid = git_annotated_commit_id(p_theirs);
	git_object *target = nullptr;
	int err = git_object_lookup(&target, p_repo, target_oid, GIT_OBJECT_COMMIT);
	if (err >= 0) {
		git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
		opts.checkout_strategy = GIT_CHECKOUT_SAFE;
		opts.progress_cb = checkout_progress_cb;
		opts.progress_payload = &p_ctx;
		err = git_checkout_tree(p_repo, target, &opts);
		if (err == GIT_ECONFLICT) {
			git_object_free(target);
			return fail("Your local changes would be overwritten by the pull. Commit or discard them first.");
		}
	}
	if (err >= 0) {
		git_reference *moved = nullptr;
		err = git_reference_set_target(&moved, p_head, target_oid, "pull: fast-forward");
		git_reference_free(moved);
	}
	git_object_free(target);
	return to_error(err);
}

// Merges p_theirs into HEAD and commits the result. On conflicts the merge is fully undone
// (hard reset, merge state cleared) and an error is returned.
Error merge_and_commit(git_repository *p_repo, git_reference *p_head, git_reference *p_upstream, const git_annotated_commit *p_theirs, RemoteContext &p_ctx) {
	report_progress(&p_ctx, "Merging...", String(), -1, false);
	git_merge_options merge_opts = GIT_MERGE_OPTIONS_INIT;
	git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
	checkout_opts.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_ALLOW_CONFLICTS;
	checkout_opts.progress_cb = checkout_progress_cb;
	checkout_opts.progress_payload = &p_ctx;
	int err = git_merge(p_repo, &p_theirs, 1, &merge_opts, &checkout_opts);
	if (err < 0) {
		// git_merge itself refused (e.g. it would overwrite an untracked file); nothing changed.
		git_repository_state_cleanup(p_repo);
		return to_error(err);
	}

	git_index *index = nullptr;
	err = git_repository_index(&index, p_repo);
	if (err >= 0 && git_index_has_conflicts(index)) {
		git_index_free(index);
		git_object *head_commit = nullptr;
		git_revparse_single(&head_commit, p_repo, "HEAD");
		git_reset(p_repo, head_commit, GIT_RESET_HARD, nullptr);
		git_object_free(head_commit);
		git_repository_state_cleanup(p_repo);
		return fail("Pulling would cause merge conflicts, so nothing was changed. Resolving conflicts isn't supported in the panel yet; use git in a terminal for this one.");
	}

	git_oid tree_oid, commit_oid;
	git_tree *tree = nullptr;
	git_signature *signature = nullptr;
	git_commit *ours = nullptr;
	git_commit *their_commit = nullptr;
	if (err >= 0) {
		err = git_index_write_tree(&tree_oid, index);
	}
	if (err >= 0) {
		err = git_tree_lookup(&tree, p_repo, &tree_oid);
	}
	if (err >= 0) {
		err = git_signature_default(&signature, p_repo);
	}
	if (err >= 0) {
		err = git_commit_lookup(&ours, p_repo, git_reference_target(p_head));
	}
	if (err >= 0) {
		err = git_commit_lookup(&their_commit, p_repo, git_annotated_commit_id(p_theirs));
	}
	if (err >= 0) {
		const git_commit *parents[2] = { ours, their_commit };
		const String message = vformat("Merge remote-tracking branch '%s'", String::utf8(git_reference_shorthand(p_upstream)));
		err = git_commit_create(&commit_oid, p_repo, "HEAD", signature, signature, nullptr, message.utf8().get_data(), tree, 2, parents);
	}
	git_repository_state_cleanup(p_repo);

	git_commit_free(their_commit);
	git_commit_free(ours);
	git_signature_free(signature);
	git_tree_free(tree);
	git_index_free(index);
	return to_error(err);
}

// merge_and_commit, with uncommitted changes to tracked files (normal in Godot, which rewrites
// project.godot and scenes) set aside first and put back afterwards, like `git pull --autostash`.
// That's what lets a conflicting merge be undone completely. r_notice explains if the changes
// couldn't be put back (a safety net: paths_blocking_pull already rules that out).
Error merge_with_autostash(git_repository *p_repo, git_reference *p_head, git_reference *p_upstream, const git_annotated_commit *p_theirs, RemoteContext &p_ctx, String &r_notice) {
	git_status_options status_opts = GIT_STATUS_OPTIONS_INIT;
	status_opts.flags = 0; // Tracked files only; untracked files are left where they are.
	git_status_list *status = nullptr;
	const bool dirty = git_status_list_new(&status, p_repo, &status_opts) == 0 && git_status_list_entrycount(status) > 0;
	git_status_list_free(status);
	if (!dirty) {
		return merge_and_commit(p_repo, p_head, p_upstream, p_theirs, p_ctx);
	}

	git_oid stash_id;
	git_signature *stasher = nullptr;
	int err = git_signature_default(&stasher, p_repo);
	if (err >= 0) {
		err = git_stash_save(&stash_id, p_repo, stasher, "godot-git: your changes, set aside while pulling", GIT_STASH_DEFAULT);
	}
	git_signature_free(stasher);
	if (err < 0) {
		return to_error(err);
	}

	const Error result = merge_and_commit(p_repo, p_head, p_upstream, p_theirs, p_ctx);
	// Keep the merge's own error message: libgit2 may overwrite it while restoring.
	const String merge_error = result == OK ? String() : GitRepository::get_last_error();

	// libgit2 (unlike the git CLI) writes conflict markers into files and drops the stash when
	// putting changes back conflicts. So if the merge touched a stashed file after all, leave the
	// user's changes untouched in the stash.
	PackedStringArray overlap;
	if (result == OK) {
		const HashSet<String> merged = changed_paths(p_repo, git_reference_target(p_head), nullptr);
		for (const String &path : stashed_paths(p_repo, &stash_id)) {
			if (merged.has(path)) {
				overlap.push_back(path);
			}
		}
	}
	bool restored = false;
	if (overlap.is_empty()) {
		git_stash_apply_options apply_opts = GIT_STASH_APPLY_OPTIONS_INIT;
		apply_opts.flags = GIT_STASH_APPLY_REINSTATE_INDEX;
		restored = git_stash_pop(p_repo, 0, &apply_opts) >= 0;
	}

	if (result == OK && !restored) {
		r_notice = vformat("Pulled, but it also changed %s, which you had uncommitted edits to. Your edits are kept in a git stash, untouched; run `git stash pop` in a terminal to merge them back.",
				overlap.is_empty() ? String("files") : String(", ").join(overlap.slice(0, 3)) + (overlap.size() > 3 ? String(", ...") : String()));
	} else if (result != OK) {
		fail(restored ? merge_error : vformat("%s Your uncommitted changes are saved in a git stash; run `git stash pop` in a terminal to get them back.", merge_error));
	}
	return result;
}

} // namespace

// Fetches the upstream, then fast-forwards, or creates a merge commit when both sides have new
// commits. It refuses (changing nothing) when the new commits touch files with uncommitted
// changes, or when the merge would conflict.
Error GitRepository::pull() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	begin_network_operation();
	notice = String();
	pulled_commits = 0;
	pull_merged = false;

	git_reference *head = nullptr;
	if (head_branch(&head, repo) < 0) {
		return FAILED;
	}

	git_buf remote_buf = GIT_BUF_INIT;
	if (git_branch_upstream_remote(&remote_buf, repo, git_reference_name(head)) < 0) {
		git_buf_dispose(&remote_buf);
		git_reference_free(head);
		return fail("This branch isn't tracking a remote branch yet. Push it first.");
	}
	const Error fetch_err = _fetch_remote(buf_to_string(remote_buf));
	if (fetch_err != OK) {
		git_reference_free(head);
		return fetch_err;
	}

	git_reference *upstream = nullptr;
	if (git_branch_upstream(&upstream, head) < 0) {
		git_reference_free(head);
		return fail("The upstream branch doesn't exist on the remote anymore.");
	}
	git_annotated_commit *theirs = nullptr;
	git_merge_analysis_t analysis = GIT_MERGE_ANALYSIS_NONE;
	git_merge_preference_t preference = GIT_MERGE_PREFERENCE_NONE;
	int err = git_annotated_commit_from_ref(&theirs, repo, upstream);
	if (err >= 0) {
		err = git_merge_analysis(&analysis, &preference, repo, (const git_annotated_commit **)&theirs, 1);
	}

	// From here on everything is local: progress only, no more canceling.
	RemoteContext ctx;
	ctx.progress = progress_callback;
	const git_oid *head_oid = git_reference_target(head);

	Error result = to_error(err);
	PackedStringArray blocking;
	if (err >= 0 && !(analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE)) {
		blocking = paths_blocking_pull(repo, head_oid, git_annotated_commit_id(theirs));
	}

	if (err < 0 || (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE)) {
		// Error, or nothing to do.
	} else if (!blocking.is_empty()) {
		// Checked before anything is touched, so your edits never end up in a stash only a
		// terminal can get back.
		result = fail(vformat("Nothing was pulled: the new commits change %s, which you have uncommitted changes to. Commit or discard your changes to %s first, then pull again.",
				String(", ").join(blocking.slice(0, 3)) + (blocking.size() > 3 ? vformat(" and %d more", blocking.size() - 3) : String()),
				blocking.size() == 1 ? String("it") : String("them")));
	} else {
		size_t ahead = 0, behind = 0;
		if (git_graph_ahead_behind(&ahead, &behind, repo, head_oid, git_annotated_commit_id(theirs)) == 0) {
			pulled_commits = (int)behind;
		}
		if ((analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) && !(preference & GIT_MERGE_PREFERENCE_NO_FASTFORWARD)) {
			result = fast_forward(repo, head, theirs, ctx);
		} else {
			pull_merged = true;
			result = merge_with_autostash(repo, head, upstream, theirs, ctx, notice);
		}
		if (result != OK) {
			pulled_commits = 0;
			pull_merged = false;
		}
	}

	git_annotated_commit_free(theirs);
	git_reference_free(upstream);
	git_reference_free(head);
	return result;
}

// Pushes the current branch to its upstream. A branch without one is published to "origin"
// (or the only remote) under the same name and starts tracking it.
Error GitRepository::push() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	begin_network_operation();

	git_reference *head = nullptr;
	if (head_branch(&head, repo) < 0) {
		return FAILED;
	}
	const String local_ref = String::utf8(git_reference_name(head));
	const String branch = String::utf8(git_reference_shorthand(head));

	String remote_name;
	String remote_ref;
	bool publish = false;

	git_buf buf = GIT_BUF_INIT;
	if (git_branch_upstream_remote(&buf, repo, local_ref.utf8().get_data()) == 0) {
		remote_name = buf_to_string(buf);
		if (git_branch_upstream_merge(&buf, repo, local_ref.utf8().get_data()) == 0) {
			remote_ref = buf_to_string(buf);
		}
	}
	git_buf_dispose(&buf);

	if (remote_name.is_empty() || remote_ref.is_empty()) {
		const PackedStringArray remotes = get_remotes();
		if (remotes.is_empty()) {
			git_reference_free(head);
			return fail("This repository has no remote to push to.");
		}
		remote_name = remotes.has("origin") ? String("origin") : remotes[0];
		remote_ref = local_ref;
		publish = true;
	}

	git_remote *remote = nullptr;
	int err = git_remote_lookup(&remote, repo, remote_name.utf8().get_data());
	if (err < 0) {
		git_reference_free(head);
		return to_error(err);
	}

	RemoteContext ctx;
	ctx.workdir = get_workdir();
	ctx.login_prompts_allowed = login_prompts_allowed;
	ctx.progress = progress_callback;
	git_push_options opts = GIT_PUSH_OPTIONS_INIT;
	set_remote_callbacks(opts.callbacks, ctx);

	report_progress(&ctx, "Connecting...", String(), -1);
	SinglePathspec refspec(vformat("%s:%s", local_ref, remote_ref));
	err = git_remote_push(remote, &refspec.array, &opts);
	git_remote_free(remote);

	Error result = remote_error(err);
	if (result == ERR_SKIP) {
		// Canceled.
	} else if (err == GIT_ENONFASTFORWARD) {
		// Both "remote has commits not present locally" (not fetched yet) and "diverged" (fetched).
		result = fail("The remote has commits you don't have yet. Pull first, then push.");
	} else if (err >= 0 && (ctx.push_rejection.contains("fetch first") || ctx.push_rejection.contains("non-fast-forward"))) {
		// The same, as reported by the server when it knows more than we fetched.
		result = fail("The remote has commits you don't have yet. Pull first, then push.");
	} else if (err >= 0 && !ctx.push_rejection.is_empty()) {
		result = fail("The remote rejected the push: " + ctx.push_rejection);
	} else if (err >= 0 && publish) {
		git_branch_set_upstream(head, vformat("%s/%s", remote_name, branch).utf8().get_data());
	}

	git_reference_free(head);
	return result;
}

// A warning from the last operation that still succeeded (e.g. a pull whose local changes had
// to stay in a stash), or "".
String GitRepository::get_notice() const {
	return notice;
}

// How the last pull() went: { "commits": int (commits it brought in), "merged": bool (made a
// merge commit rather than fast-forwarding) }.
Dictionary GitRepository::get_pull_result() const {
	Dictionary result;
	result["commits"] = pulled_commits;
	result["merged"] = pull_merged;
	return result;
}

// p_callback(text: String, fraction: float, cancellable: bool) is called (deferred, on the main
// thread) while fetch/pull/push run. fraction is -1 when the total isn't known yet.
void GitRepository::set_progress_callback(const Callable &p_callback) {
	progress_callback = p_callback;
}

// When false, network operations only use saved logins and never open a sign-in window
// (for background work nobody explicitly asked for). Default true.
void GitRepository::set_login_prompts_allowed(bool p_allowed) {
	login_prompts_allowed = p_allowed;
}

// Asks the running fetch/pull/push (on any thread) to stop as soon as it can, including one that
// waits on a login window. It then fails with ERR_SKIP and "Canceled. Nothing was changed."
// The local part of a pull (updating files, merging) is quick and isn't interrupted.
void GitRepository::cancel_network() {
	cancel_network_operation();
}
