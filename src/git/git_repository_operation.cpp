// GitRepository: git operations left in progress, usually started in a terminal and stopped at
// conflicts (a merge, rebase, cherry-pick, revert, `git am`, or a bisect). The panel shows them
// and can abort or continue them; both go through git itself, because libgit2 can't abort a
// merge without also throwing away changes made before it, and can't continue a rebase at all.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/file_access.hpp>

#include "git/git_cli.h"
#include "git/git_remote_callbacks.h"
#include "git/git_util.h"

using namespace godot_git;

namespace {

String read_git_file(git_repository *p_repo, const String &p_name) {
	const String path = String::utf8(git_repository_path(p_repo)).path_join(p_name);
	return FileAccess::file_exists(path) ? FileAccess::get_file_as_string(path) : String();
}

// "1a2b3c4 Fix the jump", for the commit a special ref (CHERRY_PICK_HEAD, REVERT_HEAD) names.
String describe_ref(git_repository *p_repo, const char *p_ref) {
	git_oid oid;
	CommitPtr commit;
	if (git_reference_name_to_id(&oid, p_repo, p_ref) < 0 || git_commit_lookup(commit.out(), p_repo, &oid) < 0) {
		return String();
	}
	char hash[8] = {};
	git_oid_tostr(hash, sizeof(hash), &oid);
	const char *summary = git_commit_summary(commit);
	return vformat("%s %s", String(hash), summary ? String::utf8(summary) : String());
}

// What the operation is about: the merge's message ("Merge branch 'feature'"), the commit being
// cherry-picked or reverted, or the branch being rebased.
String operation_subject(git_repository *p_repo, const String &p_kind) {
	if (p_kind == "merge") {
		return read_git_file(p_repo, "MERGE_MSG").get_slice("\n", 0).strip_edges();
	}
	if (p_kind == "cherry-pick") {
		return describe_ref(p_repo, "CHERRY_PICK_HEAD");
	}
	if (p_kind == "revert") {
		return describe_ref(p_repo, "REVERT_HEAD");
	}
	if (p_kind == "rebase") {
		String head = read_git_file(p_repo, "rebase-merge/head-name").strip_edges();
		if (head.is_empty()) {
			head = read_git_file(p_repo, "rebase-apply/head-name").strip_edges();
		}
		return head.trim_prefix("refs/heads/");
	}
	return String();
}

} // namespace

// The operation the repository is in the middle of:
// { "kind": "" (none) | "merge" | "rebase" | "cherry-pick" | "revert" | "apply" | "bisect",
//   "subject": what it's about (see operation_subject), "conflicts": the conflicted paths }.
Dictionary GitRepository::get_operation() const {
	Dictionary result;
	result["kind"] = String();
	result["subject"] = String();
	result["conflicts"] = PackedStringArray();
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	const String kind = operation_in_progress(repo);
	if (kind.is_empty()) {
		return result;
	}
	result["kind"] = kind;
	result["subject"] = operation_subject(repo, kind);

	PackedStringArray conflicts;
	IndexPtr index;
	IndexConflictIteratorPtr it;
	if (git_repository_index(index.out(), repo) == 0 && git_index_conflict_iterator_new(it.out(), index) == 0) {
		const git_index_entry *ancestor = nullptr;
		const git_index_entry *ours = nullptr;
		const git_index_entry *theirs = nullptr;
		while (git_index_conflict_next(&ancestor, &ours, &theirs, it) == 0) {
			const git_index_entry *entry = ours ? ours : (theirs ? theirs : ancestor);
			conflicts.push_back(String::utf8(entry->path));
		}
	}
	result["conflicts"] = conflicts;
	return result;
}

// Abandons the operation in progress, the way git does it (`git merge --abort`, ...): the branch
// and files go back to how they were before it started.
Error GitRepository::abort_operation() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	const String kind = operation_in_progress(repo);
	if (kind.is_empty()) {
		return fail("There's nothing in progress to abort.");
	}
	PackedStringArray args;
	if (kind == "bisect") {
		args = PackedStringArray({ "bisect", "reset" });
	} else {
		args = PackedStringArray({ kind == "apply" ? String("am") : kind, "--abort" });
	}
	return _run_operation_step(args, "Aborting...");
}

// Finishes the operation once its conflicts are resolved: commits the merge (with its prepared
// message), or lets a rebase, cherry-pick, revert or `git am` go on to its next step, where it
// may stop at conflicts again.
Error GitRepository::continue_operation() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	const String kind = operation_in_progress(repo);
	if (kind.is_empty() || kind == "bisect") {
		return fail("There's nothing in progress to continue.");
	}
	const PackedStringArray conflicts = get_operation()["conflicts"];
	if (!conflicts.is_empty()) {
		const String names = String(", ").join(conflicts.slice(0, 3)) + (conflicts.size() > 3 ? String(", ...") : String());
		return fail(vformat("%s still %s conflicts. Fix the conflict markers, then stage the file to mark it resolved.", names, conflicts.size() == 1 ? "has" : "have"));
	}
	if (require_identity(repo, "Nothing was continued.") != OK) {
		return FAILED;
	}
	const PackedStringArray args = kind == "merge" ? PackedStringArray({ "commit", "--no-edit" }) : PackedStringArray({ kind == "apply" ? String("am") : kind, "--continue" });
	return _run_operation_step(args, "Continuing...");
}

// Runs one `git <p_args>` step of abort / continue. Git would open an editor for a commit message
// (rebase, cherry-pick and revert do); with no terminal, the prepared message is used as it is.
Error GitRepository::_run_operation_step(const PackedStringArray &p_args, const String &p_step) {
	if (require_git("Finishing or aborting it needs git.") != OK) {
		return FAILED;
	}
	begin_network_operation();
	RemoteContext ctx;
	ctx.progress = progress_callback;
	String output;
	int exit_code = 0;
	const Error err = run_git_command(repo, ctx, p_args, p_step, output, exit_code, true);
	if (err != OK) {
		return err;
	}
	if (exit_code != 0) {
		const String tail = output_tail(output, 8);
		return fail(tail.is_empty() ? String("Git refused, without saying why.") : vformat("Git refused:\n%s", tail));
	}
	return OK;
}
