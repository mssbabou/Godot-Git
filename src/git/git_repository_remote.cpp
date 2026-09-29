// GitRepository: fetch and push (pull is in git_repository_pull.cpp). They run on a worker thread
// in the dock, report progress through the progress callback, and can be canceled from any thread.

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
	if (git_remote_url(remote) && is_ssh_url(String::utf8(git_remote_url(remote)))) {
		if (require_git(vformat("%s is an SSH remote, and SSH goes through git.", p_remote)) != OK) {
			return FAILED;
		}
		return _fetch_with_git(p_remote);
	}

	RemoteContext ctx;
	ctx.workdir = get_workdir();
	ctx.login_prompts_allowed = login_prompts_allowed;
	ctx.progress = progress_callback;
	git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
	// Branches deleted on the remote disappear here too, instead of lingering in the branch picker
	// (and in the "is this commit pushed?" checks) forever. Local branches are never touched.
	opts.prune = GIT_FETCH_PRUNE;
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
	String push_url;
	{
		RemotePtr lookup;
		if (git_remote_lookup(lookup.out(), repo, remote_name.utf8().get_data()) == 0) {
			const char *url = git_remote_pushurl(lookup) ? git_remote_pushurl(lookup) : git_remote_url(lookup);
			push_url = url ? String::utf8(url) : String();
		}
	}
	const bool ssh = is_ssh_url(push_url);
	if (ssh && require_git(vformat("%s is an SSH remote, and SSH goes through git.", remote_name)) != OK) {
		return FAILED;
	}
	if (!ssh && has_hook(repo, "pre-push") && require_git("Pushing needs git here: this repository has a pre-push hook.") != OK) {
		return FAILED;
	}
	if (ssh || has_hook(repo, "pre-push")) {
		// libgit2 runs no hooks, and handles SSH badly (see is_ssh_url); git does both.
		const Error result = _push_with_git(remote_name, refspec_text, ssh);
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
Error GitRepository::_push_with_git(const String &p_remote, const String &p_refspec, bool p_ssh) {
	RemoteContext ctx;
	ctx.progress = progress_callback;
	PackedStringArray args = ssh_args(repo);
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
	if (p_ssh) {
		return network_failure(output, "push", true);
	}
	return fail(vformat("Git didn't push (a pre-push hook may have stopped it):\n%s", output_tail(output, 8)));
}

// `git fetch`, for SSH remotes (see is_ssh_url).
Error GitRepository::_fetch_with_git(const String &p_remote) {
	RemoteContext ctx;
	ctx.progress = progress_callback;
	PackedStringArray args = ssh_args(repo);
	args.push_back("fetch");
	args.push_back("--progress");
	args.push_back("--prune");
	args.push_back(p_remote);
	String output;
	int exit_code = 0;
	const Error err = run_git_command(repo, ctx, args, "Connecting...", output, exit_code);
	if (err == ERR_SKIP) {
		git_error_set_str(GIT_ERROR_NET, "Canceled. Nothing was changed.");
		return ERR_SKIP;
	}
	if (err != OK) {
		return err;
	}
	return exit_code == 0 ? OK : network_failure(output, "fetch", true);
}

// A warning from the last operation that still succeeded (e.g. a pull whose local changes had
// to stay in a stash), or "".
String GitRepository::get_notice() const {
	return notice;
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
