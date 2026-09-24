// GitRepository's network half: fetch, pull and push. They run on a worker thread in the dock,
// report progress through the progress callback, and can be canceled from any thread.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include "git/git_cli.h"
#include "git/git_lfs.h"
#include "git/git_remote_callbacks.h"
#include "git/git_util.h"

using namespace godot_git;

Error GitRepository::_fetch_remote(const String &p_remote) {
	RemotePtr remote;
	int err = git_remote_lookup(remote.out(), repo, p_remote.utf8().get_data());
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
	return finish_network_operation(ctx, git_remote_fetch(remote, nullptr, &opts, "fetch"));
}

// Before checking out p_commit on branch p_refname in a repository with LFS files: downloads the
// files it needs from the branch's remote, with progress and Cancel, so the checkout itself
// doesn't have to. Without a remote, the files must already be here.
Error GitRepository::_fetch_lfs_files(const char *p_refname, const git_oid *p_commit) {
	begin_network_operation();
	if (require_lfs(repo, "switching branches") != OK) {
		return FAILED;
	}
	git_buf remote_name = GIT_BUF_INIT;
	if (git_branch_upstream_remote(&remote_name, repo, p_refname) < 0) {
		git_buf_dispose(&remote_name);
		return OK;
	}
	RemoteContext ctx;
	ctx.workdir = get_workdir();
	ctx.login_prompts_allowed = login_prompts_allowed;
	ctx.progress = progress_callback;
	return lfs_fetch(repo, ctx, buf_to_string(remote_name), String(git_oid_tostr_s(p_commit)));
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

	ReferencePtr head;
	if (git_repository_head(head.out(), repo) == 0) {
		git_buf remote_name = GIT_BUF_INIT;
		if (git_branch_upstream_remote(&remote_name, repo, git_reference_name(head)) == 0) {
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
	ObjectPtr target;
	int err = git_object_lookup(target.out(), p_repo, target_oid, GIT_OBJECT_COMMIT);
	if (err >= 0) {
		git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
		opts.checkout_strategy = GIT_CHECKOUT_SAFE;
		opts.progress_cb = checkout_progress_cb;
		opts.progress_payload = &p_ctx;
		err = checkout_all_or_nothing(p_repo, target, opts);
		if (err == GIT_ECONFLICT) {
			return fail("Your local changes would be overwritten by the pull. Commit or discard them first.");
		}
	}
	if (err >= 0) {
		ReferencePtr moved;
		err = git_reference_set_target(moved.out(), p_head, target_oid, "pull: fast-forward");
	}
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
	CheckoutGuard guard; // git_merge's checkout can stop partway too (a file in use).
	guard.attach(checkout_opts);
	int err = git_merge(p_repo, &p_theirs, 1, &merge_opts, &checkout_opts);
	if (err < 0) {
		// Either git_merge refused up front (e.g. it would overwrite an untracked file) or its
		// checkout stopped partway: put back whatever it changed.
		const String error = last_git_error();
		ObjectPtr head_tree;
		git_revparse_single(head_tree.out(), p_repo, "HEAD^{tree}");
		const PackedStringArray not_restored = guard.restore(p_repo, (const git_tree *)head_tree.get());
		git_repository_state_cleanup(p_repo);
		return explain_checkout_failure(p_repo, error, not_restored);
	}

	IndexPtr index;
	err = git_repository_index(index.out(), p_repo);
	if (err >= 0 && git_index_has_conflicts(index)) {
		ObjectPtr head_commit;
		git_revparse_single(head_commit.out(), p_repo, "HEAD");
		git_reset(p_repo, head_commit, GIT_RESET_HARD, nullptr);
		git_repository_state_cleanup(p_repo);
		return fail("Pulling would cause merge conflicts, so nothing was changed. Resolving conflicts isn't supported in the panel yet; use git in a terminal for this one.");
	}

	const String message = vformat("Merge remote-tracking branch '%s'", String::utf8(git_reference_shorthand(p_upstream)));
	if (err >= 0 && commit_needs_git(p_repo, COMMIT_MERGE)) {
		// git sees the merge state git_merge left (MERGE_HEAD) and makes it a merge commit, running
		// the hooks and signing it. If they refuse, the merge is undone like a conflicting one.
		const Error commit_err = commit_with_git(p_repo, p_ctx, message, COMMIT_MERGE);
		if (commit_err != OK) {
			const String reason = GitRepository::get_last_error();
			ObjectPtr head_commit;
			git_revparse_single(head_commit.out(), p_repo, "HEAD");
			git_reset(p_repo, head_commit, GIT_RESET_HARD, nullptr);
			git_repository_state_cleanup(p_repo);
			return fail(vformat("Nothing was pulled: the merge commit didn't go through, so the merge was undone. %s", reason));
		}
		git_repository_state_cleanup(p_repo);
		return OK;
	}

	git_oid tree_oid, commit_oid;
	TreePtr tree;
	SignaturePtr signature;
	CommitPtr ours;
	CommitPtr their_commit;
	if (err >= 0) {
		err = git_index_write_tree(&tree_oid, index);
	}
	if (err >= 0) {
		err = git_tree_lookup(tree.out(), p_repo, &tree_oid);
	}
	if (err >= 0) {
		err = git_signature_default(signature.out(), p_repo);
	}
	if (err >= 0) {
		err = git_commit_lookup(ours.out(), p_repo, git_reference_target(p_head));
	}
	if (err >= 0) {
		err = git_commit_lookup(their_commit.out(), p_repo, git_annotated_commit_id(p_theirs));
	}
	if (err >= 0) {
		const git_commit *parents[2] = { ours, their_commit };
		err = git_commit_create(&commit_oid, p_repo, "HEAD", signature, signature, nullptr, message.utf8().get_data(), tree, 2, parents);
	}
	git_repository_state_cleanup(p_repo);
	return to_error(err);
}

// merge_and_commit, with uncommitted changes to tracked files (normal in Godot, which rewrites
// project.godot and scenes) set aside first and put back afterwards, like `git pull --autostash`.
// That's what lets a conflicting merge be undone completely. r_notice explains if the changes
// couldn't be put back (a safety net: paths_blocking_pull already rules that out).
Error merge_with_autostash(git_repository *p_repo, git_reference *p_head, git_reference *p_upstream, const git_annotated_commit *p_theirs, RemoteContext &p_ctx, String &r_notice) {
	git_status_options status_opts = GIT_STATUS_OPTIONS_INIT;
	status_opts.flags = 0; // Tracked files only; untracked files are left where they are.
	StatusListPtr status;
	if (git_status_list_new(status.out(), p_repo, &status_opts) != 0 || git_status_list_entrycount(status) == 0) {
		return merge_and_commit(p_repo, p_head, p_upstream, p_theirs, p_ctx);
	}

	git_oid stash_id;
	SignaturePtr stasher;
	int err = git_signature_default(stasher.out(), p_repo);
	if (err >= 0) {
		err = git_stash_save(&stash_id, p_repo, stasher, "godot-git: your changes, set aside while pulling", GIT_STASH_DEFAULT);
	}
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

	ReferencePtr head;
	if (head_branch(head.out(), repo) < 0) {
		return FAILED;
	}

	git_buf remote_buf = GIT_BUF_INIT;
	if (git_branch_upstream_remote(&remote_buf, repo, git_reference_name(head)) < 0) {
		git_buf_dispose(&remote_buf);
		return fail("This branch isn't tracking a remote branch yet. Push it first.");
	}
	const String remote_name = buf_to_string(remote_buf);
	const Error fetch_err = _fetch_remote(remote_name);
	if (fetch_err != OK) {
		return fetch_err;
	}

	ReferencePtr upstream;
	if (git_branch_upstream(upstream.out(), head) < 0) {
		return fail("The upstream branch doesn't exist on the remote anymore.");
	}
	AnnotatedCommitPtr theirs;
	git_merge_analysis_t analysis = GIT_MERGE_ANALYSIS_NONE;
	git_merge_preference_t preference = GIT_MERGE_PREFERENCE_NONE;
	int err = git_annotated_commit_from_ref(theirs.out(), repo, upstream);
	if (err >= 0) {
		const git_annotated_commit *heads[] = { theirs };
		err = git_merge_analysis(&analysis, &preference, repo, heads, 1);
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
	} else if (repo_uses_lfs(repo)) {
		// The new commits' LFS files, downloaded before any file changes (see _fetch_lfs_files).
		RemoteContext lfs_ctx;
		lfs_ctx.workdir = get_workdir();
		lfs_ctx.login_prompts_allowed = login_prompts_allowed;
		lfs_ctx.progress = progress_callback;
		result = lfs_fetch(repo, lfs_ctx, remote_name, String(git_oid_tostr_s(git_annotated_commit_id(theirs))));
	}
	if (err >= 0 && !(analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) && blocking.is_empty() && result == OK) {
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
	return result;
}

// Pushes the current branch to its upstream. A branch without one is published to "origin"
// (or the only remote) under the same name and starts tracking it.
Error GitRepository::push() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	begin_network_operation();

	ReferencePtr head;
	if (head_branch(head.out(), repo) < 0) {
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
			return fail("This repository has no remote to push to.");
		}
		remote_name = remotes.has("origin") ? String("origin") : remotes[0];
		remote_ref = local_ref;
		publish = true;
	}

	RemoteContext ctx;
	ctx.workdir = get_workdir();
	ctx.login_prompts_allowed = login_prompts_allowed;
	ctx.progress = progress_callback;

	// LFS files first: pushed commits must never point at files the remote doesn't have.
	const Error lfs_err = lfs_push(repo, ctx, remote_name, local_ref);
	if (lfs_err != OK) {
		return lfs_err;
	}

	const String refspec_text = vformat("%s:%s", local_ref, remote_ref);
	if (has_hook(repo, "pre-push")) {
		// libgit2 doesn't run hooks; git does.
		const Error result = _push_with_git(remote_name, refspec_text);
		if (result == OK && publish) {
			git_branch_set_upstream(head, vformat("%s/%s", remote_name, branch).utf8().get_data());
		}
		return result;
	}

	RemotePtr remote;
	int err = git_remote_lookup(remote.out(), repo, remote_name.utf8().get_data());
	if (err < 0) {
		return to_error(err);
	}
	git_push_options opts = GIT_PUSH_OPTIONS_INIT;
	set_remote_callbacks(opts.callbacks, ctx);

	report_progress(&ctx, "Connecting...", String(), -1);
	SinglePathspec refspec(refspec_text);
	err = git_remote_push(remote, &refspec.array, &opts);

	Error result = finish_network_operation(ctx, err);
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
	return result;
}

// `git push`, for repositories with a pre-push hook. git asks for logins itself, the same way.
Error GitRepository::_push_with_git(const String &p_remote, const String &p_refspec) {
	RemoteContext ctx;
	ctx.progress = progress_callback;
	PackedStringArray args;
	args.push_back("-c");
	args.push_back(vformat("credential.interactive=%s", login_prompts_allowed ? "always" : "never"));
	args.push_back("push");
	args.push_back("--progress");
	args.push_back(p_remote);
	args.push_back(p_refspec);
	String output;
	int exit_code = 0;
	const Error err = run_git_command(repo, ctx, args, "Pushing...", output, exit_code);
	if (err == ERR_SKIP) {
		git_error_set_str(GIT_ERROR_NET, "Canceled. Nothing was changed.");
		return ERR_SKIP;
	}
	if (err != OK) {
		return err;
	}
	if (exit_code == 0) {
		return OK;
	}
	if (output.contains("[rejected]") && (output.contains("fetch first") || output.contains("non-fast-forward"))) {
		return fail("The remote has commits you don't have yet. Pull first, then push.");
	}
	return fail(vformat("Git didn't push (a pre-push hook may have stopped it):\n%s", output_tail(output, 8)));
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
