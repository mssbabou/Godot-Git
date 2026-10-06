// Staging, unstaging and discarding part of a file: one hunk, or chosen lines (from the Diff panel).

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/local_vector.hpp>

#include <algorithm>
#include <string>

#include "git/git_lfs.h"
#include "git/git_util.h"

using namespace godot_git;

namespace {

// One line of a diff that covers the whole file: kept, removed or added.
struct DiffOp {
	char origin = ' '; // GIT_DIFF_LINE_CONTEXT, _ADDITION or _DELETION.
	std::string content; // With its line ending, if it has one.
	int old_line = -1;
	int new_line = -1;
};

// The diff from p_old to p_new as every line of both, in order (all of it is context, not just
// three lines around each change).
bool whole_file_diff(const std::string &p_old, const std::string &p_new, const String &p_path, LocalVector<DiffOp> &r_ops) {
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	// More context than either version has lines, so it's one hunk from the first line to the last.
	const uint32_t lines = (uint32_t)MAX(std::count(p_old.begin(), p_old.end(), '\n'), std::count(p_new.begin(), p_new.end(), '\n')) + 2;
	opts.context_lines = lines;
	opts.interhunk_lines = lines;
	opts.flags |= GIT_DIFF_FORCE_TEXT;
	PatchPtr patch;
	const CharString path = p_path.utf8();
	if (git_patch_from_buffers(patch.out(), p_old.data(), p_old.size(), path.get_data(), p_new.data(), p_new.size(), path.get_data(), &opts) < 0) {
		return false;
	}
	for (size_t h = 0; h < git_patch_num_hunks(patch); h++) {
		const git_diff_hunk *hunk = nullptr;
		size_t lines = 0;
		if (git_patch_get_hunk(&hunk, &lines, patch, h) < 0) {
			return false;
		}
		for (size_t l = 0; l < lines; l++) {
			const git_diff_line *line = nullptr;
			if (git_patch_get_line_in_hunk(&line, patch, h, l) < 0) {
				return false;
			}
			// "\ No newline at end of file" markers: the line before them simply has no ending.
			if (line->origin != GIT_DIFF_LINE_CONTEXT && line->origin != GIT_DIFF_LINE_ADDITION && line->origin != GIT_DIFF_LINE_DELETION) {
				continue;
			}
			DiffOp op;
			op.origin = line->origin;
			op.content.assign(line->content, line->content_len);
			op.old_line = line->old_lineno;
			op.new_line = line->new_lineno;
			r_ops.push_back(op);
		}
	}
	return true;
}

String line_text(const std::string &p_content) {
	return String::utf8(p_content.data(), (int)p_content.size()).trim_suffix("\n").trim_suffix("\r");
}

} // namespace

// Stages ("stage"), unstages ("unstage") or discards ("discard") some of p_path's changed lines:
// p_lines are rows of the diff get_diff(p_path, p_staged) gave, [{ "old": its old line number (-1
// for an added line), "new": its new line number (-1 for a removed one), "text" }, ...] (all of a
// hunk's changed lines to act on the hunk). Unstaged changes can be staged or discarded, staged ones
// unstaged. Nothing changes when the file changed since that diff (a line no longer matches), and
// for binary, LFS, conflicted, deleted or renamed files, which only go whole.
Error GitRepository::apply_line_changes(const String &p_path, bool p_staged, const String &p_action, const Array &p_lines) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	if (p_staged != (p_action == "unstage") || (p_action != "stage" && p_action != "unstage" && p_action != "discard")) {
		return fail(vformat("Can't %s %s changes.", p_action, p_staged ? "staged" : "unstaged"));
	}
	if (p_lines.is_empty()) {
		return fail("No lines were chosen.");
	}
	const CharString path = p_path.utf8();
	if (is_lfs_path(repo, path.get_data())) {
		return fail(vformat("%s is stored with Git LFS, so it can only be staged, unstaged or discarded as a whole.", p_path.get_file()));
	}
	IndexPtr index;
	if (git_repository_index(index.out(), repo) < 0) {
		return to_error(-1);
	}
	const git_index_entry *ancestor = nullptr, *ours = nullptr, *theirs = nullptr;
	if (git_index_conflict_get(&ancestor, &ours, &theirs, index, path.get_data()) == 0) {
		return fail(vformat("%s has conflicts: resolve it under Conflicts first.", p_path.get_file()));
	}

	// The two versions the diff was between, both as git keeps them (LF line endings with
	// core.autocrlf): HEAD and the index for staged changes, the index and the file for the rest.
	const git_index_entry *entry = git_index_get_bypath(index, path.get_data(), 0);
	std::string old_text, new_text;
	bool had_old = false; // Whether there's an old version at all (not a new file).
	const String absolute = get_workdir().path_join(p_path);
	if (p_staged) {
		if (!entry) {
			return fail(vformat("%s is staged as deleted: unstage it as a whole.", p_path.get_file()));
		}
		ObjectPtr head_tree;
		TreePtr tree;
		BlobPtr blob;
		if (git_revparse_single(head_tree.out(), repo, "HEAD^{tree}") == 0 && read_tree_blob(repo, (git_tree *)head_tree.get(), p_path, blob)) {
			old_text = blob_text(blob);
			had_old = true;
		}
		BlobPtr staged;
		if (git_blob_lookup(staged.out(), repo, &entry->id) < 0) {
			return to_error(-1);
		}
		new_text = blob_text(staged);
	} else {
		if (!FileAccess::file_exists(absolute)) {
			return fail(vformat("%s is deleted: stage or discard that as a whole.", p_path.get_file()));
		}
		if (entry) {
			BlobPtr blob;
			if (git_blob_lookup(blob.out(), repo, &entry->id) < 0) {
				return to_error(-1);
			}
			old_text = blob_text(blob);
			had_old = true;
		}
		const PackedByteArray bytes = FileAccess::get_file_as_bytes(absolute);
		if (!apply_filters(repo, p_path, (const char *)bytes.ptr(), bytes.size(), GIT_FILTER_TO_ODB, new_text)) {
			return fail(vformat("Couldn't read %s: %s", p_path.get_file(), get_last_error()));
		}
	}
	if (git_blob_data_is_binary(old_text.data(), old_text.size()) || git_blob_data_is_binary(new_text.data(), new_text.size())) {
		return fail(vformat("%s is a binary file, so it can only be staged, unstaged or discarded as a whole.", p_path.get_file()));
	}

	LocalVector<DiffOp> ops;
	if (!whole_file_diff(old_text, new_text, p_path, ops)) {
		return to_error(-1);
	}

	// The chosen lines, each checked against the diff as it is now.
	HashSet<int> removed, added;
	for (int i = 0; i < p_lines.size(); i++) {
		const Dictionary line = p_lines[i];
		const int old_line = line.get("old", -1);
		const int new_line = line.get("new", -1);
		const String text = String(line.get("text", String())).trim_suffix("\r");
		bool found = false;
		for (const DiffOp &op : ops) {
			if ((old_line > 0 && new_line <= 0 && op.origin == GIT_DIFF_LINE_DELETION && op.old_line == old_line) ||
					(new_line > 0 && old_line <= 0 && op.origin == GIT_DIFF_LINE_ADDITION && op.new_line == new_line)) {
				found = line_text(op.content) == text;
				break;
			}
		}
		if (!found) {
			return fail(vformat("%s changed since its diff was shown, so nothing was changed. Try again.", p_path.get_file()));
		}
		if (new_line > 0) {
			added.insert(new_line);
		} else {
			removed.insert(old_line);
		}
	}

	// Staging applies the chosen changes to the old version; unstaging and discarding take them
	// back out of the new one.
	const bool forward = p_action == "stage";
	std::string result;
	for (const DiffOp &op : ops) {
		bool keep = true;
		if (op.origin == GIT_DIFF_LINE_DELETION) {
			keep = forward ? !removed.has(op.old_line) : removed.has(op.old_line);
		} else if (op.origin == GIT_DIFF_LINE_ADDITION) {
			keep = forward ? added.has(op.new_line) : !added.has(op.new_line);
		}
		if (keep) {
			result += op.content;
		}
	}

	if (p_action == "discard") {
		if (!had_old && result.empty()) {
			// Every line of a new file: the file goes, like discarding it whole.
			return DirAccess::remove_absolute(absolute) == OK ? OK : fail(vformat("Couldn't delete %s. Is it open in another program?", p_path.get_file()));
		}
		std::string on_disk;
		if (!apply_filters(repo, p_path, result.data(), result.size(), GIT_FILTER_TO_WORKTREE, on_disk)) {
			return fail(vformat("Couldn't prepare %s for writing: %s", p_path.get_file(), get_last_error()));
		}
		return write_file(absolute, on_disk.data(), on_disk.size()) ? OK : fail(vformat("Couldn't write %s. Is it open in another program?", p_path.get_file()));
	}

	if (p_action == "unstage" && !had_old && result.empty()) {
		// Every line of a newly added file: it's no longer staged at all, like unstaging it whole.
		int err = git_index_remove_bypath(index, path.get_data());
		return to_error(err < 0 ? err : git_index_write(index));
	}
	git_index_entry new_entry = {};
	new_entry.mode = entry ? entry->mode : GIT_FILEMODE_BLOB;
	new_entry.path = path.get_data();
	int err = git_index_add_from_buffer(index, &new_entry, result.data(), result.size());
	if (err >= 0) {
		err = git_index_write(index);
	}
	return to_error(err);
}
