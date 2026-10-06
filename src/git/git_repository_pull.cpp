// GitRepository: pull, and merging a branch into the current one. A pull fetches, then both
// fast-forward or merge (merge_into_head), carrying uncommitted edits across when they merge
// cleanly (see plan_pull). When something would conflict they refuse up front, changing nothing,
// and name the files; asked to start a merge (the panel asks you), they go ahead and stop at the
// conflicts instead, for the resolver.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/templates/local_vector.hpp>

#include <string>

#include "git/git_cli.h"
#include "git/git_lfs.h"
#include "git/git_remote_callbacks.h"
#include "git/git_util.h"

using namespace godot_git;

namespace {

// How a merge into HEAD is worded: pull and merge_branch share it.
struct MergeWords {
	String name; // What's merged in: "origin/main", "feature".
	String message; // The merge commit's message.
	bool pull = true;

	String nothing() const { return pull ? String("Nothing was pulled") : String("Nothing was merged"); }
	String verb() const { return pull ? String("pull") : String("merge"); }
	String done() const { return pull ? String("Pulled") : String("Merged"); }
	// What brings the new commits: "the new commits", "the commits on feature".
	String commits() const { return pull ? String("the new commits") : vformat("the commits on %s", name); }
};

// What a merge into HEAD did (see merge_into_head).
struct MergeOutcome {
	int commits = 0;
	bool merged = false;
	PackedStringArray carried;
	PackedStringArray conflicts;
	String notice;
};

// An uncommitted edit a pull carries across: set aside before the pull, then merged into the
// version the pull brought (see plan_pull).
struct CarriedEdit {
	String path;
	PackedByteArray original; // The file on disk, byte for byte, to put back if anything fails.
	std::string base; // HEAD's version before the pull, which the edit was made on.
	std::string mine; // The edit as git stores it (CRLF made LF, and so on).
	bool conflicts = false; // It overlaps the pulled changes: comes back as a conflict.
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
// Edits that overlap the incoming changes (same lines) aren't refused here but listed in
// r_conflicting, and carried with conflicts set: they can come back as conflicts to resolve.
PackedStringArray plan_pull(git_repository *p_repo, const git_oid *p_head, const git_oid *p_theirs, LocalVector<CarriedEdit> *r_carried, PackedStringArray *r_conflicting = nullptr) {
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
			edit.conflicts = !merge_text(path, edit.base, edit.mine, pulled, merged);
		}
		if (!ok) {
			refused.push_back(path);
			continue;
		}
		if (edit.conflicts && r_conflicting) {
			r_conflicting->push_back(path);
		}
		if (r_carried) {
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
Error set_aside_edits(git_repository *p_repo, const LocalVector<CarriedEdit> &p_edits, const MergeWords &p_words, String &r_backup) {
	if (p_edits.is_empty()) {
		return OK;
	}
	const String git_dir = String::utf8(git_repository_path(p_repo));
	r_backup = git_dir.path_join("godot-git-pull");
	for (int i = 2; DirAccess::dir_exists_absolute(r_backup); i++) {
		r_backup = git_dir.path_join(vformat("godot-git-pull-%d", i)); // Never overwrite an earlier one.
	}
	const String failed = vformat("%s: couldn't save a copy of your uncommitted edits first.", p_words.nothing());
	if (DirAccess::make_dir_recursive_absolute(r_backup) != OK) {
		return fail(failed);
	}
	String readme = vformat("Your uncommitted edits, saved by the Godot Git panel while it %s. If this folder is still here, the %s didn't finish: each file below is your version of the file named next to it.\n\n", p_words.pull ? "pulled" : "merged a branch", p_words.verb());
	for (uint32_t i = 0; i < p_edits.size(); i++) {
		if (!write_file(r_backup.path_join(itos(i)), (const char *)p_edits[i].original.ptr(), p_edits[i].original.size())) {
			return fail(failed);
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

// An edit that overlaps what the pull brought, as a conflict: the index gets the three versions
// (the file as it was, your edit, the pulled version), like any conflict git leaves, so the
// resolver (and a terminal) can take it from there; the file gets git's markers.
bool write_carried_conflict(git_repository *p_repo, const CarriedEdit &p_edit, const String &p_theirs_label) {
	ObjectPtr now;
	if (git_revparse_single(now.out(), p_repo, vformat("HEAD:%s", p_edit.path).utf8().get_data()) < 0 || git_object_type(now) != GIT_OBJECT_BLOB) {
		return false;
	}
	git_oid base_id, mine_id;
	IndexPtr index;
	if (git_blob_create_from_buffer(&base_id, p_repo, p_edit.base.data(), p_edit.base.size()) < 0 ||
			git_blob_create_from_buffer(&mine_id, p_repo, p_edit.mine.data(), p_edit.mine.size()) < 0 ||
			git_repository_index(index.out(), p_repo) < 0) {
		return false;
	}
	const CharString path = p_edit.path.utf8();
	git_index_entry entries[3] = {};
	const git_oid *ids[3] = { &base_id, &mine_id, git_object_id(now) };
	for (int i = 0; i < 3; i++) {
		entries[i].path = path.get_data();
		entries[i].mode = GIT_FILEMODE_BLOB;
		git_oid_cpy(&entries[i].id, ids[i]);
	}
	git_index_remove(index, path.get_data(), 0);
	if (git_index_conflict_add(index, &entries[0], &entries[1], &entries[2]) < 0 || git_index_write(index) < 0) {
		return false;
	}
	const std::string theirs = blob_text((git_blob *)now.get());
	const std::string *texts[3] = { &p_edit.base, &p_edit.mine, &theirs };
	git_merge_file_input inputs[3];
	for (int i = 0; i < 3; i++) {
		git_merge_file_input_init(&inputs[i], GIT_MERGE_FILE_INPUT_VERSION);
		inputs[i].ptr = texts[i]->data();
		inputs[i].size = texts[i]->size();
		inputs[i].path = path.get_data();
		inputs[i].mode = GIT_FILEMODE_BLOB;
	}
	const CharString theirs_label = p_theirs_label.utf8();
	git_merge_file_options options = GIT_MERGE_FILE_OPTIONS_INIT;
	options.our_label = "your changes";
	options.their_label = theirs_label.get_data();
	git_merge_file_result merged = {};
	std::string on_disk;
	const bool ok = git_merge_file(&merged, &inputs[0], &inputs[1], &inputs[2], &options) == 0 &&
			apply_filters(p_repo, p_edit.path, merged.ptr, merged.len, GIT_FILTER_TO_WORKTREE, on_disk) &&
			write_file(String::utf8(git_repository_workdir(p_repo)).path_join(p_edit.path), on_disk.data(), on_disk.size());
	git_merge_file_result_free(&merged);
	return ok;
}

// After the pull: each carried edit merged into what HEAD has now (the pull's real result, not
// the prediction), or, if the pull failed, the files put back exactly as they were. Edits that
// overlap come back as conflicts (r_conflicted); then the copies stay, with a note in the git dir
// (pull_state_path) so Abort can put everything back. Otherwise the copies are removed once
// every file is back. Returns what couldn't be done, or "".
String finish_carried_edits(git_repository *p_repo, const LocalVector<CarriedEdit> &p_edits, const String &p_backup, bool p_pulled, PackedStringArray &r_carried, const git_oid *p_old_head, const String &p_upstream, PackedStringArray &r_conflicted) {
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
			} else if (edit.conflicts) {
				done = write_carried_conflict(p_repo, edit, p_upstream);
				if (done) {
					r_conflicted.push_back(edit.path);
				}
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
	if (!r_conflicted.is_empty()) {
		// Kept until the merge is finished or aborted (see GitRepository::abort_operation).
		Dictionary state;
		state["old_head"] = String(git_oid_tostr_s(p_old_head));
		state["upstream"] = p_upstream;
		state["backup"] = p_backup;
		PackedStringArray paths;
		for (const CarriedEdit &edit : p_edits) {
			paths.push_back(edit.path);
		}
		state["edits"] = paths;
		state["conflicts"] = r_conflicted;
		const CharString json = JSON::stringify(state, "\t").utf8();
		write_file(pull_state_path(p_repo), json.get_data(), json.length());
		return String();
	}
	for (uint32_t i = 0; i < p_edits.size(); i++) {
		DirAccess::remove_absolute(p_backup.path_join(itos(i)));
	}
	DirAccess::remove_absolute(p_backup.path_join("README.txt"));
	DirAccess::remove_absolute(p_backup);
	return String();
}

Error fast_forward(git_repository *p_repo, git_reference *p_head, const git_annotated_commit *p_theirs, const MergeWords &p_words, RemoteContext &p_ctx) {
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
			return fail(vformat("Your local changes would be overwritten by the %s. Commit, stash or discard them first.", p_words.verb()));
		}
	}
	if (err >= 0) {
		ReferencePtr moved;
		const String reflog = p_words.pull ? String("pull: Fast-forward") : vformat("merge %s: Fast-forward", p_words.name);
		err = git_reference_set_target(moved.out(), p_head, target_oid, reflog.utf8().get_data());
	}
	return to_error(err);
}

// Merges p_theirs into HEAD and commits the result. On conflicts (listed in r_conflicts) the merge
// is fully undone (hard reset, merge state cleared) and an error returned; or, with
// p_leave_conflicts, left as git leaves a conflicted merge, for the resolver, and OK returned.
Error merge_and_commit(git_repository *p_repo, git_reference *p_head, const git_annotated_commit *p_theirs, const MergeWords &p_words, RemoteContext &p_ctx, bool p_leave_conflicts, PackedStringArray &r_conflicts) {
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
	const String &message = p_words.message;
	if (err >= 0 && git_index_has_conflicts(index)) {
		IndexConflictIteratorPtr it;
		if (git_index_conflict_iterator_new(it.out(), index) == 0) {
			const git_index_entry *ancestor = nullptr, *ours = nullptr, *theirs = nullptr;
			while (git_index_conflict_next(&ancestor, &ours, &theirs, it) == 0) {
				const git_index_entry *entry = ours ? ours : (theirs ? theirs : ancestor);
				r_conflicts.push_back(String::utf8(entry->path));
			}
		}
		if (p_leave_conflicts) {
			// The message Finish Merge commits with (git commit takes it from MERGE_MSG).
			const CharString merge_msg = vformat("%s\n", message).utf8();
			write_file(String::utf8(git_repository_path(p_repo)).path_join("MERGE_MSG"), merge_msg.get_data(), merge_msg.length());
			return OK;
		}
		ObjectPtr head_commit;
		git_revparse_single(head_commit.out(), p_repo, "HEAD");
		git_reset(p_repo, head_commit, GIT_RESET_HARD, nullptr);
		git_repository_state_cleanup(p_repo);
		return fail(vformat("%s: your commits and %s changed the same lines in %s.", p_words.nothing(), p_words.name, name_list(r_conflicts)));
	}

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
			return fail(vformat("%s: the merge commit didn't go through, so the merge was undone. %s", p_words.nothing(), reason));
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
		err = git_commit_create(&commit_oid, p_repo, nullptr, signature, signature, nullptr, message.utf8().get_data(), tree, 2, parents);
	}
	if (err >= 0) {
		// The reflog says what git's would ("pull: Merge made by ..."), which is how Undo tells a
		// pull from a merge (get_undo); libgit2's own entry would only say "commit".
		ReferencePtr moved;
		const String reflog = p_words.pull ? String("pull: Merge made by the 'ort' strategy.") : vformat("merge %s: Merge made by the 'ort' strategy.", p_words.name);
		err = git_reference_set_target(moved.out(), p_head, &commit_oid, reflog.utf8().get_data());
	}
	git_repository_state_cleanup(p_repo);
	return to_error(err);
}

// merge_and_commit, with uncommitted changes to tracked files (normal in Godot, which rewrites
// project.godot and scenes) set aside first and put back afterwards, like `git pull --autostash`.
// That's what lets a conflicting merge be undone completely. r_notice explains if the changes
// couldn't be put back (a safety net: paths_blocking_pull already rules that out).
Error merge_with_autostash(git_repository *p_repo, git_reference *p_head, const git_annotated_commit *p_theirs, const MergeWords &p_words, RemoteContext &p_ctx, String &r_notice, PackedStringArray &r_conflicts) {
	git_status_options status_opts = GIT_STATUS_OPTIONS_INIT;
	status_opts.flags = 0; // Tracked files only; untracked files are left where they are.
	StatusListPtr status;
	if (git_status_list_new(status.out(), p_repo, &status_opts) != 0 || git_status_list_entrycount(status) == 0) {
		return merge_and_commit(p_repo, p_head, p_theirs, p_words, p_ctx, false, r_conflicts);
	}

	git_oid stash_id;
	SignaturePtr stasher;
	int err = git_signature_default(stasher.out(), p_repo);
	if (err >= 0) {
		err = git_stash_save(&stash_id, p_repo, stasher, vformat("godot-git: your changes, set aside during a %s", p_words.verb()).utf8().get_data(), GIT_STASH_DEFAULT);
	}
	if (err < 0) {
		return to_error(err);
	}

	const Error result = merge_and_commit(p_repo, p_head, p_theirs, p_words, p_ctx, false, r_conflicts);
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
		r_notice = vformat("%s, but it also changed %s, which you had uncommitted edits to. Your edits are kept in a git stash, untouched; run `git stash pop` in a terminal to merge them back.",
				p_words.done(), overlap.is_empty() ? String("files") : name_list(overlap));
	} else if (result != OK) {
		fail(restored ? merge_error : vformat("%s Your uncommitted changes are saved in a git stash; run `git stash pop` in a terminal to get them back.", merge_error));
	}
	return result;
}

// Whether anything is staged (the index differs from HEAD). A merge left for the resolver is
// finished by committing the index, which must hold the merge and nothing else.
bool has_staged_changes(git_repository *p_repo) {
	git_status_options opts = GIT_STATUS_OPTIONS_INIT;
	opts.show = GIT_STATUS_SHOW_INDEX_ONLY;
	opts.flags = 0;
	StatusListPtr status;
	return git_status_list_new(status.out(), p_repo, &opts) == 0 && git_status_list_entrycount(status) > 0;
}

// Brings p_theirs into HEAD's branch: fast-forwards, or makes a merge commit, carrying your
// uncommitted edits across (plan_pull). Refuses, changing nothing, when something would conflict;
// with p_start_merge it stops at the conflicts instead (see GitRepository::pull). p_lfs_remote is
// where the new commits' LFS files come from ("" for none).
Error merge_into_head(git_repository *p_repo, git_reference *p_head, const git_annotated_commit *p_theirs, const MergeWords &p_words, const String &p_lfs_remote, const Callable &p_progress, bool p_login_prompts, bool p_start_merge, MergeOutcome &r_outcome) {
	git_merge_analysis_t analysis = GIT_MERGE_ANALYSIS_NONE;
	git_merge_preference_t preference = GIT_MERGE_PREFERENCE_NONE;
	const git_annotated_commit *heads[] = { p_theirs };
	const int err = git_merge_analysis(&analysis, &preference, p_repo, heads, 1);
	if (err < 0) {
		return to_error(err);
	}
	if (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) {
		return OK;
	}
	const git_oid *head_oid = git_reference_target(p_head);
	const git_oid *their_oid = git_annotated_commit_id(p_theirs);

	PackedStringArray conflicting;
	LocalVector<CarriedEdit> carried;
	const PackedStringArray blocking = plan_pull(p_repo, head_oid, their_oid, &carried, &conflicting);
	if (!blocking.is_empty()) {
		// Checked before anything is touched, so your edits never end up in a stash only a
		// terminal can get back.
		return fail(vformat("%s: %s change %s, and your uncommitted changes there can't be merged in (they're staged, new, deleted or binary). Commit, stash or discard your changes to %s first, then %s again.",
				p_words.nothing(), p_words.commits(), name_list(blocking), blocking.size() == 1 ? String("it") : String("them"), p_words.verb()));
	}
	if (!conflicting.is_empty() && !p_start_merge) {
		// The panel asks whether to start a merge, and calls again with p_start_merge if so.
		r_outcome.conflicts = conflicting;
		return fail(vformat("%s: your uncommitted changes to %s and %s changed the same lines.", p_words.nothing(), name_list(conflicting), p_words.name));
	}
	if (repo_uses_lfs(p_repo)) {
		if (require_lfs(p_repo, p_words.pull ? "pulling" : "merging") != OK) {
			return FAILED;
		}
		if (!p_lfs_remote.is_empty()) {
			// The new commits' LFS files, downloaded before any file changes.
			RemoteContext lfs_ctx;
			lfs_ctx.workdir = String::utf8(git_repository_workdir(p_repo));
			lfs_ctx.login_prompts_allowed = p_login_prompts;
			lfs_ctx.progress = p_progress;
			const Error lfs_err = lfs_fetch(p_repo, lfs_ctx, p_lfs_remote, String(git_oid_tostr_s(their_oid)));
			if (lfs_err != OK) {
				return lfs_err;
			}
		}
	}

	// From here on everything is local: progress only, no more canceling.
	RemoteContext ctx;
	ctx.progress = p_progress;
	size_t ahead = 0, behind = 0;
	if (git_graph_ahead_behind(&ahead, &behind, p_repo, head_oid, their_oid) == 0) {
		r_outcome.commits = (int)behind;
	}
	const bool fast_forward_only = (analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) && !(preference & GIT_MERGE_PREFERENCE_NO_FASTFORWARD);
	Error result = OK;
	if (!fast_forward_only) {
		r_outcome.merged = true;
		result = require_identity(p_repo, p_words.pull ? String("Nothing was pulled: your branch and the remote's have both moved on, so pulling makes a merge commit.") : vformat("Nothing was merged: merging %s makes a merge commit.", p_words.name));
	}
	// A merge left at its conflicts is finished by committing the index, so nothing else may be
	// staged; and your uncommitted edits can't be carried across a merge that isn't done.
	const bool leave_merge = p_start_merge && !fast_forward_only && carried.is_empty() && !has_staged_changes(p_repo);
	PackedStringArray carried_paths;
	for (const CarriedEdit &edit : carried) {
		carried_paths.push_back(edit.path);
	}
	String backup;
	if (result == OK) {
		result = set_aside_edits(p_repo, carried, p_words, backup);
	}
	PackedStringArray merge_conflicts;
	if (result == OK) {
		if (fast_forward_only) {
			result = fast_forward(p_repo, p_head, p_theirs, p_words, ctx);
		} else if (leave_merge) {
			// Unrelated uncommitted edits stay where they are: git_merge leaves files it doesn't
			// touch alone.
			result = merge_and_commit(p_repo, p_head, p_theirs, p_words, ctx, true, merge_conflicts);
		} else {
			result = merge_with_autostash(p_repo, p_head, p_theirs, p_words, ctx, r_outcome.notice, merge_conflicts);
			if (result != OK && !merge_conflicts.is_empty()) {
				if (!p_start_merge) {
					r_outcome.conflicts = merge_conflicts; // The panel asks whether to start a merge.
				} else {
					fail(vformat("%s: your commits and %s changed the same lines in %s, and %s. Commit or discard your changes first, then %s again.", p_words.nothing(), p_words.name, name_list(merge_conflicts),
							carried.is_empty() ? String("you have staged changes") : vformat("you have uncommitted changes to %s", name_list(carried_paths)), p_words.verb()));
				}
			}
		}
	}
	if (leave_merge && result == OK && !merge_conflicts.is_empty()) {
		r_outcome.conflicts = merge_conflicts;
	}
	if (!backup.is_empty()) {
		// Keep the merge's own error message: putting the files back may set another.
		const String merge_error = result == OK ? String() : GitRepository::get_last_error();
		PackedStringArray carried_conflicts;
		const String problem = finish_carried_edits(p_repo, carried, backup, result == OK, r_outcome.carried, head_oid, p_words.name, carried_conflicts);
		r_outcome.conflicts.append_array(carried_conflicts);
		if (result != OK) {
			fail(problem.is_empty() ? merge_error : vformat("%s %s", merge_error, problem));
		} else if (!problem.is_empty()) {
			r_outcome.notice = vformat("%s. %s", p_words.done(), problem);
		}
	}
	if (result != OK) {
		r_outcome.commits = 0;
		r_outcome.merged = false;
	}
	return result;
}

// p_branch as a branch to merge: a local branch first, else a remote-tracking one ("origin/x").
bool lookup_merge_branch(git_repository *p_repo, const String &p_branch, ReferencePtr &r_ref, bool &r_remote) {
	const CharString name = p_branch.utf8();
	r_remote = false;
	if (git_branch_lookup(r_ref.out(), p_repo, name.get_data(), GIT_BRANCH_LOCAL) == 0) {
		return true;
	}
	r_remote = true;
	if (git_branch_lookup(r_ref.out(), p_repo, name.get_data(), GIT_BRANCH_REMOTE) == 0) {
		return true;
	}
	git_error_clear();
	return false;
}

// git's own message for merging p_branch into p_into: "Merge branch 'feature'", or "Merge
// remote-tracking branch 'origin/x' into dev" (git leaves out "into" for the main branch).
String merge_message(const String &p_branch, bool p_remote, const String &p_into) {
	const String message = vformat("Merge %s '%s'", p_remote ? "remote-tracking branch" : "branch", p_branch);
	return p_into == "main" || p_into == "master" ? message : vformat("%s into %s", message, p_into);
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

// The uncommitted edits a pull would bring back as conflicts (same lines as the new commits), as of
// the last fetch: Pull then asks whether to start a merge.
PackedStringArray GitRepository::get_pull_conflicts() const {
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
	PackedStringArray conflicting;
	plan_pull(repo, ours, theirs, nullptr, &conflicting);
	return conflicting;
}

// Fetches the upstream, then fast-forwards, or creates a merge commit when both sides have new
// commits. Uncommitted edits to files the new commits change are merged into the new versions
// when they don't overlap (see plan_pull). When something would conflict (your edits, or your
// commits, on the same lines as the new commits) it refuses, changing nothing, and lists the
// files in get_pull_result()["conflicts"]; with p_start_merge it goes ahead and stops at those
// conflicts: a merge git knows (your commits), or the panel's own (your uncommitted edits; see
// write_carried_conflict). Staged, new, deleted, binary and LFS files in the way always refuse.
Error GitRepository::pull(bool p_start_merge) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	begin_network_operation();
	notice = String();
	pulled_commits = 0;
	pull_merged = false;
	pull_carried.clear();
	pull_conflicts.clear();
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
	const int err = git_annotated_commit_from_ref(theirs.out(), repo, upstream);
	if (err < 0) {
		return to_error(err);
	}
	MergeWords words;
	words.name = String::utf8(git_reference_shorthand(upstream));
	words.message = vformat("Merge remote-tracking branch '%s'", words.name);
	MergeOutcome outcome;
	const Error result = merge_into_head(repo, head, theirs, words, remote_name, progress_callback, login_prompts_allowed, p_start_merge, outcome);
	pulled_commits = outcome.commits;
	pull_merged = outcome.merged;
	pull_carried = outcome.carried;
	pull_conflicts = outcome.conflicts;
	notice = outcome.notice;
	return result;
}

// Merges p_branch (a local branch, or a remote-tracking one like "origin/feature") into the
// current branch, like `git merge`, but the way pull merges: fast-forwards when it can, carries
// your uncommitted edits across, and refuses up front when something would conflict, listing the
// files in get_pull_result()["conflicts"]; with p_start_merge it stops at those conflicts for the
// resolver instead. get_pull_result() says how it went. Doesn't fetch; may download LFS files.
Error GitRepository::merge_branch(const String &p_branch, bool p_start_merge) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	begin_network_operation();
	notice = String();
	pulled_commits = 0;
	pull_merged = false;
	pull_carried.clear();
	pull_conflicts.clear();
	if (require_no_operation(repo, "merge") != OK) {
		return FAILED;
	}
	ReferencePtr head;
	if (head_branch(head.out(), repo) < 0) {
		return FAILED;
	}
	ReferencePtr branch;
	bool remote = false;
	if (!lookup_merge_branch(repo, p_branch, branch, remote)) {
		return fail(vformat("There's no branch called %s anymore.", p_branch));
	}
	if (git_reference_cmp(branch, head) == 0) {
		return fail("That's the branch you're on.");
	}
	String lfs_remote;
	git_buf remote_buf = GIT_BUF_INIT;
	const int found = remote ? git_branch_remote_name(&remote_buf, repo, git_reference_name(branch)) : git_branch_upstream_remote(&remote_buf, repo, git_reference_name(branch));
	if (found == 0) {
		lfs_remote = buf_to_string(remote_buf);
	} else {
		git_buf_dispose(&remote_buf);
		git_error_clear(); // No remote is fine: its LFS files are here already, or nowhere.
	}

	AnnotatedCommitPtr theirs;
	const int err = git_annotated_commit_from_ref(theirs.out(), repo, branch);
	if (err < 0) {
		return to_error(err);
	}
	MergeWords words;
	words.name = p_branch;
	words.message = merge_message(p_branch, remote, String::utf8(git_reference_shorthand(head)));
	words.pull = false;
	MergeOutcome outcome;
	const Error result = merge_into_head(repo, head, theirs, words, lfs_remote, progress_callback, login_prompts_allowed, p_start_merge, outcome);
	pulled_commits = outcome.commits;
	pull_merged = outcome.merged;
	pull_carried = outcome.carried;
	pull_conflicts = outcome.conflicts;
	notice = outcome.notice;
	return result;
}

// The branches that could be merged into the current one, as get_branch_list lists them (local
// first, then remote branches without a local one, newest first), without the current branch:
// [{ "name", "local", "time", "commits": how many of its commits the current branch doesn't have
// (0: nothing to merge), "behind": how many of the current branch's commits it doesn't have }].
Array GitRepository::get_merge_branches() const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	ReferencePtr head;
	if (git_repository_head(head.out(), repo) < 0 || !git_reference_target(head)) {
		git_error_clear();
		return result;
	}
	for (const Variant &item : get_branch_list()) {
		const Dictionary branch = item;
		if (bool(branch["current"])) {
			continue;
		}
		ReferencePtr ref;
		bool remote = false;
		if (!lookup_merge_branch(repo, branch["name"], ref, remote) || !git_reference_target(ref)) {
			continue;
		}
		size_t ahead = 0, behind = 0;
		git_graph_ahead_behind(&ahead, &behind, repo, git_reference_target(head), git_reference_target(ref));
		Dictionary entry;
		entry["name"] = branch["name"];
		entry["local"] = branch["local"];
		entry["time"] = branch["time"];
		entry["commits"] = (int64_t)behind;
		entry["behind"] = (int64_t)ahead;
		result.push_back(entry);
	}
	return result;
}

// What merging p_branch into the current branch would do, worked out without changing anything:
// { "commits": its commits the current branch doesn't have, "files": files they change,
//   "fast_forward": the branch just moves forward (no merge commit), "conflicts": the files that
//   would stop at conflicts (your commits or your uncommitted edits against its commits),
//   "carried": your uncommitted edits it would merge into the new versions, "problem": why it
//   can't be done ("" when it can), "behind_remote": for a local branch, how many commits its
//   remote branch has that it doesn't (they wouldn't be merged), "remote_branch": that branch }.
Dictionary GitRepository::get_merge_preview(const String &p_branch) const {
	Dictionary result;
	result["commits"] = 0;
	result["files"] = 0;
	result["fast_forward"] = false;
	result["conflicts"] = PackedStringArray();
	result["carried"] = PackedStringArray();
	result["problem"] = String();
	result["behind_remote"] = 0;
	result["remote_branch"] = String();
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	ReferencePtr head;
	ReferencePtr branch;
	bool remote = false;
	if (require_no_operation(repo, "merge") != OK || head_branch(head.out(), repo) < 0) {
		result["problem"] = get_last_error();
		git_error_clear();
		return result;
	}
	if (!lookup_merge_branch(repo, p_branch, branch, remote) || !git_reference_target(branch)) {
		result["problem"] = vformat("There's no branch called %s anymore.", p_branch);
		return result;
	}
	const git_oid *ours = git_reference_target(head);
	const git_oid *theirs = git_reference_target(branch);
	if (!remote) {
		ReferencePtr upstream;
		size_t ahead = 0, behind = 0;
		if (git_branch_upstream(upstream.out(), branch) == 0 && git_reference_target(upstream) &&
				git_graph_ahead_behind(&ahead, &behind, repo, theirs, git_reference_target(upstream)) == 0 && behind > 0) {
			result["behind_remote"] = (int64_t)behind;
			result["remote_branch"] = String::utf8(git_reference_shorthand(upstream));
		}
		git_error_clear();
	}
	size_t ahead = 0, behind = 0;
	git_oid base;
	if (git_graph_ahead_behind(&ahead, &behind, repo, ours, theirs) < 0 || git_merge_base(&base, repo, ours, theirs) < 0) {
		result["problem"] = vformat("%s and %s have no history in common, so they can't be merged here.", String::utf8(git_reference_shorthand(head)), p_branch);
		git_error_clear();
		return result;
	}
	result["commits"] = (int64_t)behind;
	if (behind == 0) {
		return result; // Nothing to merge.
	}
	result["files"] = changed_paths(repo, &base, theirs).size();
	const bool fast_forward_only = ahead == 0;
	result["fast_forward"] = fast_forward_only;

	PackedStringArray conflicting;
	LocalVector<CarriedEdit> carried;
	const PackedStringArray blocking = plan_pull(repo, ours, theirs, &carried, &conflicting);
	if (!blocking.is_empty()) {
		result["problem"] = vformat("Your uncommitted changes to %s can't be merged with the commits on %s (they're staged, new, deleted or binary). Commit, stash or discard them first.", name_list(blocking), p_branch);
		return result;
	}
	PackedStringArray carried_paths;
	for (const CarriedEdit &edit : carried) {
		if (!edit.conflicts) {
			carried_paths.push_back(edit.path);
		}
	}
	result["carried"] = carried_paths;

	// Your commits against theirs, merged in memory: the files it would stop at.
	PackedStringArray commit_conflicts;
	if (!fast_forward_only) {
		CommitPtr our_commit, their_commit;
		IndexPtr merged;
		if (git_commit_lookup(our_commit.out(), repo, ours) == 0 && git_commit_lookup(their_commit.out(), repo, theirs) == 0 &&
				git_merge_commits(merged.out(), repo, our_commit, their_commit, nullptr) == 0 && git_index_has_conflicts(merged)) {
			IndexConflictIteratorPtr it;
			if (git_index_conflict_iterator_new(it.out(), merged) == 0) {
				const git_index_entry *ancestor = nullptr, *mine = nullptr, *other = nullptr;
				while (git_index_conflict_next(&ancestor, &mine, &other, it) == 0) {
					const git_index_entry *entry = mine ? mine : (other ? other : ancestor);
					commit_conflicts.push_back(String::utf8(entry->path));
				}
			}
		}
		git_error_clear();
		if (!commit_conflicts.is_empty() && (!carried.is_empty() || has_staged_changes(repo))) {
			// merge_into_head refuses this too: a merge stopped at conflicts is finished by
			// committing the index, so your own changes can't be in the way.
			PackedStringArray mine;
			for (const CarriedEdit &edit : carried) {
				mine.push_back(edit.path);
			}
			result["problem"] = vformat("Your commits and %s changed the same lines in %s, and %s. Commit or discard your changes first.", p_branch, name_list(commit_conflicts),
					carried.is_empty() ? String("you have staged changes") : vformat("you have uncommitted changes to %s", name_list(mine)));
		}
	}
	commit_conflicts.append_array(conflicting);
	result["conflicts"] = commit_conflicts;
	return result;
}

// How the last pull() or merge_branch() went: { "commits": int (commits it brought in), "merged": bool (made a
// merge commit rather than fast-forwarding), "carried": the files whose uncommitted edits it
// merged into the new versions, "conflicts": the files that conflict: after a refusal, what a
// merge would stop at; after pull(true) or merge_branch(.., true), what it stopped at }.
Dictionary GitRepository::get_pull_result() const {
	Dictionary result;
	result["commits"] = pulled_commits;
	result["merged"] = pull_merged;
	result["carried"] = pull_carried;
	result["conflicts"] = pull_conflicts;
	return result;
}

// Copies of your edits a pull set aside and never put back (the editor went down mid-pull): the
// .git/godot-git-pull* folders, except the one a pull stopped at conflicts still uses.
// [{ "folder", "files": [{ "path", "copy": the copy's file, "back": the file already has it }] }].
Array GitRepository::get_pull_leftovers() const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	const String git_dir = String::utf8(git_repository_path(repo));
	const String in_use = read_pull_state(repo).get("backup", String());
	Ref<DirAccess> dir = DirAccess::open(git_dir);
	if (dir.is_null()) {
		return result;
	}
	for (const String &name : dir->get_directories()) {
		const String folder = git_dir.path_join(name);
		if (!name.begins_with("godot-git-pull") || folder.simplify_path() == in_use.simplify_path()) {
			continue;
		}
		Array files;
		for (const String &line : FileAccess::get_file_as_string(folder.path_join("README.txt")).split("\n", false)) {
			// "3  scenes/level.tscn", after the README's opening sentence.
			const int gap = line.find("  ");
			if (gap <= 0 || !line.left(gap).is_valid_int() || !FileAccess::file_exists(folder.path_join(line.left(gap)))) {
				continue;
			}
			Dictionary file;
			file["path"] = line.substr(gap + 2).strip_edges();
			file["copy"] = folder.path_join(line.left(gap));
			const String on_disk = get_workdir().path_join(file["path"]);
			file["back"] = FileAccess::file_exists(on_disk) && FileAccess::get_file_as_bytes(on_disk) == FileAccess::get_file_as_bytes(file["copy"]);
			files.push_back(file);
		}
		Dictionary leftover;
		leftover["folder"] = folder;
		leftover["files"] = files;
		result.push_back(leftover);
	}
	return result;
}

// Puts the copies in p_folder (get_pull_leftovers) back over the files, byte for byte, or with
// p_put_back false only deletes them. The folder goes once every copy is back.
Error GitRepository::resolve_pull_leftovers(const String &p_folder, bool p_put_back) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	for (const Variant &item : get_pull_leftovers()) {
		const Dictionary leftover = item;
		if (String(leftover["folder"]) != p_folder) {
			continue;
		}
		const Array files = leftover["files"];
		PackedStringArray left;
		for (int i = 0; i < files.size(); i++) {
			const Dictionary file = files[i];
			if (p_put_back && !bool(file["back"])) {
				const PackedByteArray bytes = FileAccess::get_file_as_bytes(file["copy"]);
				if (!write_file(get_workdir().path_join(file["path"]), (const char *)bytes.ptr(), bytes.size())) {
					left.push_back(file["path"]);
					continue;
				}
			}
			DirAccess::remove_absolute(file["copy"]);
		}
		if (!left.is_empty()) {
			return fail(vformat("Couldn't write %s; the copies are still in %s.", name_list(left), p_folder));
		}
		DirAccess::remove_absolute(p_folder.path_join("README.txt"));
		DirAccess::remove_absolute(p_folder);
		return OK;
	}
	return fail("Those copies are gone already.");
}
