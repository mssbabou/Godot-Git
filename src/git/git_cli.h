#pragma once

// Steps handed to the git command line because libgit2 would silently skip part of them: it
// runs no hooks (pre-commit, commit-msg, pre-push, ...) and never signs commits. When a
// repository uses neither, GitRepository does the step with libgit2 as usual. Internal to the
// GitRepository sources.

#include <git2.h>

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "git/git_remote_callbacks.h"

using namespace godot;

namespace godot_git {

enum CommitKind {
	COMMIT_NEW,
	COMMIT_AMEND,
	COMMIT_MERGE,
};

// Whether the repository has the hook p_name (core.hooksPath, or .git/hooks).
bool has_hook(git_repository *p_repo, const char *p_name);

// Whether making this kind of commit needs git itself: signing is on, or a hook would run.
bool commit_needs_git(git_repository *p_repo, CommitKind p_kind);

// Runs `git <p_args>` in the repository and waits for it. Each line it prints is shown as
// progress (git's "Writing objects: 45% (9/20)" becomes the step and a fraction; anything else,
// such as a hook's output, is shown under p_step), and all of it is returned in r_output.
// Cancellable: ERR_SKIP with "Canceled." Otherwise OK, with git's exit code in r_exit_code.
Error run_git_command(git_repository *p_repo, RemoteContext &p_ctx, const PackedStringArray &p_args, const String &p_step, String &r_output, int &r_exit_code);

// Whether p_url is an SSH remote ("ssh://host/repo" or scp-like "git@host:repo"). Fetch and push
// for those go through git: libgit2 can't pass options to ssh on Windows, discards ssh's error
// output, and on Windows would start a different ssh than git does.
bool is_ssh_url(const String &p_url);

// Arguments for git that make ssh fail instead of waiting on a prompt nobody can see (there's no
// terminal): -c core.sshCommand="ssh -o BatchMode=yes". Empty if the user set their own ssh
// command (GIT_SSH_COMMAND, GIT_SSH or core.sshCommand), which is then used as it is.
PackedStringArray ssh_args(git_repository *p_repo);

// An error for a failed `git fetch`/`git push` from its output, explaining the usual SSH failures
// (no key, unknown host, can't connect). p_action: "fetch", "push".
Error network_failure(const String &p_output, const String &p_action, bool p_ssh);

// The last p_count non-empty lines of p_output, for error messages.
String output_tail(const String &p_output, int p_count);

// Commits what's staged with `git commit` (hooks run, signing applies). For COMMIT_MERGE, the
// merge state libgit2's git_merge left behind (MERGE_HEAD) makes it a merge commit.
Error commit_with_git(git_repository *p_repo, RemoteContext &p_ctx, const String &p_message, CommitKind p_kind);

} // namespace godot_git
