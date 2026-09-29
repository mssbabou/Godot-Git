// GitRepository: stashes. Setting changes aside (what's staged, or everything), listing them
// (stashes made in a terminal too), and restoring one all or nothing: a restore that would clash
// with your current edits, or conflict with commits made since, is refused before anything
// changes, and the stash is only removed once it applied completely (libgit2 gotcha 2).
// Stashes are named by their commit hash: their position changes as stashes come and go.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/local_vector.hpp>

#include "git/git_cli.h"
#include "git/git_lfs.h"
#include "git/git_remote_callbacks.h"
#include "git/git_util.h"

using namespace godot_git;

namespace {

struct StashEntry {
	size_t index = 0;
	String message;
	git_oid id;
};

LocalVector<StashEntry> list_stashes(git_repository *p_repo) {
	LocalVector<StashEntry> entries;
	git_stash_foreach(
			p_repo, [](size_t p_index, const char *p_message, const git_oid *p_id, void *p_payload) -> int {
				StashEntry entry;
				entry.index = p_index;
				entry.message = String::utf8(p_message ? p_message : "");
				git_oid_cpy(&entry.id, p_id);
				static_cast<LocalVector<StashEntry> *>(p_payload)->push_back(entry);
				return 0;
			},
			&entries);
	return entries;
}

// The stash's position (for libgit2's apply and drop) from its hash; false if it's gone.
bool find_stash(git_repository *p_repo, const String &p_hash, size_t &r_index) {
	for (const StashEntry &entry : list_stashes(p_repo)) {
		if (String(git_oid_tostr_s(&entry.id)) == p_hash) {
			r_index = entry.index;
			return true;
		}
	}
	return false;
}

// Every file path in p_tree (a stash's untracked files).
void tree_paths(git_tree *p_tree, HashSet<String> &r_paths) {
	git_tree_walk(
			p_tree, GIT_TREEWALK_PRE, [](const char *p_root, const git_tree_entry *p_entry, void *p_payload) -> int {
				if (git_tree_entry_type(p_entry) == GIT_OBJECT_BLOB) {
					static_cast<HashSet<String> *>(p_payload)->insert(String::utf8(p_root) + String::utf8(git_tree_entry_name(p_entry)));
				}
				return 0;
			},
			&r_paths);
}

// The paths where the trees differ.
void tree_diff_paths(git_repository *p_repo, git_tree *p_from, git_tree *p_to, HashSet<String> &r_paths) {
	DiffPtr diff;
	if (git_diff_tree_to_tree(diff.out(), p_repo, p_from, p_to, nullptr) == 0) {
		for (size_t i = 0; i < git_diff_num_deltas(diff); i++) {
			const git_diff_delta *delta = git_diff_get_delta(diff, i);
			r_paths.insert(String::utf8(delta->old_file.path));
			r_paths.insert(String::utf8(delta->new_file.path));
		}
	}
}

// Whether merging p_ours and p_theirs (both from p_base) conflicts, and where.
PackedStringArray merge_conflicts(git_repository *p_repo, git_tree *p_base, git_tree *p_ours, git_tree *p_theirs) {
	PackedStringArray conflicts;
	IndexPtr merged;
	if (git_merge_trees(merged.out(), p_repo, p_base, p_ours, p_theirs, nullptr) < 0) {
		conflicts.push_back("?");
		return conflicts;
	}
	IndexConflictIteratorPtr it;
	if (git_index_has_conflicts(merged) && git_index_conflict_iterator_new(it.out(), merged) == 0) {
		const git_index_entry *ancestor = nullptr;
		const git_index_entry *ours = nullptr;
		const git_index_entry *theirs = nullptr;
		while (git_index_conflict_next(&ancestor, &ours, &theirs, it) == 0) {
			const git_index_entry *entry = ours ? ours : (theirs ? theirs : ancestor);
			conflicts.push_back(String::utf8(entry->path));
		}
	}
	conflicts.sort();
	return conflicts;
}

} // namespace

// The stashes, newest first: [{ "hash", "message" (what it's about: our own stashes name their
// files, "player.gd, level.tscn and 2 more"), "branch" (the branch it was made on), "time" }].
Array GitRepository::get_stashes() const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	for (const StashEntry &entry : list_stashes(repo)) {
		// git's own form: "On main: <message>", or "WIP on main: 1a2b3c4 <last commit's summary>".
		String branch;
		String message = entry.message;
		for (const char *prefix : { "WIP on ", "On " }) {
			if (message.begins_with(prefix) && message.contains(": ")) {
				const String rest = message.trim_prefix(prefix);
				branch = rest.get_slice(": ", 0);
				message = rest.substr(branch.length() + 2);
				break;
			}
		}
		CommitPtr commit;
		Dictionary item;
		item["hash"] = String(git_oid_tostr_s(&entry.id));
		item["message"] = message;
		item["branch"] = branch;
		item["time"] = git_commit_lookup(commit.out(), repo, &entry.id) == 0 ? (int64_t)git_commit_time(commit) : (int64_t)0;
		result.push_back(item);
	}
	return result;
}

// Sets changes aside in a new stash, named p_message, or after its files when that's empty.
// p_staged: exactly what's staged (`git stash push --staged`); otherwise everything, new
// (untracked) files included.
//
// What's staged goes through git itself: libgit2 1.9's path-limited stash (`paths` in
// git_stash_save_options) also resets every other file's unstaged edits without saving them
// anywhere (found 2026-09-29, test_stash.gd). git's --staged stashes exactly the staged changes,
// even of a file that also has unstaged ones, and leaves everything else alone.
Error GitRepository::stash(bool p_staged, const String &p_message) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	if (require_no_operation(repo, "stash") != OK || require_lfs(repo, "stashing") != OK || require_identity(repo, "Nothing was stashed.") != OK) {
		return FAILED;
	}
	PackedStringArray names;
	if (p_staged) {
		for (const Variant &entry : get_status()) {
			if (!String(Dictionary(entry)["index"]).is_empty()) {
				names.push_back(String(Dictionary(entry)["path"]).get_file());
			}
		}
	} else {
		for (const String &path : uncommitted_paths(repo)) {
			if (!names.has(path.get_file())) {
				names.push_back(path.get_file());
			}
		}
	}
	if (names.is_empty()) {
		return fail(p_staged ? "There's nothing staged to stash." : "There's nothing to stash.");
	}
	const String message = p_message.strip_edges().is_empty() ? name_list(names) : p_message.strip_edges().replace("\n", " ");

	if (p_staged) {
		if (require_git("Stashing what's staged needs git (2.35 or newer).") != OK) {
			return FAILED;
		}
		begin_network_operation();
		RemoteContext ctx;
		ctx.progress = progress_callback;
		String output;
		int exit_code = 0;
		const Error err = run_git_command(repo, ctx, PackedStringArray({ "stash", "push", "--staged", "--quiet", "-m", message }), "Stashing...", output, exit_code);
		if (err != OK) {
			return err;
		}
		if (exit_code != 0) {
			const String tail = output_tail(output, 6);
			return fail(tail.contains("--staged") ? String("Stashing what's staged needs git 2.35 or newer; update git from git-scm.com.") : vformat("Git didn't stash:\n%s", tail));
		}
		return OK;
	}

	SignaturePtr stasher;
	git_signature_default(stasher.out(), repo);
	const CharString message_utf8 = message.utf8();
	git_oid id;
	const int err = git_stash_save(&id, repo, stasher, message_utf8.get_data(), GIT_STASH_INCLUDE_UNTRACKED);
	if (err == GIT_ENOTFOUND) {
		return fail("There's nothing to stash.");
	}
	return to_error(err);
}

// Puts stash p_hash's changes back and removes the stash, or refuses and changes nothing: when
// your current edits touch files the stash changes too, or when the stash changes the same lines
// as commits made since. What was staged comes back staged, unless that part conflicts (then it
// comes back unstaged, and get_notice() says so).
Error GitRepository::restore_stash(const String &p_hash) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	notice = String();
	if (require_no_operation(repo, "restore a stash") != OK || require_lfs(repo, "restoring a stash") != OK) {
		return FAILED;
	}
	size_t index = 0;
	CommitPtr stash_commit, base_commit, index_commit, untracked_commit;
	TreePtr stash_tree, base_tree, index_tree, untracked_tree;
	ObjectPtr head; // HEAD's tree.
	git_oid stash_id;
	if (!find_stash(repo, p_hash, index) || git_oid_fromstr(&stash_id, p_hash.utf8().get_data()) < 0 || git_commit_lookup(stash_commit.out(), repo, &stash_id) < 0) {
		return fail("That stash doesn't exist anymore.");
	}
	if (git_commit_tree(stash_tree.out(), stash_commit) < 0 || git_commit_parent(base_commit.out(), stash_commit, 0) < 0 || git_commit_tree(base_tree.out(), base_commit) < 0 ||
			git_commit_parent(index_commit.out(), stash_commit, 1) < 0 || git_commit_tree(index_tree.out(), index_commit) < 0 ||
			git_revparse_single(head.out(), repo, "HEAD^{tree}") < 0) {
		return FAILED;
	}
	git_tree *head_tree = (git_tree *)head.get();
	const bool has_untracked = git_commit_parentcount(stash_commit) > 2 && git_commit_parent(untracked_commit.out(), stash_commit, 2) == 0 && git_commit_tree(untracked_tree.out(), untracked_commit) == 0;

	// Your edits to the same files: git would refuse, or mix them. Named, nothing touched.
	HashSet<String> touched;
	tree_diff_paths(repo, base_tree, stash_tree, touched);
	tree_diff_paths(repo, base_tree, index_tree, touched);
	HashSet<String> untracked;
	if (has_untracked) {
		tree_paths(untracked_tree, untracked);
	}
	const String workdir = get_workdir();
	IndexPtr index_now;
	git_repository_index(index_now.out(), repo);
	// A new .uid or .import that git doesn't track yet is Godot's, not yours: the editor writes a
	// fresh one the moment a stash takes the old one away. The stashed one replaces it (it's the
	// uid scenes refer to), so it doesn't block. Tracked ones may hold your import settings.
	auto generated_by_godot = [&](const String &p_path) {
		return (p_path.ends_with(".uid") || p_path.ends_with(".import")) && !has_file_at("HEAD", p_path) &&
				!(index_now && git_index_get_bypath(index_now, p_path.utf8().get_data(), 0));
	};
	PackedStringArray busy;
	PackedStringArray regenerated;
	for (const String &path : uncommitted_paths(repo)) {
		if ((touched.has(path) || untracked.has(path)) && !busy.has(path) && !regenerated.has(path)) {
			(generated_by_godot(path) ? regenerated : busy).push_back(path);
		}
	}
	for (const String &path : untracked) {
		if (!busy.has(path) && !regenerated.has(path) && FileAccess::file_exists(workdir.path_join(path))) {
			// An ignored file of the same name would be overwritten.
			(generated_by_godot(path) ? regenerated : busy).push_back(path);
		}
	}
	if (!busy.is_empty()) {
		busy.sort();
		return fail(vformat("Nothing was restored: you have uncommitted changes to %s, which the stash changes too. Commit, stash or discard them first.", name_list(busy)));
	}

	// Commits made since the stash that change the same lines: a restore would conflict.
	const PackedStringArray conflicts = merge_conflicts(repo, base_tree, head_tree, stash_tree);
	if (!conflicts.is_empty()) {
		return fail(vformat("Nothing was restored: the stash changes the same lines as commits made since, in %s. Restoring it would need resolving conflicts, which the panel can't do yet.", name_list(conflicts)));
	}
	const bool staged_fits = merge_conflicts(repo, base_tree, head_tree, index_tree).is_empty();

	// Everything checks out: Godot's regenerated files make way for the stashed ones.
	for (const String &path : regenerated) {
		DirAccess::remove_absolute(workdir.path_join(path));
	}

	git_stash_apply_options opts = GIT_STASH_APPLY_OPTIONS_INIT;
	opts.flags = staged_fits ? GIT_STASH_APPLY_REINSTATE_INDEX : GIT_STASH_APPLY_DEFAULT;
	opts.checkout_options.checkout_strategy = GIT_CHECKOUT_SAFE;
	const int err = git_stash_apply(repo, index, &opts);
	if (err < 0) {
		return fail(vformat("The stash couldn't be restored, and it's kept. %s", last_git_error()));
	}
	if (!staged_fits) {
		notice = "Restored the stash. What was staged in it came back unstaged: it no longer fits the staged state since new commits.";
	}
	return to_error(git_stash_drop(repo, index));
}

// Deletes stash p_hash for good.
Error GitRepository::delete_stash(const String &p_hash) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	size_t index = 0;
	if (!find_stash(repo, p_hash, index)) {
		return fail("That stash doesn't exist anymore.");
	}
	return to_error(git_stash_drop(repo, index));
}
