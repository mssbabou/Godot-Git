// GitRepository: local changes (stage, unstage, discard, commit, amend) and switching or
// creating branches.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/templates/local_vector.hpp>

#include "git/git_cli.h"
#include "git/git_lfs.h"
#include "git/git_remote_callbacks.h"
#include "git/git_util.h"

using namespace godot_git;

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
// During a merge stopped at conflicts, once none is left, it commits the merge (see _commit_merge).
Error GitRepository::commit(const String &p_message) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");

	git_error_clear();
	const bool merging = operation_in_progress(repo) == "merge";
	if ((!merging && require_no_operation(repo, "commit") != OK) || require_lfs(repo, "committing") != OK || require_identity(repo, "Nothing was committed.") != OK) {
		return FAILED;
	}
	if (merging) {
		return _commit_merge(p_message);
	}
	if (commit_needs_git(repo, COMMIT_NEW)) {
		return _commit_with_git(p_message, false);
	}
	git_oid oid;
	return to_error(git_commit_create_from_stage(&oid, repo, p_message.utf8().get_data(), nullptr));
}

// The merge's commit, like `git commit` during a merge: what's staged, with HEAD and the merged
// commit (MERGE_HEAD) as parents, even when it changes no files (you kept your side everywhere:
// the commit still records the branch as merged, so its conflicts don't come back). Refused while
// a file is still conflicted.
Error GitRepository::_commit_merge(const String &p_message) {
	IndexPtr index;
	if (git_repository_index(index.out(), repo) < 0) {
		return FAILED;
	}
	if (git_index_has_conflicts(index)) {
		const PackedStringArray conflicts = get_operation()["conflicts"];
		return fail(vformat("%s still %s conflicts: resolve %s first, then commit the merge.", name_list(conflicts), conflicts.size() == 1 ? "has" : "have", conflicts.size() == 1 ? "it" : "them"));
	}
	if (commit_needs_git(repo, COMMIT_MERGE)) {
		// git sees MERGE_HEAD and makes it a merge commit, running the hooks and signing it.
		begin_network_operation();
		RemoteContext ctx;
		ctx.progress = progress_callback;
		const Error err = commit_with_git(repo, ctx, p_message, COMMIT_MERGE);
		if (err == OK) {
			git_repository_state_cleanup(repo);
		}
		return err;
	}

	LocalVector<git_oid> parent_ids;
	git_oid head_id;
	if (git_reference_name_to_id(&head_id, repo, "HEAD") < 0) {
		return FAILED;
	}
	parent_ids.push_back(head_id);
	const auto add_merge_head = [](const git_oid *p_oid, void *p_payload) -> int {
		((LocalVector<git_oid> *)p_payload)->push_back(*p_oid);
		return 0;
	};
	int err = git_repository_mergehead_foreach(repo, add_merge_head, &parent_ids);
	LocalVector<CommitPtr> parents;
	parents.resize(parent_ids.size());
	LocalVector<const git_commit *> parent_ptrs;
	for (uint32_t i = 0; err >= 0 && i < parent_ids.size(); i++) {
		err = git_commit_lookup(parents[i].out(), repo, &parent_ids[i]);
		parent_ptrs.push_back(parents[i]);
	}
	git_oid tree_id, commit_id;
	TreePtr tree;
	SignaturePtr signature;
	if (err >= 0) {
		err = git_index_write_tree(&tree_id, index);
	}
	if (err >= 0) {
		err = git_tree_lookup(tree.out(), repo, &tree_id);
	}
	if (err >= 0) {
		err = git_signature_default(signature.out(), repo);
	}
	if (err >= 0) {
		err = git_commit_create(&commit_id, repo, "HEAD", signature, signature, nullptr, p_message.utf8().get_data(), tree, parent_ptrs.size(), parent_ptrs.ptr());
	}
	if (err >= 0) {
		git_repository_state_cleanup(repo);
	}
	return to_error(err);
}

// Whether HEAD's commit is on any remote-tracking branch, i.e. already pushed (or fetched).
bool GitRepository::is_head_pushed() const {
	ERR_FAIL_NULL_V_MSG(repo, false, "Repository is not open.");
	git_oid head;
	if (git_reference_name_to_id(&head, repo, "HEAD") < 0) {
		return false;
	}
	return on_remote_branch(repo, &head);
}

// Replaces the last commit with one that has p_message and what's staged now (which may be
// nothing: then only the message changes). Refused once the commit is pushed: rewriting it
// would leave teammates with a commit that no longer exists here.
Error GitRepository::amend(const String &p_message) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	if (require_no_operation(repo, "amend") != OK || require_lfs(repo, "committing") != OK || require_identity(repo, "The commit wasn't amended.") != OK) {
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
		err = git_commit_amend(&id, last, nullptr, nullptr, committer, nullptr, p_message.utf8().get_data(), tree);
		if (err >= 0) {
			// git's reflog message, "commit (amend): ...", which is how Undo knows it was an amend
			// (get_undo); libgit2's own entry would only say "commit".
			CommitPtr amended;
			ReferencePtr moved;
			err = git_commit_lookup(amended.out(), repo, &id);
			if (err >= 0) {
				err = git_reference_set_target(moved.out(), head, &id, vformat("commit (amend): %s", String::utf8(git_commit_summary(amended))).utf8().get_data());
			}
		}
	}
	return to_error(err);
}

// Whether committing (or amending) goes through the git command line, because hooks would run
// or commits get signed. That can take a while (a hook may run a linter), so the dock does it in
// the background.
bool GitRepository::commit_runs_git(bool p_amend) const {
	ERR_FAIL_NULL_V_MSG(repo, false, "Repository is not open.");
	if (!p_amend && operation_in_progress(repo) == "merge") {
		return commit_needs_git(repo, COMMIT_MERGE);
	}
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
	if (require_no_operation(repo, "switch branches") != OK) {
		return FAILED;
	}

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
			// ERR_BUSY: your changes are in the way (the panel offers to stash them). Named when
			// they're the files the branches differ in; libgit2 also refuses for a few others.
			PackedStringArray in_way;
			ReferencePtr head;
			if (git_repository_head(head.out(), repo) == 0) {
				const HashSet<String> differ = changed_paths(repo, git_reference_target(head), git_object_id(target));
				for (const String &path : uncommitted_paths(repo)) {
					if (differ.has(path) && !in_way.has(path)) {
						in_way.push_back(path);
					}
				}
			}
			in_way.sort();
			fail(in_way.is_empty() ? String("Your local changes would be overwritten by switching branches. Commit, stash or discard them first.")
								   : vformat("Your changes to %s would be overwritten by switching branches. Commit, stash or discard them first.", name_list(in_way)));
			return ERR_BUSY;
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

// Renames a local branch (the current one too: HEAD follows). Its upstream setting moves with it,
// like `git branch -m`; the branch on the remote keeps its old name.
Error GitRepository::rename_branch(const String &p_branch, const String &p_new_name) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	if (require_no_operation(repo, "rename branches") != OK) {
		return FAILED;
	}
	int valid = 0;
	if (git_branch_name_is_valid(&valid, p_new_name.utf8().get_data()) < 0 || !valid) {
		return fail(vformat("\"%s\" isn't a valid branch name.", p_new_name));
	}
	ReferencePtr ref;
	if (git_branch_lookup(ref.out(), repo, p_branch.utf8().get_data(), GIT_BRANCH_LOCAL) < 0) {
		return fail(vformat("There is no branch called \"%s\".", p_branch));
	}
	ReferencePtr renamed;
	const int err = git_branch_move(renamed.out(), ref, p_new_name.utf8().get_data(), 0);
	if (err == GIT_EEXISTS) {
		return fail(vformat("A branch called \"%s\" already exists.", p_new_name));
	}
	return to_error(err);
}

// Deletes a local branch, never the current one. The branch on the remote stays. Commits only it
// had are lost (see get_branch_details), so the panel asks first.
Error GitRepository::delete_branch(const String &p_branch) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	if (require_no_operation(repo, "delete branches") != OK) {
		return FAILED;
	}
	ReferencePtr ref;
	if (git_branch_lookup(ref.out(), repo, p_branch.utf8().get_data(), GIT_BRANCH_LOCAL) < 0) {
		return fail(vformat("There is no branch called \"%s\".", p_branch));
	}
	if (git_branch_is_head(ref) == 1) {
		return fail(vformat("You're on %s. Switch to another branch before deleting it.", p_branch));
	}
	return to_error(git_branch_delete(ref));
}

// What deleting a local branch would mean: "unique", how many of its commits no other branch, tag
// or remote-tracking branch has (those would be lost; -1 if unknown), and "upstream", the
// remote-tracking branch it follows ("origin/x", or "").
Dictionary GitRepository::get_branch_details(const String &p_branch) const {
	Dictionary details;
	details["unique"] = -1;
	details["upstream"] = String();
	ERR_FAIL_NULL_V_MSG(repo, details, "Repository is not open.");

	ReferencePtr ref;
	if (git_branch_lookup(ref.out(), repo, p_branch.utf8().get_data(), GIT_BRANCH_LOCAL) < 0) {
		return details;
	}
	ReferencePtr upstream;
	const char *upstream_name = nullptr;
	if (git_branch_upstream(upstream.out(), ref) == 0 && git_branch_name(&upstream_name, upstream) == 0) {
		details["upstream"] = String::utf8(upstream_name);
	}

	RevwalkPtr walk;
	if (git_revwalk_new(walk.out(), repo) < 0 || git_revwalk_push_ref(walk, git_reference_name(ref)) < 0) {
		return details;
	}
	git_revwalk_hide_glob(walk, "refs/remotes");
	git_revwalk_hide_glob(walk, "refs/tags");
	git_revwalk_hide_head(walk); // Fails without commits or on an unborn branch; nothing to hide then.
	for (const String &other : get_branches()) {
		if (other != p_branch) {
			git_revwalk_hide_ref(walk, vformat("refs/heads/%s", other).utf8().get_data());
		}
	}
	int unique = 0;
	git_oid oid;
	while (git_revwalk_next(&oid, walk) == 0) {
		unique++;
	}
	git_error_clear(); // From a hide that had nothing to hide.
	details["unique"] = unique;
	return details;
}
