// GitRepository: opening a repository, reading its state (status, line stats, branches,
// history) and local changes (stage, discard, commit, branches). The network half (fetch, pull,
// push) is in git_repository_remote.cpp.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/local_vector.hpp>
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
	ClassDB::bind_method(D_METHOD("get_commits", "max_count"), &GitRepository::get_commits, DEFVAL(50));
	ClassDB::bind_method(D_METHOD("uses_lfs"), &GitRepository::uses_lfs);
	ClassDB::bind_method(D_METHOD("has_file_at", "revision", "path"), &GitRepository::has_file_at);
	ClassDB::bind_method(D_METHOD("get_git_needs"), &GitRepository::get_git_needs);

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
	ClassDB::bind_method(D_METHOD("add_remote", "name", "url"), &GitRepository::add_remote);
	ClassDB::bind_method(D_METHOD("get_identity"), &GitRepository::get_identity);
	ClassDB::bind_method(D_METHOD("set_identity", "name", "email", "global"), &GitRepository::set_identity);

	ClassDB::bind_method(D_METHOD("fetch"), &GitRepository::fetch);
	ClassDB::bind_method(D_METHOD("pull"), &GitRepository::pull);
	ClassDB::bind_method(D_METHOD("push"), &GitRepository::push);
	ClassDB::bind_method(D_METHOD("get_notice"), &GitRepository::get_notice);
	ClassDB::bind_method(D_METHOD("get_pull_result"), &GitRepository::get_pull_result);
	ClassDB::bind_method(D_METHOD("set_progress_callback", "callback"), &GitRepository::set_progress_callback);
	ClassDB::bind_method(D_METHOD("set_login_prompts_allowed", "allowed"), &GitRepository::set_login_prompts_allowed);

	ClassDB::bind_static_method("GitRepository", D_METHOD("init_repository", "path", "project_path"), &GitRepository::init_repository);
	ClassDB::bind_static_method("GitRepository", D_METHOD("set_config_home", "path"), &GitRepository::set_config_home);
	ClassDB::bind_static_method("GitRepository", D_METHOD("cancel_network"), &GitRepository::cancel_network);

	ClassDB::bind_static_method("GitRepository", D_METHOD("get_last_error"), &GitRepository::get_last_error);
	ClassDB::bind_static_method("GitRepository", D_METHOD("get_libgit2_version"), &GitRepository::get_libgit2_version);
	ClassDB::bind_static_method("GitRepository", D_METHOD("is_git_installed"), &GitRepository::is_git_installed);
	ClassDB::bind_static_method("GitRepository", D_METHOD("check_git_installed"), &GitRepository::check_git_installed);
	ClassDB::bind_static_method("GitRepository", D_METHOD("set_git_program", "program"), &GitRepository::set_git_program);
}

GitRepository::~GitRepository() {
	close();
}

void GitRepository::close() {
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

	const size_t count = git_diff_num_deltas(diff);
	for (size_t i = 0; i < count; i++) {
		// An LFS file's diff would be of its pointer text, which says nothing about the real file,
		// and computing it runs the whole file through git-lfs.
		const git_diff_delta *lfs_delta = git_diff_get_delta(diff, i);
		const char *lfs_path = lfs_delta->new_file.path ? lfs_delta->new_file.path : lfs_delta->old_file.path;
		if (is_lfs_path(repo, lfs_path)) {
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
// { "branch": String, "upstream": String ("" if none), "ahead": int, "behind": int, "has_remotes": bool }
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

// Returns up to p_max_count commits reachable from HEAD, newest first:
// [{ "id": String (short hash), "hash": String, "summary": String, "message": String,
//    "author": String, "time": int (unix), "unpushed": bool }, ...]
// "unpushed" means the commit isn't on any remote-tracking branch yet.
Array GitRepository::get_commits(int p_max_count) const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	HashSet<String> unpushed;
	if (!get_remotes().is_empty()) {
		RevwalkPtr walk;
		if (git_revwalk_new(walk.out(), repo) == 0 && git_revwalk_push_head(walk) == 0) {
			git_revwalk_hide_glob(walk, "refs/remotes/*");
			git_oid oid;
			// Capped so a huge never-pushed branch can't stall the panel.
			while (unpushed.size() < 1000 && git_revwalk_next(&oid, walk) == 0) {
				unpushed.insert(String(git_oid_tostr_s(&oid)));
			}
		}
	}

	RevwalkPtr walk;
	if (git_revwalk_new(walk.out(), repo) < 0) {
		return result;
	}
	git_revwalk_sorting(walk, GIT_SORT_TIME);
	if (git_revwalk_push_head(walk) < 0) {
		// No commits yet.
		return result;
	}

	git_oid oid;
	while (result.size() < p_max_count && git_revwalk_next(&oid, walk) == 0) {
		CommitPtr commit;
		if (git_commit_lookup(commit.out(), repo, &oid) < 0) {
			continue;
		}

		const String hash = String(git_oid_tostr_s(&oid));
		const git_signature *author = git_commit_author(commit);
		const char *summary = git_commit_summary(commit);
		const char *message = git_commit_message(commit);

		Dictionary item;
		item["id"] = hash.substr(0, 7);
		item["hash"] = hash;
		item["summary"] = summary ? String::utf8(summary) : String();
		item["message"] = message ? String::utf8(message).strip_edges() : String();
		item["author"] = author ? String::utf8(author->name) : String();
		item["time"] = (int64_t)git_commit_time(commit);
		item["unpushed"] = unpushed.has(hash);
		result.push_back(item);
	}
	return result;
}

Error GitRepository::stage(const String &p_path) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");

	git_error_clear();
	if (require_lfs(repo, "staging") != OK) {
		return FAILED;
	}
	const CharString path = p_path.utf8();
	unsigned int flags = 0;
	int err = git_status_file(&flags, repo, path.get_data());
	if (err < 0) {
		return to_error(err);
	}

	IndexPtr index;
	err = git_repository_index(index.out(), repo);
	if (err < 0) {
		return to_error(err);
	}

	// A file deleted from disk is staged by removing it from the index.
	if (flags & GIT_STATUS_WT_DELETED) {
		err = git_index_remove_bypath(index, path.get_data());
	} else {
		err = git_index_add_bypath(index, path.get_data());
	}
	if (err >= 0) {
		err = git_index_write(index);
	}
	return to_error(err);
}

Error GitRepository::unstage(const String &p_path) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");

	// With no commits yet there's no HEAD; a null target just drops the path from the index.
	ObjectPtr head;
	git_revparse_single(head.out(), repo, "HEAD");

	SinglePathspec pathspec(p_path);
	return to_error(git_reset_default(repo, head, &pathspec.array));
}

Error GitRepository::stage_all() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	if (require_lfs(repo, "staging") != OK) {
		return FAILED;
	}

	IndexPtr index;
	int err = git_repository_index(index.out(), repo);
	if (err < 0) {
		return to_error(err);
	}

	// add_all picks up new and modified files, update_all picks up deletions.
	const git_strarray everything = { nullptr, 0 };
	err = git_index_add_all(index, &everything, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr);
	if (err >= 0) {
		err = git_index_update_all(index, &everything, nullptr, nullptr);
	}
	if (err >= 0) {
		err = git_index_write(index);
	}
	return to_error(err);
}

Error GitRepository::unstage_all() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");

	// Like `git reset`: the index goes back to HEAD, files on disk are untouched.
	// (git_reset_default can't do "all paths"; it requires a non-empty pathspec.)
	ObjectPtr head;
	if (git_revparse_single(head.out(), repo, "HEAD^{commit}") == 0) {
		return to_error(git_reset(repo, head, GIT_RESET_MIXED, nullptr));
	}

	// No commits yet: everything staged is new, so unstaging all means an empty index.
	IndexPtr index;
	int err = git_repository_index(index.out(), repo);
	if (err >= 0) {
		err = git_index_clear(index);
	}
	if (err >= 0) {
		err = git_index_write(index);
	}
	return to_error(err);
}

// Throws away unstaged changes to p_path (restores it from the index).
// Untracked files are deleted. This can't be undone.
Error GitRepository::discard(const String &p_path) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");

	unsigned int flags = 0;
	int err = git_status_file(&flags, repo, p_path.utf8().get_data());
	if (err < 0) {
		return to_error(err);
	}

	if (flags & GIT_STATUS_WT_NEW) {
		return DirAccess::remove_absolute(get_workdir().path_join(p_path));
	}

	SinglePathspec pathspec(p_path);
	git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
	opts.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;
	opts.paths = pathspec.array;
	err = git_checkout_index(repo, nullptr, &opts);
	return to_error(err);
}

// Commits what's staged. Author/committer come from git config (user.name / user.email).
Error GitRepository::commit(const String &p_message) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");

	git_error_clear();
	if (require_lfs(repo, "committing") != OK || require_identity(repo, "Nothing was committed.") != OK) {
		return FAILED;
	}
	if (commit_needs_git(repo, COMMIT_NEW)) {
		return _commit_with_git(p_message, false);
	}
	git_oid oid;
	return to_error(git_commit_create_from_stage(&oid, repo, p_message.utf8().get_data(), nullptr));
}

// Whether HEAD's commit is on any remote-tracking branch, i.e. already pushed (or fetched).
bool GitRepository::is_head_pushed() const {
	ERR_FAIL_NULL_V_MSG(repo, false, "Repository is not open.");
	git_oid head;
	if (git_reference_name_to_id(&head, repo, "HEAD") < 0) {
		return false;
	}
	LocalVector<git_oid> remote_tips;
	BranchIteratorPtr it;
	if (git_branch_iterator_new(it.out(), repo, GIT_BRANCH_REMOTE) == 0) {
		ReferencePtr ref;
		git_branch_t type;
		while (git_branch_next(ref.out(), &type, it) == 0) {
			if (git_reference_type(ref) == GIT_REFERENCE_DIRECT) {
				remote_tips.push_back(*git_reference_target(ref));
			}
		}
	}
	return !remote_tips.is_empty() && git_graph_reachable_from_any(repo, &head, remote_tips.ptr(), remote_tips.size()) == 1;
}

// Replaces the last commit with one that has p_message and what's staged now (which may be
// nothing: then only the message changes). Refused once the commit is pushed: rewriting it
// would leave teammates with a commit that no longer exists here.
Error GitRepository::amend(const String &p_message) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	if (require_lfs(repo, "committing") != OK || require_identity(repo, "The commit wasn't amended.") != OK) {
		return FAILED;
	}
	ReferencePtr head;
	if (head_branch(head.out(), repo) < 0) {
		return FAILED;
	}
	if (is_head_pushed()) {
		return fail("The last commit is already pushed, so amending it would change history your teammates may have. Make a new commit instead.");
	}
	if (commit_needs_git(repo, COMMIT_AMEND)) {
		return _commit_with_git(p_message, true);
	}

	CommitPtr last;
	IndexPtr index;
	git_oid tree_id;
	TreePtr tree;
	SignaturePtr committer;
	int err = git_commit_lookup(last.out(), repo, git_reference_target(head));
	if (err >= 0) {
		err = git_repository_index(index.out(), repo);
	}
	if (err >= 0) {
		err = git_index_write_tree(&tree_id, index);
	}
	if (err >= 0) {
		err = git_tree_lookup(tree.out(), repo, &tree_id);
	}
	if (err >= 0) {
		// Like `git commit --amend`: the original author stays, the committer is you, now.
		err = git_signature_default(committer.out(), repo);
	}
	if (err >= 0) {
		git_oid id;
		err = git_commit_amend(&id, last, "HEAD", nullptr, committer, nullptr, p_message.utf8().get_data(), tree);
	}
	return to_error(err);
}

// Whether committing (or amending) goes through the git command line, because hooks would run
// or commits get signed. That can take a while (a hook may run a linter), so the dock does it in
// the background.
bool GitRepository::commit_runs_git(bool p_amend) const {
	ERR_FAIL_NULL_V_MSG(repo, false, "Repository is not open.");
	return commit_needs_git(repo, p_amend ? COMMIT_AMEND : COMMIT_NEW);
}

Error GitRepository::_commit_with_git(const String &p_message, bool p_amend) {
	begin_network_operation();
	RemoteContext ctx;
	ctx.progress = progress_callback;
	return commit_with_git(repo, ctx, p_message, p_amend ? COMMIT_AMEND : COMMIT_NEW);
}

// Switches to a branch. Accepts a local branch ("feature") or a remote-tracking one
// ("origin/feature"); for the latter a local branch tracking it is created if needed.
// Refuses (and changes nothing) if local changes would be overwritten.
Error GitRepository::checkout_branch(const String &p_branch) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();

	ReferencePtr ref;
	int err = git_branch_lookup(ref.out(), repo, p_branch.utf8().get_data(), GIT_BRANCH_LOCAL);
	if (err == GIT_ENOTFOUND) {
		ReferencePtr remote_ref;
		if (git_branch_lookup(remote_ref.out(), repo, p_branch.utf8().get_data(), GIT_BRANCH_REMOTE) < 0) {
			return fail(vformat("There is no branch called \"%s\".", p_branch));
		}
		// "origin/feature" -> "feature".
		const int slash = p_branch.find("/");
		const String local_name = slash >= 0 ? p_branch.substr(slash + 1) : p_branch;

		// A local branch with that name may already exist; use it rather than failing.
		err = git_branch_lookup(ref.out(), repo, local_name.utf8().get_data(), GIT_BRANCH_LOCAL);
		if (err == GIT_ENOTFOUND) {
			CommitPtr commit;
			err = git_commit_lookup(commit.out(), repo, git_reference_target(remote_ref));
			if (err >= 0) {
				err = git_branch_create(ref.out(), repo, local_name.utf8().get_data(), commit, 0);
			}
			if (err >= 0) {
				git_branch_set_upstream(ref, p_branch.utf8().get_data());
			}
		}
	}
	if (err < 0) {
		return to_error(err);
	}

	ObjectPtr target;
	err = git_reference_peel(target.out(), ref, GIT_OBJECT_COMMIT);
	if (err >= 0 && repo_uses_lfs(repo)) {
		const Error lfs_err = _fetch_lfs_files(git_reference_name(ref), git_object_id(target));
		if (lfs_err != OK) {
			return lfs_err;
		}
	}
	if (err >= 0) {
		git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
		opts.checkout_strategy = GIT_CHECKOUT_SAFE;
		err = checkout_all_or_nothing(repo, target, opts);
		if (err == GIT_ECONFLICT) {
			fail("Your local changes would be overwritten by switching branches. Commit or discard them first.");
		}
	}
	if (err >= 0) {
		err = git_repository_set_head(repo, git_reference_name(ref));
	}
	return to_error(err);
}

// Creates a branch at the current commit and switches to it.
Error GitRepository::create_branch(const String &p_name) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();

	int valid = 0;
	if (git_branch_name_is_valid(&valid, p_name.utf8().get_data()) < 0 || !valid) {
		return fail(vformat("\"%s\" isn't a valid branch name.", p_name));
	}

	ObjectPtr head;
	if (git_revparse_single(head.out(), repo, "HEAD^{commit}") < 0) {
		return fail("Make a first commit before creating branches.");
	}
	ReferencePtr ref;
	const int err = git_branch_create(ref.out(), repo, p_name.utf8().get_data(), (git_commit *)head.get(), 0);
	if (err == GIT_EEXISTS) {
		return fail(vformat("A branch called \"%s\" already exists.", p_name));
	}
	if (err < 0) {
		return to_error(err);
	}
	return to_error(git_repository_set_head(repo, git_reference_name(ref)));
}

// Adds a remote (e.g. "origin") to push to and pull from. Nothing is contacted yet.
Error GitRepository::add_remote(const String &p_name, const String &p_url) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	const String url = p_url.strip_edges();
	if (url.is_empty()) {
		return fail("Paste the repository's URL first.");
	}
	int valid = 0;
	if (git_remote_name_is_valid(&valid, p_name.utf8().get_data()) < 0 || !valid) {
		return fail(vformat("\"%s\" isn't a valid remote name.", p_name));
	}
	RemotePtr remote;
	const int err = git_remote_create(remote.out(), repo, p_name.utf8().get_data(), url.utf8().get_data());
	if (err == GIT_EEXISTS) {
		return fail(vformat("This repository already has a remote called \"%s\".", p_name));
	}
	return to_error(err);
}

// The name and email commits are made with: { "name", "email" }, "" where git has none.
Dictionary GitRepository::get_identity() const {
	Dictionary result;
	result["name"] = String();
	result["email"] = String();
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	ConfigPtr config;
	if (git_repository_config_snapshot(config.out(), repo) < 0) {
		return result;
	}
	for (const char *key : { "name", "email" }) {
		git_buf value = GIT_BUF_INIT;
		if (git_config_get_string_buf(&value, config, vformat("user.%s", key).utf8().get_data()) == 0) {
			result[key] = buf_to_string(value).strip_edges();
		} else {
			git_buf_dispose(&value);
		}
	}
	return result;
}

// Sets the name and email commits are made with, like `git config [--global] user.name`: for
// every repository (the user's global git config, shared with the git CLI and other tools), or
// only for this one.
Error GitRepository::set_identity(const String &p_name, const String &p_email, bool p_global) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	const String name = p_name.strip_edges();
	const String email = p_email.strip_edges();
	if (name.is_empty() || email.is_empty()) {
		return fail("Fill in both your name and your email.");
	}
	ConfigPtr config;
	int err = 0;
	if (p_global) {
		String path;
		git_buf found = GIT_BUF_INIT;
		if (git_config_find_global(&found) == 0) {
			path = buf_to_string(found);
		} else {
			// No global config file yet: create it where git looks first (~/.gitconfig).
			git_buf_dispose(&found);
			git_buf dirs = GIT_BUF_INIT;
			git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, &dirs);
			const String first = buf_to_string(dirs).get_slice(String::chr(GIT_PATH_LIST_SEPARATOR), 0);
			if (first.is_empty()) {
				return fail("Couldn't find your home folder to save your name and email in.");
			}
			path = first.path_join(".gitconfig");
		}
		err = git_config_open_ondisk(config.out(), path.utf8().get_data());
	} else {
		ConfigPtr all;
		err = git_repository_config(all.out(), repo);
		if (err >= 0) {
			err = git_config_open_level(config.out(), all, GIT_CONFIG_LEVEL_LOCAL);
		}
	}
	if (err >= 0) {
		err = git_config_set_string(config, "user.name", name.utf8().get_data());
	}
	if (err >= 0) {
		err = git_config_set_string(config, "user.email", email.utf8().get_data());
	}
	return to_error(err);
}

// Makes the folder p_path a new, empty git repository. Its first branch is named like the git
// CLI would (init.defaultBranch, else "main"). The .gitignore and .gitattributes Godot's project
// manager writes are added to p_project_path when missing (p_path is that folder or one above it).
Error GitRepository::init_repository(const String &p_path, const String &p_project_path) {
	git_error_clear();
	const String path = ProjectSettings::get_singleton()->globalize_path(p_path);
	const String project_path = ProjectSettings::get_singleton()->globalize_path(p_project_path);

	String branch = "main";
	ConfigPtr config;
	git_buf configured = GIT_BUF_INIT;
	if (git_config_open_default(config.out()) == 0 && git_config_get_string_buf(&configured, config, "init.defaultBranch") == 0) {
		branch = buf_to_string(configured);
	} else {
		git_buf_dispose(&configured);
	}
	const CharString branch_utf8 = branch.utf8();

	git_repository_init_options options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
	options.flags = GIT_REPOSITORY_INIT_NO_REINIT | GIT_REPOSITORY_INIT_MKPATH;
	options.initial_head = branch_utf8.get_data();
	git_repository *created = nullptr;
	const int err = git_repository_init_ext(&created, path.utf8().get_data(), &options);
	git_repository_free(created);
	if (err == GIT_EEXISTS) {
		return fail("That folder is already a git repository.");
	}
	if (err < 0) {
		return to_error(err);
	}

	// Godot's own files, word for word (editor/version_control/editor_vcs_interface.cpp).
	const String ignore = project_path.path_join(".gitignore");
	if (!FileAccess::file_exists(ignore)) {
		Ref<FileAccess> file = FileAccess::open(ignore, FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string("# Godot 4+ specific ignores\n.godot/\n/android/\n");
		}
	}
	const String attributes = project_path.path_join(".gitattributes");
	if (!FileAccess::file_exists(attributes)) {
		Ref<FileAccess> file = FileAccess::open(attributes, FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string("# Normalize EOL for all files that Git considers text files.\n* text=auto eol=lf\n");
		}
	}
	return OK;
}

// For the tests: read the global and system git config from p_path instead of the user's own
// ("" goes back to normal), so they can see what happens when git has no name and email.
void GitRepository::set_config_home(const String &p_path) {
	const CharString path = p_path.utf8();
	for (git_config_level_t level : { GIT_CONFIG_LEVEL_GLOBAL, GIT_CONFIG_LEVEL_XDG, GIT_CONFIG_LEVEL_SYSTEM, GIT_CONFIG_LEVEL_PROGRAMDATA }) {
		git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, level, p_path.is_empty() ? nullptr : path.get_data());
	}
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
