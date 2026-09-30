// GitRepository: opening a repository and reading its state (status, line stats, branches, sync
// status, what it needs git for). The rest is split by area: git_repository_local.cpp (stage,
// commit, switch), _history, _diff, _remote (fetch, push), _pull, _operation, _setup.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include "git/git_cli.h"
#include "git/git_lfs.h"
#include "git/git_remote_callbacks.h"
#include "git/git_util.h"

using namespace godot_git;

namespace {

String index_status_name(unsigned int p_status) {
	if (p_status & GIT_STATUS_INDEX_NEW) {
		return "new";
	}
	if (p_status & GIT_STATUS_INDEX_MODIFIED) {
		return "modified";
	}
	if (p_status & GIT_STATUS_INDEX_DELETED) {
		return "deleted";
	}
	if (p_status & GIT_STATUS_INDEX_RENAMED) {
		return "renamed";
	}
	if (p_status & GIT_STATUS_INDEX_TYPECHANGE) {
		return "typechange";
	}
	return "";
}

String worktree_status_name(unsigned int p_status) {
	if (p_status & GIT_STATUS_WT_NEW) {
		return "untracked";
	}
	if (p_status & GIT_STATUS_WT_MODIFIED) {
		return "modified";
	}
	if (p_status & GIT_STATUS_WT_DELETED) {
		return "deleted";
	}
	if (p_status & GIT_STATUS_WT_RENAMED) {
		return "renamed";
	}
	if (p_status & GIT_STATUS_WT_TYPECHANGE) {
		return "typechange";
	}
	if (p_status & GIT_STATUS_CONFLICTED) {
		return "conflicted";
	}
	return "";
}

} // namespace

void GitRepository::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "path"), &GitRepository::open);
	ClassDB::bind_method(D_METHOD("is_open"), &GitRepository::is_open);
	ClassDB::bind_method(D_METHOD("get_workdir"), &GitRepository::get_workdir);
	ClassDB::bind_method(D_METHOD("get_current_branch"), &GitRepository::get_current_branch);
	ClassDB::bind_method(D_METHOD("get_branches"), &GitRepository::get_branches);
	ClassDB::bind_method(D_METHOD("get_remote_branches"), &GitRepository::get_remote_branches);
	ClassDB::bind_method(D_METHOD("get_remotes"), &GitRepository::get_remotes);
	ClassDB::bind_method(D_METHOD("get_sync_status"), &GitRepository::get_sync_status);
	ClassDB::bind_method(D_METHOD("get_status"), &GitRepository::get_status);
	ClassDB::bind_method(D_METHOD("get_line_stats", "staged"), &GitRepository::get_line_stats);
	ClassDB::bind_method(D_METHOD("get_diff", "path", "staged"), &GitRepository::get_diff);
	ClassDB::bind_method(D_METHOD("get_commit_files", "hash"), &GitRepository::get_commit_files);
	ClassDB::bind_method(D_METHOD("get_commit_diff", "hash", "path"), &GitRepository::get_commit_diff);
	ClassDB::bind_method(D_METHOD("get_file_bytes", "version", "path"), &GitRepository::get_file_bytes);
	ClassDB::bind_method(D_METHOD("get_commits", "max_count"), &GitRepository::get_commits, DEFVAL(50));
	ClassDB::bind_method(D_METHOD("uses_lfs"), &GitRepository::uses_lfs);
	ClassDB::bind_method(D_METHOD("has_file_at", "revision", "path"), &GitRepository::has_file_at);
	ClassDB::bind_method(D_METHOD("get_git_needs"), &GitRepository::get_git_needs);
	ClassDB::bind_method(D_METHOD("get_remote_url", "remote"), &GitRepository::get_remote_url);
	ClassDB::bind_method(D_METHOD("get_large_staged_files", "min_size"), &GitRepository::get_large_staged_files);
	ClassDB::bind_method(D_METHOD("get_pull_blockers"), &GitRepository::get_pull_blockers);
	ClassDB::bind_method(D_METHOD("get_stashes"), &GitRepository::get_stashes);
	ClassDB::bind_method(D_METHOD("get_stash_files", "hash"), &GitRepository::get_stash_files);
	ClassDB::bind_method(D_METHOD("get_stash_diff", "hash", "path"), &GitRepository::get_stash_diff);
	ClassDB::bind_method(D_METHOD("stash", "staged", "message"), &GitRepository::stash, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("restore_stash", "hash"), &GitRepository::restore_stash);
	ClassDB::bind_method(D_METHOD("delete_stash", "hash"), &GitRepository::delete_stash);
	ClassDB::bind_method(D_METHOD("get_operation"), &GitRepository::get_operation);

	ClassDB::bind_method(D_METHOD("stage", "path"), &GitRepository::stage);
	ClassDB::bind_method(D_METHOD("unstage", "path"), &GitRepository::unstage);
	ClassDB::bind_method(D_METHOD("stage_all"), &GitRepository::stage_all);
	ClassDB::bind_method(D_METHOD("unstage_all"), &GitRepository::unstage_all);
	ClassDB::bind_method(D_METHOD("discard", "path"), &GitRepository::discard);
	ClassDB::bind_method(D_METHOD("commit", "message"), &GitRepository::commit);
	ClassDB::bind_method(D_METHOD("amend", "message"), &GitRepository::amend);
	ClassDB::bind_method(D_METHOD("is_head_pushed"), &GitRepository::is_head_pushed);
	ClassDB::bind_method(D_METHOD("commit_runs_git", "amend"), &GitRepository::commit_runs_git);
	ClassDB::bind_method(D_METHOD("checkout_branch", "branch"), &GitRepository::checkout_branch);
	ClassDB::bind_method(D_METHOD("create_branch", "name"), &GitRepository::create_branch);
	ClassDB::bind_method(D_METHOD("rename_branch", "branch", "new_name"), &GitRepository::rename_branch);
	ClassDB::bind_method(D_METHOD("delete_branch", "branch"), &GitRepository::delete_branch);
	ClassDB::bind_method(D_METHOD("get_branch_details", "branch"), &GitRepository::get_branch_details);
	ClassDB::bind_method(D_METHOD("add_remote", "name", "url"), &GitRepository::add_remote);
	ClassDB::bind_method(D_METHOD("get_identity"), &GitRepository::get_identity);
	ClassDB::bind_method(D_METHOD("set_identity", "name", "email", "global"), &GitRepository::set_identity);

	ClassDB::bind_method(D_METHOD("fetch"), &GitRepository::fetch);
	ClassDB::bind_method(D_METHOD("pull"), &GitRepository::pull);
	ClassDB::bind_method(D_METHOD("push"), &GitRepository::push);
	ClassDB::bind_method(D_METHOD("abort_operation"), &GitRepository::abort_operation);
	ClassDB::bind_method(D_METHOD("continue_operation"), &GitRepository::continue_operation);
	ClassDB::bind_method(D_METHOD("get_notice"), &GitRepository::get_notice);
	ClassDB::bind_method(D_METHOD("get_pull_result"), &GitRepository::get_pull_result);
	ClassDB::bind_method(D_METHOD("set_progress_callback", "callback"), &GitRepository::set_progress_callback);
	ClassDB::bind_method(D_METHOD("set_login_prompts_allowed", "allowed"), &GitRepository::set_login_prompts_allowed);

	ClassDB::bind_static_method("GitRepository", D_METHOD("init_repository", "path", "project_path"), &GitRepository::init_repository);
	ClassDB::bind_static_method("GitRepository", D_METHOD("set_config_home", "path"), &GitRepository::set_config_home);
	ClassDB::bind_static_method("GitRepository", D_METHOD("cancel_network"), &GitRepository::cancel_network);

	ClassDB::bind_static_method("GitRepository", D_METHOD("get_last_error"), &GitRepository::get_last_error);
	ClassDB::bind_static_method("GitRepository", D_METHOD("get_libgit2_version"), &GitRepository::get_libgit2_version);
	ClassDB::bind_static_method("GitRepository", D_METHOD("diff_lines", "old", "new"), &GitRepository::diff_lines);
	ClassDB::bind_static_method("GitRepository", D_METHOD("is_git_installed"), &GitRepository::is_git_installed);
	ClassDB::bind_static_method("GitRepository", D_METHOD("check_git_installed"), &GitRepository::check_git_installed);
	ClassDB::bind_static_method("GitRepository", D_METHOD("set_git_program", "program"), &GitRepository::set_git_program);
}

GitRepository::~GitRepository() {
	close();
}

void GitRepository::close() {
	commits_key = String();
	commits_cache = Array();
	if (repo) {
		release_lfs(repo);
		git_repository_free(repo);
		repo = nullptr;
	}
}

// Opens the repository containing p_path, searching parent folders like the git CLI does.
// Accepts res:// and user:// paths as well as absolute OS paths.
Error GitRepository::open(const String &p_path) {
	close();

	const String path = ProjectSettings::get_singleton()->globalize_path(p_path);
	const int err = git_repository_open_ext(&repo, path.utf8().get_data(), 0, nullptr);
	if (err < 0) {
		repo = nullptr;
		return err == GIT_ENOTFOUND ? ERR_FILE_NOT_FOUND : FAILED;
	}
	return OK;
}

bool GitRepository::is_open() const {
	return repo != nullptr;
}

String GitRepository::get_workdir() const {
	ERR_FAIL_NULL_V_MSG(repo, String(), "Repository is not open.");
	const char *workdir = git_repository_workdir(repo);
	return workdir ? String::utf8(workdir) : String();
}

String GitRepository::get_current_branch() const {
	ERR_FAIL_NULL_V_MSG(repo, String(), "Repository is not open.");

	ReferencePtr head;
	const int err = git_repository_head(head.out(), repo);
	if (err == GIT_EUNBORNBRANCH) {
		// Fresh repo with no commits: HEAD points at a branch that doesn't exist yet.
		if (git_reference_lookup(head.out(), repo, "HEAD") == 0 && git_reference_symbolic_target(head)) {
			return String::utf8(git_reference_symbolic_target(head)).trim_prefix("refs/heads/");
		}
		return String();
	}
	if (err < 0) {
		return String();
	}

	// Detached HEAD yields "HEAD" here.
	return String::utf8(git_reference_shorthand(head));
}

// Returns an Array of Dictionaries: { "path": String, "index": String, "worktree": String }.
// "index" is the staged change, "worktree" the unstaged one; either may be "".
Array GitRepository::get_status() const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	git_status_options opts = GIT_STATUS_OPTIONS_INIT;
	opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
	opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS | GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX;

	StatusListPtr list;
	if (git_status_list_new(list.out(), repo, &opts) < 0) {
		ERR_FAIL_V_MSG(result, "git status failed: " + get_last_error());
	}

	const size_t count = git_status_list_entrycount(list);
	for (size_t i = 0; i < count; i++) {
		const git_status_entry *entry = git_status_byindex(list, i);
		if (entry->status == GIT_STATUS_CURRENT || (entry->status & GIT_STATUS_IGNORED)) {
			continue;
		}

		const git_diff_delta *delta = entry->head_to_index ? entry->head_to_index : entry->index_to_workdir;
		const char *path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;

		Dictionary item;
		item["path"] = String::utf8(path);
		item["index"] = index_status_name(entry->status);
		item["worktree"] = worktree_status_name(entry->status);
		result.push_back(item);
	}
	return result;
}

// Lines added/removed per changed file: { path: Vector2i(added, removed) }.
// Binary (and very large) files report Vector2i(-1, -1).
// p_staged: HEAD vs index (what the next commit contains); otherwise index vs working tree.
Dictionary GitRepository::get_line_stats(bool p_staged) const {
	Dictionary result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	opts.max_size = 2 * 1024 * 1024; // Bigger files are treated as binary; counting them isn't worth the time.

	DiffPtr diff;
	int err = 0;
	if (p_staged) {
		ObjectPtr head_tree;
		git_revparse_single(head_tree.out(), repo, "HEAD^{tree}"); // Stays null before the first commit.
		err = git_diff_tree_to_index(diff.out(), repo, (git_tree *)head_tree.get(), nullptr, &opts);
		if (err >= 0) {
			git_diff_find_similar(diff, nullptr);
		}
	} else {
		opts.flags = GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS | GIT_DIFF_SHOW_UNTRACKED_CONTENT;
		err = git_diff_index_to_workdir(diff.out(), repo, nullptr, &opts);
	}
	if (err < 0) {
		return result;
	}

	// Checked per file only in repositories that use LFS: an attribute lookup costs about half a
	// millisecond per file on Windows, a second for 2,000 changed files.
	const bool uses_lfs = repo_uses_lfs(repo);
	const size_t count = git_diff_num_deltas(diff);
	for (size_t i = 0; i < count; i++) {
		// An LFS file's diff would be of its pointer text, which says nothing about the real file,
		// and computing it runs the whole file through git-lfs.
		const git_diff_delta *lfs_delta = git_diff_get_delta(diff, i);
		const char *lfs_path = lfs_delta->new_file.path ? lfs_delta->new_file.path : lfs_delta->old_file.path;
		if (uses_lfs && is_lfs_path(repo, lfs_path)) {
			result[String::utf8(lfs_path)] = Vector2i(-1, -1);
			continue;
		}

		PatchPtr patch;
		if (git_patch_from_diff(patch.out(), diff, i) < 0 || !patch) {
			continue;
		}
		const git_diff_delta *delta = git_patch_get_delta(patch);
		const char *path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;

		size_t added = 0, removed = 0;
		if (delta->flags & GIT_DIFF_FLAG_BINARY) {
			result[String::utf8(path)] = Vector2i(-1, -1);
		} else if (git_patch_line_stats(nullptr, &added, &removed, patch) == 0) {
			result[String::utf8(path)] = Vector2i((int32_t)added, (int32_t)removed);
		}
	}
	return result;
}

// Whether some files are stored with Git LFS. Switching branches can then download files, so the
// dock runs it in the background.
bool GitRepository::uses_lfs() const {
	ERR_FAIL_NULL_V_MSG(repo, false, "Repository is not open.");
	return repo_uses_lfs(repo);
}

// Whether a commit ("HEAD", "feature", "origin/feature") contains a file.
bool GitRepository::has_file_at(const String &p_revision, const String &p_path) const {
	ERR_FAIL_NULL_V_MSG(repo, false, "Repository is not open.");
	ObjectPtr blob;
	return git_revparse_single(blob.out(), repo, vformat("%s:%s", p_revision, p_path).utf8().get_data()) == 0;
}

// What in this repository only works through the git program (see git_cli.h), so the dock can
// say up front what won't work without it: { "commit": why committing needs git, or "";
// "pre_push": bool; "lfs": bool; "ssh_remotes", "https_remotes": remote names (HTTPS logins
// come from git's credential helper) }.
Dictionary GitRepository::get_git_needs() const {
	Dictionary result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	result["commit"] = commit_git_reason(repo, COMMIT_NEW);
	result["pre_push"] = has_hook(repo, "pre-push");
	result["lfs"] = repo_uses_lfs(repo);
	PackedStringArray ssh;
	PackedStringArray https;
	for (const String &name : get_remotes()) {
		RemotePtr remote;
		if (git_remote_lookup(remote.out(), repo, name.utf8().get_data()) < 0 || !git_remote_url(remote)) {
			continue;
		}
		const String url = String::utf8(git_remote_url(remote));
		if (is_ssh_url(url)) {
			ssh.push_back(name);
		} else if (url.begins_with("http://") || url.begins_with("https://")) {
			https.push_back(name);
		}
	}
	result["ssh_remotes"] = ssh;
	result["https_remotes"] = https;
	return result;
}

// The URL p_remote fetches from, or "" if there's no such remote.
String GitRepository::get_remote_url(const String &p_remote) const {
	RemotePtr remote;
	if (!repo || git_remote_lookup(remote.out(), repo, p_remote.utf8().get_data()) < 0 || !git_remote_url(remote)) {
		return String();
	}
	return String::utf8(git_remote_url(remote));
}

// Staged files (new or changed) of at least p_min_size bytes, as [{path, size}], largest first.
// Sizes are of what's staged: a Git LFS file is only its small pointer, so it never counts.
Array GitRepository::get_large_staged_files(int64_t p_min_size) const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	ObjectPtr head_tree; // Null before the first commit: everything staged is new.
	git_revparse_single(head_tree.out(), repo, "HEAD^{tree}");
	DiffPtr diff;
	OdbPtr odb;
	if (git_diff_tree_to_index(diff.out(), repo, (git_tree *)head_tree.get(), nullptr, nullptr) < 0 || git_repository_odb(odb.out(), repo) < 0) {
		return result;
	}
	for (size_t i = 0; i < git_diff_num_deltas(diff); i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);
		if (delta->status == GIT_DELTA_DELETED) {
			continue;
		}
		size_t size = 0;
		git_object_t type;
		if (git_odb_read_header(&size, &type, odb, &delta->new_file.id) == 0 && (int64_t)size >= p_min_size) {
			Dictionary file;
			file["path"] = String::utf8(delta->new_file.path);
			file["size"] = (int64_t)size;
			int at = 0;
			while (at < result.size() && (int64_t)Dictionary(result[at])["size"] >= (int64_t)size) {
				at++;
			}
			result.insert(at, file);
		}
	}
	return result;
}

PackedStringArray GitRepository::get_branches() const {
	PackedStringArray branches;
	ERR_FAIL_NULL_V_MSG(repo, branches, "Repository is not open.");

	BranchIteratorPtr it;
	if (git_branch_iterator_new(it.out(), repo, GIT_BRANCH_LOCAL) < 0) {
		return branches;
	}

	ReferencePtr ref;
	git_branch_t type;
	while (git_branch_next(ref.out(), &type, it) == 0) {
		const char *name = nullptr;
		if (git_branch_name(&name, ref) == 0) {
			branches.push_back(String::utf8(name));
		}
	}
	return branches;
}

// Remote-tracking branches as "remote/branch" (e.g. "origin/main"), without the "origin/HEAD" alias.
PackedStringArray GitRepository::get_remote_branches() const {
	PackedStringArray branches;
	ERR_FAIL_NULL_V_MSG(repo, branches, "Repository is not open.");

	BranchIteratorPtr it;
	if (git_branch_iterator_new(it.out(), repo, GIT_BRANCH_REMOTE) < 0) {
		return branches;
	}

	ReferencePtr ref;
	git_branch_t type;
	while (git_branch_next(ref.out(), &type, it) == 0) {
		const char *name = nullptr;
		if (git_reference_type(ref) == GIT_REFERENCE_DIRECT && git_branch_name(&name, ref) == 0) {
			branches.push_back(String::utf8(name));
		}
	}
	return branches;
}

PackedStringArray GitRepository::get_remotes() const {
	PackedStringArray remotes;
	ERR_FAIL_NULL_V_MSG(repo, remotes, "Repository is not open.");

	git_strarray list = { nullptr, 0 };
	if (git_remote_list(&list, repo) == 0) {
		for (size_t i = 0; i < list.count; i++) {
			remotes.push_back(String::utf8(list.strings[i]));
		}
		git_strarray_dispose(&list);
	}
	return remotes;
}

// How the current branch relates to its upstream:
// { "branch": String, "upstream": String ("" if none), "ahead": int, "behind": int, "has_remotes": bool,
// "head": HEAD's commit id ("" without commits) }
Dictionary GitRepository::get_sync_status() const {
	Dictionary result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	result["branch"] = get_current_branch();
	result["upstream"] = String();
	result["ahead"] = 0;
	result["behind"] = 0;
	result["has_remotes"] = !get_remotes().is_empty();
	// Every fetch (ours or the git CLI's) rewrites FETCH_HEAD, so its age says how fresh the
	// ahead/behind counts are. 0 if never fetched.
	const String fetch_head = String::utf8(git_repository_commondir(repo)).path_join("FETCH_HEAD");
	result["last_fetched"] = FileAccess::file_exists(fetch_head) ? (int64_t)FileAccess::get_modified_time(fetch_head) : (int64_t)0;

	git_oid head_id;
	result["head"] = git_reference_name_to_id(&head_id, repo, "HEAD") == 0 ? String(git_oid_tostr_s(&head_id)) : String();
	git_error_clear(); // No commits yet.

	ReferencePtr head;
	if (git_repository_head(head.out(), repo) < 0) {
		return result;
	}
	ReferencePtr upstream;
	if (git_reference_is_branch(head) && git_branch_upstream(upstream.out(), head) == 0) {
		result["upstream"] = String::utf8(git_reference_shorthand(upstream));

		const git_oid *local_oid = git_reference_target(head);
		const git_oid *upstream_oid = git_reference_target(upstream);
		size_t ahead = 0, behind = 0;
		if (local_oid && upstream_oid && git_graph_ahead_behind(&ahead, &behind, repo, local_oid, upstream_oid) == 0) {
			result["ahead"] = (int64_t)ahead;
			result["behind"] = (int64_t)behind;
		}
	}
	return result;
}

String GitRepository::get_last_error() {
	// Since libgit2 1.8 this is never null; "no error" comes back with class GIT_ERROR_NONE.
	const git_error *err = git_error_last();
	return (err && err->klass != GIT_ERROR_NONE && err->message) ? String::utf8(err->message) : String();
}

// Whether git itself is installed (checked once; see git_installed()).
bool GitRepository::is_git_installed() {
	return git_installed();
}

// Checks again whether git is installed, e.g. when the editor gets focus back.
bool GitRepository::check_git_installed() {
	return check_git();
}

// For the tests: run p_program instead of "git", e.g. one that doesn't exist.
void GitRepository::set_git_program(const String &p_program) {
	godot_git::set_git_program(p_program);
}

String GitRepository::get_libgit2_version() {
	int major = 0, minor = 0, rev = 0;
	git_libgit2_version(&major, &minor, &rev);
	return vformat("%d.%d.%d", major, minor, rev);
}
