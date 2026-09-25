#pragma once

// Git LFS support. libgit2 knows nothing about LFS: on its own it checks out the small pointer
// files LFS stores in git instead of the real files, and commits whole files into git instead of
// LFS. This registers a libgit2 filter for `filter=lfs` that hands file contents to the real
// git-lfs program (the way git itself does), and wraps `git lfs fetch` / `git lfs push`, which
// move the files themselves. Internal to the GitRepository sources.

#include <git2.h>

#include <godot_cpp/variant/string.hpp>

#include "git/git_remote_callbacks.h"

using namespace godot;

namespace godot_git {

// Registers the "lfs" filter with libgit2. Call once, after git_libgit2_init().
void register_lfs_filter();

// Stops every git-lfs filter process still running. Call before git_libgit2_shutdown().
void shutdown_lfs();

// Stops p_repo's git-lfs filter process, if it has one. Call before freeing the repository.
void release_lfs(git_repository *p_repo);

// Whether git-lfs works on this machine. Checked once, and again after check_git().
bool lfs_installed();
void forget_lfs_check();

// Whether the repository's .gitattributes sends files through Git LFS.
bool repo_uses_lfs(git_repository *p_repo);

// FAILED, with a message saying so, if the repository uses LFS but git-lfs isn't installed:
// checking out or committing would then silently do the wrong thing. OK otherwise.
Error require_lfs(git_repository *p_repo, const String &p_action);

// Downloads the LFS files commit p_commit needs from p_remote, so checking it out doesn't have
// to (with progress, and cancellable). Does nothing for a repository without LFS.
Error lfs_fetch(git_repository *p_repo, RemoteContext &p_ctx, const String &p_remote, const String &p_commit);

// Uploads the LFS files p_ref's commits need to p_remote. Run before pushing the commits: the
// remote would otherwise have commits pointing at files it doesn't have.
Error lfs_push(git_repository *p_repo, RemoteContext &p_ctx, const String &p_remote, const String &p_ref);

// Whether p_path is stored in LFS (its filter attribute is "lfs").
bool is_lfs_path(git_repository *p_repo, const char *p_path);

} // namespace godot_git
