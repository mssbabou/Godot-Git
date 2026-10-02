// GitRepository: resolving conflicts. A conflicted file is read from the index's three versions
// (base, mine, theirs), not from the file on disk, which holds git's conflict markers and maybe
// edits; the result is written through the worktree filters and staged, which is what marks a
// conflict resolved.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>

#include <string>

#include "git/git_lfs.h"
#include "git/git_util.h"

using namespace godot_git;

namespace {

// Long enough that no line of a real file starts with it, so the merged text can be split
// back into its parts without guessing.
constexpr int MARKER_SIZE = 40;

// The three versions of a conflicted path (any may be missing: added on both sides, deleted on
// one), with "mine" and "theirs" as the person resolving sees them.
struct ConflictSides {
	BlobPtr base, mine, theirs;
	bool found = false;
};

// During a rebase git's "ours" (stage 2) is the branch being rebased onto and "theirs" (stage 3)
// is your own commit being replayed; everywhere else "ours" is yours. Mine is always yours.
bool sides_swapped(git_repository *p_repo) {
	return operation_in_progress(p_repo) == "rebase";
}

void read_sides(git_repository *p_repo, const String &p_path, ConflictSides &sides) {
	IndexPtr index;
	if (git_repository_index(index.out(), p_repo) < 0) {
		return;
	}
	const git_index_entry *ancestor = nullptr;
	const git_index_entry *ours = nullptr;
	const git_index_entry *theirs = nullptr;
	if (git_index_conflict_get(&ancestor, &ours, &theirs, index, p_path.utf8().get_data()) < 0) {
		return;
	}
	sides.found = true;
	if (sides_swapped(p_repo)) {
		std::swap(ours, theirs);
	}
	if (ancestor) {
		git_blob_lookup(sides.base.out(), p_repo, &ancestor->id);
	}
	if (ours) {
		git_blob_lookup(sides.mine.out(), p_repo, &ours->id);
	}
	if (theirs) {
		git_blob_lookup(sides.theirs.out(), p_repo, &theirs->id);
	}
}

String blob_text(git_blob *p_blob) {
	return p_blob ? String::utf8((const char *)git_blob_rawcontent(p_blob), (int)git_blob_rawsize(p_blob)) : String();
}

git_merge_file_input merge_input(git_blob *p_blob, const CharString &p_path) {
	git_merge_file_input input = GIT_MERGE_FILE_INPUT_INIT;
	if (p_blob) {
		input.ptr = (const char *)git_blob_rawcontent(p_blob);
		input.size = (size_t)git_blob_rawsize(p_blob);
	}
	input.path = p_path.get_data();
	return input;
}

// Splits git's diff3-style merge of the three versions into blocks: text both sides agree on
// (non-overlapping changes already merged in), and conflicts with each side's text.
Array split_merged(const String &p_merged) {
	Array blocks;
	const String open = String("<").repeat(MARKER_SIZE);
	const String base_mark = String("|").repeat(MARKER_SIZE);
	const String middle = String("=").repeat(MARKER_SIZE);
	const String close = String(">").repeat(MARKER_SIZE);
	auto is_marker = [](const String &p_line, const String &p_marker) {
		return p_line.begins_with(p_marker) && (p_line.length() == p_marker.length() || p_line[p_marker.length()] == ' ' || p_line[p_marker.length()] == '\n' || p_line[p_marker.length()] == '\r');
	};

	String same, mine, base, theirs;
	enum { SAME,
		MINE,
		BASE,
		THEIRS } part = SAME;
	int from = 0;
	while (from < p_merged.length()) {
		int end = p_merged.find("\n", from);
		end = end < 0 ? p_merged.length() : end + 1;
		const String line = p_merged.substr(from, end - from);
		from = end;
		if (part == SAME && is_marker(line, open)) {
			if (!same.is_empty()) {
				Dictionary block;
				block["kind"] = "same";
				block["text"] = same;
				blocks.push_back(block);
				same = String();
			}
			part = MINE;
		} else if (part == MINE && is_marker(line, base_mark)) {
			part = BASE;
		} else if ((part == MINE || part == BASE) && is_marker(line, middle)) {
			part = THEIRS;
		} else if (part == THEIRS && is_marker(line, close)) {
			Dictionary block;
			block["kind"] = "conflict";
			block["mine"] = mine;
			block["base"] = base;
			block["theirs"] = theirs;
			blocks.push_back(block);
			mine = base = theirs = String();
			part = SAME;
		} else {
			String &target = part == SAME ? same : part == MINE ? mine
					: part == BASE								? base
																: theirs;
			target += line;
		}
	}
	if (!same.is_empty()) {
		Dictionary block;
		block["kind"] = "same";
		block["text"] = same;
		blocks.push_back(block);
	}
	return blocks;
}

// A commit as people know it: a branch pointing at it (local first), else its short hash.
String commit_name(git_repository *p_repo, const git_oid &p_oid) {
	for (git_branch_t type : { GIT_BRANCH_LOCAL, GIT_BRANCH_REMOTE }) {
		BranchIteratorPtr it;
		if (git_branch_iterator_new(it.out(), p_repo, type) < 0) {
			continue;
		}
		git_reference *ref = nullptr;
		git_branch_t found;
		while (git_branch_next(&ref, &found, it) == 0) {
			ReferencePtr owned;
			*owned.out() = ref;
			const git_oid *target = git_reference_target(ref);
			if (target && git_oid_equal(target, &p_oid) && !String::utf8(git_reference_shorthand(ref)).ends_with("/HEAD")) {
				return String::utf8(git_reference_shorthand(ref));
			}
		}
	}
	char hash[8] = {};
	git_oid_tostr(hash, sizeof(hash), &p_oid);
	return String(hash);
}

// The commit named in one of git's state files (.git/MERGE_HEAD, .git/rebase-merge/onto, ...).
String state_file_commit(git_repository *p_repo, const char *p_file) {
	const String text = FileAccess::get_file_as_string(String::utf8(git_repository_path(p_repo)).path_join(p_file)).strip_edges();
	git_oid oid;
	if (text.length() < 40 || git_oid_fromstr(&oid, text.left(40).utf8().get_data()) < 0) {
		return String();
	}
	return commit_name(p_repo, oid);
}

String head_name(git_repository *p_repo) {
	ReferencePtr head;
	if (git_repository_head(head.out(), p_repo) < 0) {
		return String();
	}
	return git_reference_is_branch(head) ? String::utf8(git_reference_shorthand(head)) : commit_name(p_repo, *git_reference_target(head));
}

// What mine and theirs are called in the resolver: branch names where there are some.
void side_labels(git_repository *p_repo, const String &p_kind, String &r_mine, String &r_theirs) {
	r_mine = head_name(p_repo);
	if (p_kind == "merge") {
		r_theirs = state_file_commit(p_repo, "MERGE_HEAD");
	} else if (p_kind == "rebase") {
		// Your branch is being replayed onto another; HEAD is detached on the other one.
		const String branch = FileAccess::get_file_as_string(String::utf8(git_repository_path(p_repo)).path_join("rebase-merge/head-name")).strip_edges();
		r_mine = branch.trim_prefix("refs/heads/");
		r_theirs = state_file_commit(p_repo, "rebase-merge/onto");
	} else if (p_kind == "cherry-pick") {
		r_theirs = state_file_commit(p_repo, "CHERRY_PICK_HEAD");
	} else if (p_kind == "pull") {
		// Your uncommitted edits against what the pull brought (see GitRepository::pull).
		r_mine = "your changes";
		r_theirs = read_pull_state(p_repo).get("upstream", String());
	} else if (p_kind == "revert") {
		r_theirs = vformat("revert of %s", state_file_commit(p_repo, "REVERT_HEAD"));
	}
}

} // namespace

// A conflicted file, for the resolver:
// { "path", "kind": the operation ("merge", "rebase", ...),
//   "mine_label", "theirs_label": what the sides are ("main", "origin/main", "feature", "a1b2c3d"),
//   "mine_exists", "theirs_exists", "base_exists": whether each version exists,
//   "binary": a side is binary or the file is stored with LFS (then no blocks: only a whole side
//   can be taken),
//   "blocks": [{ "kind": "same", "text" } | { "kind": "conflict", "mine", "base", "theirs" }] }.
// Texts are as git stores them (LF line endings with core.autocrlf), joined they make the file.
// "mine" is your side: the current branch, or during a rebase your commit being replayed. Empty
// if p_path isn't conflicted.
Dictionary GitRepository::get_conflict(const String &p_path) const {
	Dictionary result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	ConflictSides sides;
	read_sides(repo, p_path, sides);
	if (!sides.found) {
		return result;
	}
	result["path"] = p_path;
	result["kind"] = operation_in_progress(repo);
	String mine_label, theirs_label;
	side_labels(repo, result["kind"], mine_label, theirs_label);
	result["mine_label"] = mine_label;
	result["theirs_label"] = theirs_label;
	result["mine_exists"] = sides.mine.get() != nullptr;
	result["theirs_exists"] = sides.theirs.get() != nullptr;
	result["base_exists"] = sides.base.get() != nullptr;
	bool binary = false;
	for (git_blob *blob : { sides.base.get(), sides.mine.get(), sides.theirs.get() }) {
		binary = binary || (blob && git_blob_is_binary(blob));
	}
	// An LFS file's versions in git are pointers: merging those lines would mean nothing.
	binary = binary || (repo_uses_lfs(repo) && is_lfs_path(repo, p_path.utf8().get_data()));
	result["binary"] = binary;
	if (binary || !sides.mine.get() || !sides.theirs.get()) {
		result["blocks"] = Array(); // Deleted on one side, or binary: whole sides only.
		return result;
	}

	const CharString path = p_path.utf8();
	const git_merge_file_input base = merge_input(sides.base.get(), path);
	const git_merge_file_input mine = merge_input(sides.mine.get(), path);
	const git_merge_file_input theirs = merge_input(sides.theirs.get(), path);
	git_merge_file_options options = GIT_MERGE_FILE_OPTIONS_INIT;
	options.flags = GIT_MERGE_FILE_STYLE_DIFF3;
	options.marker_size = MARKER_SIZE;
	git_merge_file_result merged = {};
	if (git_merge_file(&merged, &base, &mine, &theirs, &options) < 0) {
		git_merge_file_result_free(&merged);
		ERR_FAIL_V_MSG(result, "Couldn't merge the versions of " + p_path + ": " + get_last_error());
	}
	result["blocks"] = split_merged(String::utf8(merged.ptr, (int)merged.len));
	git_merge_file_result_free(&merged);
	return result;
}

// Resolves p_path with p_text (as git stores it: see get_conflict) as its new content: written to
// the file through the worktree filters (CRLF with core.autocrlf), then staged.
Error GitRepository::resolve_conflict(const String &p_path, const String &p_text) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	ConflictSides sides;
	read_sides(repo, p_path, sides);
	if (!sides.found) {
		return fail(vformat("%s has no conflict to resolve.", p_path));
	}
	const CharString text = p_text.utf8();
	return _write_resolution(p_path, text.get_data(), text.length());
}

// Resolves p_path by taking one side's whole file ("mine" or "theirs"), which also covers binary
// files and a side that deleted it (then the file is deleted).
Error GitRepository::resolve_conflict_with(const String &p_path, const String &p_side) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	ConflictSides sides;
	read_sides(repo, p_path, sides);
	if (!sides.found) {
		return fail(vformat("%s has no conflict to resolve.", p_path));
	}
	git_blob *blob = p_side == "theirs" ? sides.theirs.get() : sides.mine.get();
	if (!blob) {
		DirAccess::remove_absolute(get_workdir().path_join(p_path));
		IndexPtr index;
		int err = git_repository_index(index.out(), repo);
		if (err >= 0) {
			err = git_index_remove_bypath(index, p_path.utf8().get_data());
		}
		if (err >= 0) {
			err = git_index_write(index);
		}
		return to_error(err);
	}
	return _write_resolution(p_path, (const char *)git_blob_rawcontent(blob), (size_t)git_blob_rawsize(blob));
}

Error GitRepository::_write_resolution(const String &p_path, const char *p_data, size_t p_size) {
	std::string on_disk;
	if (!apply_filters(repo, p_path, p_data, p_size, GIT_FILTER_TO_WORKTREE, on_disk)) {
		return fail(vformat("Couldn't prepare %s for writing: %s", p_path, get_last_error()));
	}
	const String absolute = get_workdir().path_join(p_path);
	DirAccess::make_dir_recursive_absolute(absolute.get_base_dir());
	Ref<FileAccess> file = FileAccess::open(absolute, FileAccess::WRITE);
	if (file.is_null()) {
		return fail(vformat("Couldn't write %s.", p_path));
	}
	PackedByteArray bytes;
	bytes.resize(on_disk.size());
	if (!on_disk.empty()) {
		memcpy(bytes.ptrw(), on_disk.data(), on_disk.size());
	}
	file->store_buffer(bytes);
	file.unref();
	Error err = stage(p_path);
	// Resolving one of your edits after a pull leaves it as it was: an uncommitted, unstaged
	// change (now with the pulled changes in it), not something staged for you.
	if (err == OK && operation_in_progress(repo) == "pull" && PackedStringArray(read_pull_state(repo).get("edits", PackedStringArray())).has(p_path)) {
		ObjectPtr head_commit;
		const CharString path = p_path.utf8();
		char *paths[] = { (char *)path.get_data() };
		git_strarray pathspec = { paths, 1 };
		if (git_revparse_single(head_commit.out(), repo, "HEAD") == 0) {
			err = to_error(git_reset_default(repo, head_commit, &pathspec));
		}
	}
	return err;
}
