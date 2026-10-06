// GitRepository: what History offers on a commit: a file's version back, the last commit undone, a
// commit reverted, a branch at a commit, and the commit a line comes from.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/templates/vector.hpp>

#include <cstring>

#include "git/git_lfs.h"
#include "git/git_util.h"

using namespace godot_git;

namespace {

// The commit p_revision names ("abc123", "abc123^1"), or null.
bool lookup_commit(git_repository *p_repo, const String &p_revision, CommitPtr &r_commit) {
	ObjectPtr object;
	if (git_revparse_single(object.out(), p_repo, vformat("%s^{commit}", p_revision).utf8().get_data()) < 0) {
		return false;
	}
	return git_commit_lookup(r_commit.out(), p_repo, git_object_id(object)) == 0;
}

} // namespace

// Writes p_path as it was in p_revision (a commit, or "<commit>^1" for the version before it)
// into the working folder, as an uncommitted change nothing stages. A file p_revision doesn't
// have is deleted (restoring to before the commit that added it). Refused while p_path has
// uncommitted changes, which it would overwrite.
Error GitRepository::restore_file_version(const String &p_revision, const String &p_path) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	if (require_lfs(repo, "restoring files") != OK) {
		return FAILED;
	}
	if (uncommitted_paths(repo).has(p_path)) {
		return fail(vformat("%s has uncommitted changes, which restoring would overwrite. Commit, stash or discard them first.", p_path.get_file()));
	}
	CommitPtr commit;
	TreePtr tree;
	if (!lookup_commit(repo, p_revision, commit) || git_commit_tree(tree.out(), commit) < 0) {
		return fail("That version isn't in this repository.");
	}
	const String absolute = get_workdir().path_join(p_path);
	BlobPtr blob;
	if (!read_tree_blob(repo, tree, p_path, blob)) {
		if (!FileAccess::file_exists(absolute)) {
			return OK; // Not there, and not there now either.
		}
		return DirAccess::remove_absolute(absolute) == OK ? OK : fail(vformat("Couldn't delete %s. Is it open in another program?", p_path.get_file()));
	}
	// Through the same filters a checkout uses (CRLF, LFS), so the file is what git would write.
	std::string content;
	const std::string raw = blob_text(blob);
	if (!apply_filters(repo, p_path, raw.data(), raw.size(), GIT_FILTER_TO_WORKTREE, content)) {
		return fail(vformat("Couldn't prepare %s: %s", p_path.get_file(), get_last_error()));
	}
	DirAccess::make_dir_recursive_absolute(absolute.get_base_dir());
	if (!write_file(absolute, content.data(), content.size())) {
		return fail(vformat("Couldn't write %s. Is it open in another program?", p_path.get_file()));
	}
	return OK;
}

// Takes back the last commit: the branch moves back one commit and its changes wait, staged, to
// be committed again (git reset --soft HEAD~1). Only while the commit isn't pushed (like Amend),
// and not a merge or the very first commit.
Error GitRepository::undo_last_commit() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	if (require_no_operation(repo, "undo a commit") != OK) {
		return FAILED;
	}
	CommitPtr head;
	if (!lookup_commit(repo, "HEAD", head)) {
		return fail("There's no commit to undo.");
	}
	if (is_head_pushed()) {
		return fail("The last commit is already pushed, so undoing it here would leave your teammates with a commit you no longer have. Revert it instead.");
	}
	if (git_commit_parentcount(head) > 1) {
		return fail("The last commit is a merge. Undoing a merge isn't supported here.");
	}
	if (git_commit_parentcount(head) == 0) {
		return fail("That's the first commit in this repository, so there's nothing to go back to.");
	}
	CommitPtr parent;
	if (git_commit_parent(parent.out(), head, 0) < 0) {
		return to_error(-1);
	}
	return to_error(git_reset(repo, (git_object *)parent.get(), GIT_RESET_SOFT, nullptr));
}

namespace {

// What the newest entry of HEAD's reflog did, as far as Undo is concerned (see get_undo).
struct LastOperation {
	String kind; // "commit", "amend", "pull", "merge", "switch"; "" when there's nothing to undo.
	git_oid before = {}; // HEAD before it.
	git_oid after = {}; // HEAD after it (the current HEAD).
	String previous_branch; // "switch": the branch it left.
	String merged; // "merge": what was merged in, when the reflog or message says.
};

// "Merge branch 'feature'" / "Merge remote-tracking branch 'origin/x'" -> "feature" / "origin/x".
String merged_name(const String &p_summary) {
	const int open = p_summary.find("'");
	const int close = open < 0 ? -1 : p_summary.find("'", open + 1);
	return close > open ? p_summary.substr(open + 1, close - open - 1) : String();
}

LastOperation last_operation(git_repository *p_repo) {
	LastOperation op;
	ReflogPtr reflog;
	git_oid head;
	if (git_reflog_read(reflog.out(), p_repo, "HEAD") < 0 || git_reflog_entrycount(reflog) == 0 || git_reference_name_to_id(&head, p_repo, "HEAD") < 0) {
		return op;
	}
	const git_reflog_entry *entry = git_reflog_entry_byindex(reflog, 0);
	const char *raw = git_reflog_entry_message(entry);
	const String message = raw ? String::utf8(raw) : String();
	op.before = *git_reflog_entry_id_old(entry);
	op.after = *git_reflog_entry_id_new(entry);
	if (git_oid_cmp(&op.after, &head) != 0 || git_oid_is_zero(&op.before)) {
		return op; // Not what made HEAD what it is, or there was nothing before it.
	}

	if (message.begins_with("checkout: moving from ")) {
		// "checkout: moving from main to feature": back to main, if it's still a branch here.
		const String rest = message.trim_prefix("checkout: moving from ");
		const int to = rest.rfind(" to ");
		const String from = to < 0 ? String() : rest.substr(0, to);
		ReferencePtr branch, current;
		if (!from.is_empty() && git_branch_lookup(branch.out(), p_repo, from.utf8().get_data(), GIT_BRANCH_LOCAL) == 0 &&
				(head_branch(current.out(), p_repo) < 0 || String::utf8(git_reference_shorthand(current)) != from)) {
			op.kind = "switch";
			op.previous_branch = from;
		}
		return op;
	}

	CommitPtr commit;
	if (git_commit_lookup(commit.out(), p_repo, &head) < 0) {
		return op;
	}
	const unsigned int parents = git_commit_parentcount(commit);
	const bool first_parent_is_before = parents > 0 && git_oid_cmp(git_commit_parent_id(commit, 0), &op.before) == 0;
	if (message.begins_with("commit (amend)")) {
		op.kind = "amend";
	} else if (message.begins_with("commit (initial)")) {
		return op;
	} else if (message.begins_with("commit") && first_parent_is_before) {
		op.kind = parents > 1 ? "merge" : "commit";
		if (parents > 1) {
			op.merged = merged_name(String::utf8(git_commit_summary(commit)));
		}
	} else if (message.begins_with("pull") || message.begins_with("merge ")) {
		// A fast-forward, or a merge commit on top of where HEAD was.
		const bool forward = git_graph_descendant_of(p_repo, &op.after, &op.before) == 1;
		if (forward || first_parent_is_before) {
			op.kind = message.begins_with("pull") ? "pull" : "merge";
			if (op.kind == "merge") {
				op.merged = message.trim_prefix("merge ").get_slice(":", 0);
			}
		}
	}
	return op;
}

String short_id(const git_oid *p_id) {
	return String(git_oid_tostr_s(p_id)).left(7);
}

} // namespace

// What Undo would undo: the last thing that moved HEAD (from its reflog, so a commit, pull, merge
// or switch made in a terminal counts too). { } when there's nothing it can undo, else
// { "kind": "commit" | "amend" | "pull" | "merge" | "switch", "label": "Undo Pull" (a menu item),
//   "detail": what undoing does, in a sentence or two, "reason": why it can't be undone now ("" when
//   it can), "branch": the branch a switch would go back to }.
// Undo never touches uncommitted changes, and doesn't take back what's pushed (Revert does that).
Dictionary GitRepository::get_undo() {
	ERR_FAIL_NULL_V_MSG(repo, Dictionary(), "Repository is not open.");
	const LastOperation op = last_operation(repo);
	Dictionary result;
	if (op.kind.is_empty()) {
		return result;
	}
	result["kind"] = op.kind;
	ReferencePtr head;
	const String branch = head_branch(head.out(), repo) >= 0 ? String::utf8(git_reference_shorthand(head)) : String("HEAD");
	CommitPtr after, before;
	git_commit_lookup(after.out(), repo, &op.after);
	git_commit_lookup(before.out(), repo, &op.before);
	const String after_summary = after.get() ? String::utf8(git_commit_summary(after)) : String();
	const String before_summary = before.get() ? String::utf8(git_commit_summary(before)) : String();
	String reason;

	if (op.kind == "switch") {
		result["label"] = vformat("Switch Back to %s", op.previous_branch);
		result["detail"] = vformat("Back to %s, the branch you were on before %s. Your uncommitted changes come along, as when you switch yourself.", op.previous_branch, branch);
		result["branch"] = op.previous_branch;
	} else if (op.kind == "commit") {
		result["label"] = "Undo Commit";
		result["detail"] = vformat("Takes back \"%s\": its changes go back to Staged Changes, and its message into the message box.", after_summary);
		if (on_remote_branch(repo, &op.after)) {
			reason = "It's already pushed: undoing it here would leave your teammates with a commit you no longer have. Revert it instead (right-click it in History).";
		}
	} else if (op.kind == "amend") {
		result["label"] = "Undo Amend";
		result["detail"] = vformat("Puts the commit back as it was before you amended it (\"%s\"). What the amend added goes back to Staged Changes.", before_summary);
		if (on_remote_branch(repo, &op.after)) {
			reason = "The amended commit is already pushed, so taking it back here would leave your teammates with a commit you no longer have.";
		}
	} else {
		size_t ahead = 0, behind = 0;
		git_graph_ahead_behind(&ahead, &behind, repo, &op.after, &op.before);
		if (ahead > 1 && after.get() && git_commit_parentcount(after) > 1) {
			ahead--; // The merge commit itself isn't one it brought in ("Pulled and merged 1 commit").
		}
		const String what = op.kind == "pull" ? String("the pull") : (op.merged.is_empty() ? String("the merge") : vformat("merging %s", op.merged));
		result["label"] = op.kind == "pull" ? "Undo Pull" : "Undo Merge";
		result["detail"] = vformat("Moves %s back to where it was before %s (%s \"%s\"), taking back the %s it brought in. Your uncommitted changes stay where they are.%s", branch, what, short_id(&op.before), before_summary,
				ahead == 1 ? String("commit") : vformat("%d commits", (int)ahead), op.kind == "pull" ? " You can pull again any time." : (op.merged.is_empty() ? "" : vformat(" %s itself stays as it is.", op.merged)));
		// A merge commit made here and pushed since can't be taken back here; a fast-forward only
		// brought in commits that are on the remote anyway.
		if (git_oid_cmp(&op.after, &op.before) != 0 && after.get() && git_commit_parentcount(after) > 1 && on_remote_branch(repo, &op.after)) {
			reason = vformat("The merge commit is already pushed, so taking %s back here would leave your teammates with a commit you no longer have.", what);
		} else {
			// Only files the pull or merge changed are put back; your changes to them would be lost.
			const HashSet<String> changed = changed_paths(repo, &op.before, &op.after);
			PackedStringArray in_the_way;
			for (const String &path : uncommitted_paths(repo)) {
				if (changed.has(path)) {
					in_the_way.push_back(path);
				}
			}
			if (!in_the_way.is_empty()) {
				reason = vformat("You have uncommitted changes to %s, which %s changed. Commit, stash or discard them first.", name_list(in_the_way), what);
			}
		}
	}
	const Dictionary operation = get_operation();
	if (!String(operation.get("kind", String())).is_empty()) {
		reason = "Finish or abort what's in progress first.";
	}
	result["reason"] = reason;
	return result;
}

// Undoes what get_undo describes. Changes nothing when it can't (get_last_error says why).
Error GitRepository::undo_last_operation() {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	const Dictionary undo = get_undo();
	if (undo.is_empty()) {
		return fail("There's nothing to undo.");
	}
	if (!String(undo["reason"]).is_empty()) {
		return fail(undo["reason"]);
	}
	const String kind = undo["kind"];
	if (kind == "switch") {
		return checkout_branch(undo["branch"]);
	}
	const LastOperation op = last_operation(repo);
	ObjectPtr before;
	if (git_object_lookup(before.out(), repo, &op.before, GIT_OBJECT_COMMIT) < 0) {
		return to_error(-1);
	}
	if (kind == "commit" || kind == "amend") {
		// What it committed goes back to staged: only the branch moves.
		return to_error(git_reset(repo, before, GIT_RESET_SOFT, nullptr));
	}
	if (require_lfs(repo, "undoing it") != OK) {
		return FAILED;
	}
	// A pull or merge: the files it changed go back too. Safe checkout, all or nothing; your other
	// uncommitted changes are left alone (get_undo refused if any were in its files).
	ReferencePtr head;
	if (head_branch(head.out(), repo) < 0) {
		return to_error(-1);
	}
	git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
	opts.checkout_strategy = GIT_CHECKOUT_SAFE;
	int err = checkout_all_or_nothing(repo, before, opts);
	if (err == GIT_ECONFLICT) {
		return fail("Nothing was undone: your uncommitted changes are in files it would change. Commit, stash or discard them first.");
	}
	if (err >= 0) {
		ReferencePtr moved;
		err = git_reference_set_target(moved.out(), head, &op.before, vformat("reset: %s", String(undo["label"]).to_lower()).utf8().get_data());
	}
	return to_error(err);
}

// Adds a commit that undoes p_hash (for a merge, what it brought in), with git's message
// ("Revert "..."" / "This reverts commit ..."). Changes nothing if it can't be done cleanly: when
// newer commits changed the same lines, or when you have uncommitted changes in its files or
// anything staged (the revert commit would take those along).
Error GitRepository::revert_commit(const String &p_hash) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	if (require_no_operation(repo, "revert") != OK || require_lfs(repo, "reverting") != OK || require_identity(repo, "Nothing was reverted.") != OK) {
		return FAILED;
	}
	CommitPtr target, head;
	if (!lookup_commit(repo, p_hash, target) || !lookup_commit(repo, "HEAD", head)) {
		return fail("That commit isn't in this repository.");
	}

	IndexPtr reverted;
	git_merge_options merge_opts = GIT_MERGE_OPTIONS_INIT;
	const unsigned int mainline = git_commit_parentcount(target) > 1 ? 1 : 0;
	int err = git_revert_commit(reverted.out(), repo, target, head, mainline, &merge_opts);
	if (err < 0) {
		return to_error(err);
	}
	if (git_index_has_conflicts(reverted)) {
		PackedStringArray conflicted;
		IndexConflictIteratorPtr it;
		const git_index_entry *ancestor, *ours, *theirs;
		if (git_index_conflict_iterator_new(it.out(), reverted) == 0) {
			while (git_index_conflict_next(&ancestor, &ours, &theirs, it) == 0) {
				const git_index_entry *entry = ours ? ours : (theirs ? theirs : ancestor);
				conflicted.push_back(String::utf8(entry->path));
			}
		}
		return fail(vformat("Nothing was reverted: newer commits changed the same lines of %s, so undoing this commit would need resolving by hand.", name_list(conflicted)));
	}
	git_oid tree_id;
	if ((err = git_index_write_tree_to(&tree_id, reverted, repo)) < 0) {
		return to_error(err);
	}
	TreePtr head_tree, new_tree;
	if (git_commit_tree(head_tree.out(), head) < 0 || git_tree_lookup(new_tree.out(), repo, &tree_id) < 0) {
		return to_error(-1);
	}
	DiffPtr diff;
	if ((err = git_diff_tree_to_tree(diff.out(), repo, head_tree, new_tree, nullptr)) < 0) {
		return to_error(err);
	}
	PackedStringArray paths;
	for (size_t i = 0; i < git_diff_num_deltas(diff); i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);
		paths.push_back(String::utf8(delta->new_file.path));
		if (strcmp(delta->old_file.path, delta->new_file.path) != 0) {
			paths.push_back(String::utf8(delta->old_file.path));
		}
	}
	if (paths.is_empty()) {
		return fail("Nothing to revert: its changes are already undone.");
	}

	// Your own work stays yours: nothing staged (it would go into the revert commit), and no
	// uncommitted changes in the files it touches.
	StatusListPtr status;
	git_status_options status_opts = GIT_STATUS_OPTIONS_INIT;
	status_opts.show = GIT_STATUS_SHOW_INDEX_ONLY;
	if (git_status_list_new(status.out(), repo, &status_opts) == 0 && git_status_list_entrycount(status) > 0) {
		return fail("Nothing was reverted: you have staged changes, which the revert commit would take along. Commit or unstage them first.");
	}
	PackedStringArray busy;
	const PackedStringArray uncommitted = uncommitted_paths(repo);
	for (const String &path : paths) {
		if (uncommitted.has(path)) {
			busy.push_back(path);
		}
	}
	if (!busy.is_empty()) {
		return fail(vformat("Nothing was reverted: you have uncommitted changes to %s, which this commit changed too. Commit, stash or discard them first.", name_list(busy)));
	}

	// Its files as the revert has them, in the working folder and staged; then an ordinary commit
	// (hooks and signing included). If that fails, the files go back.
	Vector<CharString> path_data;
	Vector<char *> path_pointers;
	for (const String &path : paths) {
		path_data.push_back(path.utf8());
	}
	for (CharString &data : path_data) {
		path_pointers.push_back(data.ptrw());
	}
	git_checkout_options checkout = GIT_CHECKOUT_OPTIONS_INIT;
	checkout.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;
	checkout.paths.strings = path_pointers.ptrw();
	checkout.paths.count = path_pointers.size();
	if ((err = checkout_all_or_nothing(repo, (git_object *)new_tree.get(), checkout)) < 0) {
		return to_error(err);
	}
	const String summary = String::utf8(git_commit_summary(target));
	const String message = vformat("Revert \"%s\"\n\nThis reverts commit %s.\n", summary, String(git_oid_tostr_s(git_commit_id(target))));
	const Error committed = commit(message);
	if (committed != OK) {
		const String reason = get_last_error();
		git_oid now;
		if (git_reference_name_to_id(&now, repo, "HEAD") == 0 && !git_oid_equal(&now, git_commit_id(head))) {
			// The commit was made; a hook after it (post-commit) failed or was canceled. The files
			// are right as they are.
			return OK;
		}
		checkout.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;
		checkout_all_or_nothing(repo, (git_object *)head_tree.get(), checkout);
		return fail(vformat("Nothing was reverted: %s", reason));
	}
	return OK;
}

// Creates a branch at p_hash without switching to it.
Error GitRepository::create_branch_at(const String &p_name, const String &p_hash) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	int valid = 0;
	if (git_branch_name_is_valid(&valid, p_name.utf8().get_data()) < 0 || !valid) {
		return fail(vformat("\"%s\" isn't a valid branch name.", p_name));
	}
	CommitPtr commit;
	if (!lookup_commit(repo, p_hash, commit)) {
		return fail("That commit isn't in this repository.");
	}
	ReferencePtr ref;
	const int err = git_branch_create(ref.out(), repo, p_name.utf8().get_data(), commit, 0);
	if (err == GIT_EEXISTS) {
		return fail(vformat("A branch called \"%s\" already exists.", p_name));
	}
	return to_error(err);
}

// The commit a line of p_path last changed in, with p_text as the file's current content (the
// editor's, unsaved edits included) and p_line 0-based: { "hash", "id", "summary", "author",
// "time" }, or {} when the line isn't committed yet (or the file isn't).
Dictionary GitRepository::get_line_commit(const String &p_path, const String &p_text, int p_line) const {
	Dictionary result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	git_blame_options opts = GIT_BLAME_OPTIONS_INIT;
	BlamePtr file_blame, buffer_blame;
	if (git_blame_file(file_blame.out(), repo, p_path.utf8().get_data(), &opts) < 0) {
		git_error_clear();
		return result;
	}
	const CharString text = p_text.replace("\r\n", "\n").utf8();
	if (git_blame_buffer(buffer_blame.out(), file_blame, text.get_data(), text.length()) < 0) {
		git_error_clear();
		return result;
	}
	const git_blame_hunk *hunk = git_blame_get_hunk_byline(buffer_blame, p_line + 1);
	if (!hunk || git_oid_is_zero(&hunk->final_commit_id)) {
		return result; // Changed since the last commit.
	}
	CommitPtr commit;
	if (git_commit_lookup(commit.out(), repo, &hunk->final_commit_id) < 0) {
		return result;
	}
	const String hash = String(git_oid_tostr_s(&hunk->final_commit_id));
	const git_signature *author = git_commit_author(commit);
	result["hash"] = hash;
	result["id"] = hash.left(7);
	result["summary"] = String::utf8(git_commit_summary(commit));
	result["author"] = author ? String::utf8(author->name) : String();
	result["time"] = (int64_t)git_commit_time(commit);
	return result;
}
