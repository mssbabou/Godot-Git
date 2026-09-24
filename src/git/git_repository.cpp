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
	ClassDB::bind_method(D_METHOD("get_commits", "max_count"), &GitRepository::get_commits, DEFVAL(50));
	ClassDB::bind_method(D_METHOD("uses_lfs"), &GitRepository::uses_lfs);

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

	ClassDB::bind_method(D_METHOD("fetch"), &GitRepository::fetch);
	ClassDB::bind_method(D_METHOD("pull"), &GitRepository::pull);
	ClassDB::bind_method(D_METHOD("push"), &GitRepository::push);
	ClassDB::bind_method(D_METHOD("get_notice"), &GitRepository::get_notice);
	ClassDB::bind_method(D_METHOD("get_pull_result"), &GitRepository::get_pull_result);
	ClassDB::bind_method(D_METHOD("set_progress_callback", "callback"), &GitRepository::set_progress_callback);
	ClassDB::bind_method(D_METHOD("set_login_prompts_allowed", "allowed"), &GitRepository::set_login_prompts_allowed);

	ClassDB::bind_static_method("GitRepository", D_METHOD("cancel_network"), &GitRepository::cancel_network);

	ClassDB::bind_static_method("GitRepository", D_METHOD("get_last_error"), &GitRepository::get_last_error);
	ClassDB::bind_static_method("GitRepository", D_METHOD("get_libgit2_version"), &GitRepository::get_libgit2_version);
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
	if (require_lfs(repo, "committing") != OK) {
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
	if (require_lfs(repo, "committing") != OK) {
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

String GitRepository::get_last_error() {
	// Since libgit2 1.8 this is never null; "no error" comes back with class GIT_ERROR_NONE.
	const git_error *err = git_error_last();
	return (err && err->klass != GIT_ERROR_NONE && err->message) ? String::utf8(err->message) : String();
}

String GitRepository::get_libgit2_version() {
	int major = 0, minor = 0, rev = 0;
	git_libgit2_version(&major, &minor, &rev);
	return vformat("%d.%d.%d", major, minor, rev);
}
