// GitRepository: pull. It fetches, then fast-forwards or merges, carrying uncommitted edits across
// when they merge cleanly (see plan_pull), and refuses up front, changing nothing, when they don't.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/templates/local_vector.hpp>

#include <string>

#include "git/git_cli.h"
#include "git/git_lfs.h"
#include "git/git_remote_callbacks.h"
#include "git/git_util.h"

using namespace godot_git;

namespace {

// An uncommitted edit a pull carries across: set aside before the pull, then merged into the
// version the pull brought (see plan_pull).
struct CarriedEdit {
	String path;
	PackedByteArray original; // The file on disk, byte for byte, to put back if anything fails.
	std::string base; // HEAD's version before the pull, which the edit was made on.
	std::string mine; // The edit as git stores it (CRLF made LF, and so on).
};

// A three-way merge of one file's contents, the way git merges a file both sides changed.
// False when it conflicts.
bool merge_text(const String &p_path, const std::string &p_base, const std::string &p_ours, const std::string &p_theirs, std::string &r_merged) {
	const CharString path = p_path.utf8();
	const std::string *texts[3] = { &p_base, &p_ours, &p_theirs };
	git_merge_file_input inputs[3];
	for (int i = 0; i < 3; i++) {
		git_merge_file_input_init(&inputs[i], GIT_MERGE_FILE_INPUT_VERSION);
		inputs[i].ptr = texts[i]->data();
		inputs[i].size = texts[i]->size();
		inputs[i].path = path.get_data();
		inputs[i].mode = GIT_FILEMODE_BLOB;
	}
	git_merge_file_result result = {};
	const bool clean = git_merge_file(&result, &inputs[0], &inputs[1], &inputs[2], nullptr) == 0 && result.automergeable;
	if (clean) {
		r_merged.assign(result.ptr, result.len);
	}
	git_merge_file_result_free(&result);
	return clean;
}

// The uncommitted files the incoming commits (merge base -> p_theirs) change too, in two groups.
// Carried (r_carried): an unstaged edit to a text file that merges cleanly into the version the
// pull brings, checked here in memory; the pull merges it back afterwards. Refused (returned,
// sorted): staged changes, new or deleted files, binary and LFS files, and edits that would
// conflict. A pull with any of those changes nothing.
PackedStringArray plan_pull(git_repository *p_repo, const git_oid *p_head, const git_oid *p_theirs, LocalVector<CarriedEdit> *r_carried) {
	PackedStringArray refused;
	git_oid base_oid;
	if (git_merge_base(&base_oid, p_repo, p_head, p_theirs) != 0) {
		return refused;
	}
	const HashSet<String> incoming = changed_paths(p_repo, &base_oid, p_theirs);
	PackedStringArray overlapping;
	for (const String &path : uncommitted_paths(p_repo)) {
		if (incoming.has(path) && !overlapping.has(path)) {
			overlapping.push_back(path);
		}
	}
	if (overlapping.is_empty()) {
		return refused;
	}

	CommitPtr head_commit, base_commit, their_commit;
	TreePtr head_tree, base_tree, their_tree;
	IndexPtr index;
	const bool ready = git_commit_lookup(head_commit.out(), p_repo, p_head) == 0 && git_commit_tree(head_tree.out(), head_commit) == 0 &&
			git_commit_lookup(base_commit.out(), p_repo, &base_oid) == 0 && git_commit_tree(base_tree.out(), base_commit) == 0 &&
			git_commit_lookup(their_commit.out(), p_repo, p_theirs) == 0 && git_commit_tree(their_tree.out(), their_commit) == 0 &&
			git_repository_index(index.out(), p_repo) == 0;
	const bool uses_lfs = repo_uses_lfs(p_repo);
	const String workdir = String::utf8(git_repository_workdir(p_repo));

	for (const String &path : overlapping) {
		const CharString path_utf8 = path.utf8();
		const String file = workdir.path_join(path);
		BlobPtr head_blob, base_blob, their_blob;
		const git_index_entry *staged = ready ? git_index_get_bypath(index, path_utf8.get_data(), 0) : nullptr;
		bool ok = ready && !(uses_lfs && is_lfs_path(p_repo, path_utf8.get_data())) &&
				read_tree_blob(p_repo, head_tree, path, head_blob) && // Not a new file.
				read_tree_blob(p_repo, their_tree, path, their_blob) && // Not deleted by the new commits.
				staged && git_oid_equal(&staged->id, git_blob_id(head_blob)) && // Nothing staged.
				FileAccess::file_exists(file) && // Not deleted here.
				!git_blob_is_binary(head_blob) && !git_blob_is_binary(their_blob);

		CarriedEdit edit;
		std::string pulled, merged;
		if (ok) {
			const PackedByteArray bytes = FileAccess::get_file_as_bytes(file);
			edit.original = bytes;
			ok = apply_filters(p_repo, path, (const char *)bytes.ptr(), bytes.size(), GIT_FILTER_TO_ODB, edit.mine) && !git_blob_data_is_binary(edit.mine.data(), edit.mine.size());
		}
		if (ok) {
			// What HEAD will have after the pull: theirs, or, when your branch changed the file
			// too, the merge of both (if that conflicts, the pull refuses by itself anyway).
			const bool base_has = read_tree_blob(p_repo, base_tree, path, base_blob);
			if (base_has && git_oid_equal(git_blob_id(base_blob), git_blob_id(head_blob))) {
				pulled = blob_text(their_blob);
			} else {
				ok = merge_text(path, base_has ? blob_text(base_blob) : std::string(), blob_text(head_blob), blob_text(their_blob), pulled);
			}
		}
		if (ok) {
			edit.base = blob_text(head_blob);
			ok = merge_text(path, edit.base, edit.mine, pulled, merged);
		}
		if (!ok) {
			refused.push_back(path);
		} else if (r_carried) {
			edit.path = path;
			r_carried->push_back(edit);
		}
	}
	refused.sort();
	return refused;
}

// Before the pull: the carried files back to HEAD's version, so the pull sees no local changes
// there. Copies go to .git/godot-git-pull first (with a README naming each file), so the edits
// exist on disk even if the editor goes down mid-pull.
Error set_aside_edits(git_repository *p_repo, const LocalVector<CarriedEdit> &p_edits, String &r_backup) {
	if (p_edits.is_empty()) {
		return OK;
	}
	const String git_dir = String::utf8(git_repository_path(p_repo));
	r_backup = git_dir.path_join("godot-git-pull");
	for (int i = 2; DirAccess::dir_exists_absolute(r_backup); i++) {
		r_backup = git_dir.path_join(vformat("godot-git-pull-%d", i)); // Never overwrite an earlier one.
	}
	if (DirAccess::make_dir_recursive_absolute(r_backup) != OK) {
		return fail("Couldn't save a copy of your uncommitted edits before pulling, so nothing was pulled.");
	}
	String readme = "Your uncommitted edits, saved by the Godot Git panel while it pulled. If this folder is still here, the pull didn't finish: each file below is your version of the file named next to it.\n\n";
	for (uint32_t i = 0; i < p_edits.size(); i++) {
		if (!write_file(r_backup.path_join(itos(i)), (const char *)p_edits[i].original.ptr(), p_edits[i].original.size())) {
			return fail("Couldn't save a copy of your uncommitted edits before pulling, so nothing was pulled.");
		}
		readme += vformat("%d  %s\n", i, p_edits[i].path);
	}
	const CharString readme_utf8 = readme.utf8();
	write_file(r_backup.path_join("README.txt"), readme_utf8.get_data(), readme_utf8.length());

	LocalVector<CharString> paths_utf8;
	LocalVector<char *> paths;
	for (const CarriedEdit &edit : p_edits) {
		paths_utf8.push_back(edit.path.utf8());
	}
	for (CharString &path : paths_utf8) {
		paths.push_back(path.ptrw());
	}
	git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
	opts.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;
	opts.paths.strings = paths.ptr();
	opts.paths.count = paths.size();
	return to_error(git_checkout_head(p_repo, &opts));
}

// After the pull: each carried edit merged into what HEAD has now (the pull's real result, not
// the prediction), or, if the pull failed, the files put back exactly as they were. The copies
// are removed once every file is back. Returns what couldn't be done, or "".
String finish_carried_edits(git_repository *p_repo, const LocalVector<CarriedEdit> &p_edits, const String &p_backup, bool p_pulled, PackedStringArray &r_carried) {
	if (p_edits.is_empty()) {
		return String();
	}
	const String workdir = String::utf8(git_repository_workdir(p_repo));
	PackedStringArray left;
	for (const CarriedEdit &edit : p_edits) {
		const String file = workdir.path_join(edit.path);
		bool done = false;
		if (p_pulled) {
			ObjectPtr now;
			std::string merged, on_disk;
			done = git_revparse_single(now.out(), p_repo, vformat("HEAD:%s", edit.path).utf8().get_data()) == 0 && git_object_type(now) == GIT_OBJECT_BLOB &&
					merge_text(edit.path, edit.base, edit.mine, blob_text((git_blob *)now.get()), merged) &&
					apply_filters(p_repo, edit.path, merged.data(), merged.size(), GIT_FILTER_TO_WORKTREE, on_disk) &&
					write_file(file, on_disk.data(), on_disk.size());
			if (done) {
				r_carried.push_back(edit.path);
			}
		} else {
			done = write_file(file, (const char *)edit.original.ptr(), edit.original.size());
		}
		if (!done) {
			left.push_back(edit.path);
		}
	}
	if (!left.is_empty()) {
		return vformat("Your uncommitted edits to %s couldn't be put back; they're saved, untouched, in %s.", String(", ").join(left), p_backup);
	}
	for (uint32_t i = 0; i < p_edits.size(); i++) {
		DirAccess::remove_absolute(p_backup.path_join(itos(i)));
	}
	DirAccess::remove_absolute(p_backup.path_join("README.txt"));
	DirAccess::remove_absolute(p_backup);
	return String();
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
				overlap.is_empty() ? String("files") : name_list(overlap));
	} else if (result != OK) {
		fail(restored ? merge_error : vformat("%s Your uncommitted changes are saved in a git stash; run `git stash pop` in a terminal to get them back.", merge_error));
	}
	return result;
}

} // namespace

// The uncommitted files a pull would refuse for, as of the last fetch (see plan_pull), so the panel
// can say so before Pull is pressed. Empty when there's nothing to pull.
PackedStringArray GitRepository::get_pull_blockers() const {
	ReferencePtr head;
	ReferencePtr upstream;
	if (!repo || git_repository_head(head.out(), repo) < 0 || git_branch_upstream(upstream.out(), head) < 0) {
		return PackedStringArray();
	}
	const git_oid *ours = git_reference_target(head);
	const git_oid *theirs = git_reference_target(upstream);
	if (!ours || !theirs || git_oid_equal(ours, theirs)) {
		return PackedStringArray();
	}
	return plan_pull(repo, ours, theirs, nullptr);
}

// Fetches the upstream, then fast-forwards, or creates a merge commit when both sides have new
// commits. Uncommitted edits to files the new commits change are merged into the new versions
// when they don't overlap (see plan_pull); otherwise it refuses, changing nothing, as it does
// when the merge itself would conflict.
Error GitRepository::pull() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	begin_network_operation();
	notice = String();
	pulled_commits = 0;
	pull_merged = false;
	pull_carried.clear();
	if (require_no_operation(repo, "pull") != OK) {
		return FAILED;
	}

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
	LocalVector<CarriedEdit> carried;
	if (err >= 0 && !(analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE)) {
		blocking = plan_pull(repo, head_oid, git_annotated_commit_id(theirs), &carried);
	}

	if (err < 0 || (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE)) {
		// Error, or nothing to do.
	} else if (!blocking.is_empty()) {
		// Checked before anything is touched, so your edits never end up in a stash only a
		// terminal can get back.
		result = fail(vformat("Nothing was pulled: the new commits change %s, and your uncommitted changes there can't be merged in (they touch the same lines, or they're staged, new, deleted or binary). Commit or discard your changes to %s first, then pull again.",
				name_list(blocking),
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
		const bool fast_forward_only = (analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) && !(preference & GIT_MERGE_PREFERENCE_NO_FASTFORWARD);
		if (!fast_forward_only) {
			pull_merged = true;
			result = require_identity(repo, "Nothing was pulled: your branch and the remote's have both moved on, so pulling makes a merge commit.");
		}
		String backup;
		if (result == OK) {
			result = set_aside_edits(repo, carried, backup);
		}
		if (result == OK) {
			result = fast_forward_only ? fast_forward(repo, head, theirs, ctx) : merge_with_autostash(repo, head, upstream, theirs, ctx, notice);
		}
		if (!backup.is_empty()) {
			// Keep the pull's own error message: putting the files back may set another.
			const String pull_error = result == OK ? String() : GitRepository::get_last_error();
			const String problem = finish_carried_edits(repo, carried, backup, result == OK, pull_carried);
			if (result != OK) {
				fail(problem.is_empty() ? pull_error : vformat("%s %s", pull_error, problem));
			} else if (!problem.is_empty()) {
				notice = vformat("Pulled. %s", problem);
			}
		}
		if (result != OK) {
			pulled_commits = 0;
			pull_merged = false;
		}
	}
	return result;
}

// How the last pull() went: { "commits": int (commits it brought in), "merged": bool (made a
// merge commit rather than fast-forwarding), "carried": the files whose uncommitted edits it
// merged into the new versions }.
Dictionary GitRepository::get_pull_result() const {
	Dictionary result;
	result["commits"] = pulled_commits;
	result["merged"] = pull_merged;
	result["carried"] = pull_carried;
	return result;
}
