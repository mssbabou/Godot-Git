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

// The last p_count non-empty lines of p_output, for error messages.
String output_tail(const String &p_output, int p_count);

// Commits what's staged with `git commit` (hooks run, signing applies). For COMMIT_MERGE, the
// merge state libgit2's git_merge left behind (MERGE_HEAD) makes it a merge commit.
Error commit_with_git(git_repository *p_repo, RemoteContext &p_ctx, const String &p_message, CommitKind p_kind);

} // namespace godot_git
