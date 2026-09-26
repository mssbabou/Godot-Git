// GitRepository: the diff of one file, for the diff view.

#include "git/git_repository.h"

#include <git2.h>

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include "git/git_lfs.h"
#include "git/git_util.h"

using namespace godot_git;

namespace {

// Bigger files aren't diffed: libgit2 treats them as binary, and nobody reads such a diff anyway.
constexpr git_object_size_t MAX_DIFF_SIZE = 2 * 1024 * 1024;

String delta_status_name(git_delta_t p_status) {
	switch (p_status) {
		case GIT_DELTA_ADDED:
			return "new";
		case GIT_DELTA_DELETED:
			return "deleted";
		case GIT_DELTA_MODIFIED:
			return "modified";
		case GIT_DELTA_RENAMED:
			return "renamed";
		case GIT_DELTA_COPIED:
			return "copied";
		case GIT_DELTA_UNTRACKED:
			return "untracked";
		case GIT_DELTA_TYPECHANGE:
			return "typechange";
		default:
			return String();
	}
}

// The text after a hunk's "@@ -1,3 +1,4 @@": the enclosing function or section, if git found one.
String hunk_context(const git_diff_hunk *p_hunk) {
	const String header = String::utf8(p_hunk->header, (int)p_hunk->header_len).strip_edges();
	const int end = header.find("@@", 2);
	return end < 0 ? String() : header.substr(end + 2).strip_edges();
}

// Describes the change to p_path in p_diff (see GitRepository::get_diff). "kind" is "unchanged"
// when p_diff doesn't touch the file.
Dictionary describe_file_diff(git_repository *p_repo, git_diff *p_diff, const String &p_path) {
	Dictionary result;
	result["path"] = p_path;
	result["old_path"] = p_path;
	result["status"] = String();
	result["kind"] = "unchanged";
	result["added"] = 0;
	result["removed"] = 0;
	result["hunks"] = Array();

	const CharString path_utf8 = p_path.utf8();
	const size_t count = git_diff_num_deltas(p_diff);
	for (size_t i = 0; i < count; i++) {
		const git_diff_delta *delta = git_diff_get_delta(p_diff, i);
		const char *new_path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;
		if (strcmp(new_path, path_utf8.get_data()) != 0) {
			continue;
		}

		result["old_path"] = String::utf8(delta->old_file.path ? delta->old_file.path : new_path);
		result["status"] = delta_status_name(delta->status);

		// An LFS file's diff would be of its pointer text, which says nothing about the real file,
		// and computing it runs the whole file through git-lfs.
		if (is_lfs_path(p_repo, new_path)) {
			result["kind"] = "lfs";
			return result;
		}

		PatchPtr patch;
		if (git_patch_from_diff(patch.out(), p_diff, i) < 0 || !patch) {
			result["kind"] = "binary";
			return result;
		}
		const git_diff_delta *patch_delta = git_patch_get_delta(patch);
		if (patch_delta->flags & GIT_DIFF_FLAG_BINARY) {
			const bool too_large = patch_delta->old_file.size > MAX_DIFF_SIZE || patch_delta->new_file.size > MAX_DIFF_SIZE;
			result["kind"] = too_large ? "too_large" : "binary";
			return result;
		}

		size_t added = 0, removed = 0;
		git_patch_line_stats(nullptr, &added, &removed, patch);
		result["added"] = (int64_t)added;
		result["removed"] = (int64_t)removed;
		result["kind"] = "text";

		Array hunks;
		const size_t hunk_count = git_patch_num_hunks(patch);
		for (size_t h = 0; h < hunk_count; h++) {
			const git_diff_hunk *hunk = nullptr;
			size_t line_count = 0;
			if (git_patch_get_hunk(&hunk, &line_count, patch, h) < 0) {
				continue;
			}

			PackedByteArray origins;
			PackedInt32Array old_numbers;
			PackedInt32Array new_numbers;
			PackedStringArray text;
			for (size_t l = 0; l < line_count; l++) {
				const git_diff_line *line = nullptr;
				if (git_patch_get_line_in_hunk(&line, patch, h, l) < 0) {
					continue;
				}
				// "\ No newline at end of file" markers ('=', '>', '<') aren't lines of the file.
				if (line->origin != GIT_DIFF_LINE_CONTEXT && line->origin != GIT_DIFF_LINE_ADDITION && line->origin != GIT_DIFF_LINE_DELETION) {
					continue;
				}
				// Lines come with their line ending; a file checked out with CRLF has "\r" too.
				String content = String::utf8(line->content, (int)line->content_len);
				content = content.trim_suffix("\n").trim_suffix("\r");
				origins.push_back((uint8_t)line->origin);
				old_numbers.push_back(line->old_lineno);
				new_numbers.push_back(line->new_lineno);
				text.push_back(content);
			}

			Dictionary item;
			item["old_start"] = hunk->old_start;
			item["old_lines"] = hunk->old_lines;
			item["new_start"] = hunk->new_start;
			item["new_lines"] = hunk->new_lines;
			item["context"] = hunk_context(hunk);
			item["origins"] = origins;
			item["old_numbers"] = old_numbers;
			item["new_numbers"] = new_numbers;
			item["text"] = text;
			hunks.push_back(item);
		}
		result["hunks"] = hunks;
		return result;
	}
	return result;
}

} // namespace

// The uncommitted change to one file, for the diff view. p_staged: HEAD vs index (what the next
// commit contains); otherwise index vs working tree, like the two lists in the dock.
// { "path", "old_path" (differs for a staged rename), "status" (as in get_status), "kind",
//   "added", "removed", "hunks" }. "kind" is "text", or "binary", "too_large" (over 2 MB), "lfs"
// (stored with Git LFS) or "unchanged", which have no hunks. Each hunk is { "old_start",
// "old_lines", "new_start", "new_lines", "context" (the function or section it's in, if git found
// one), and one entry per line in "origins" ('+', '-' or ' '), "old_numbers", "new_numbers" (-1
// where the line doesn't exist on that side) and "text" (without the line ending) }.
Dictionary GitRepository::get_diff(const String &p_path, bool p_staged) const {
	ERR_FAIL_NULL_V_MSG(repo, Dictionary(), "Repository is not open.");

	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	opts.max_size = MAX_DIFF_SIZE;

	DiffPtr diff;
	int err = 0;
	if (p_staged) {
		// The whole index, not just this path: a staged rename is only found as one (and shown as
		// the file's real change) when both its old and new path are in the diff.
		ObjectPtr head_tree;
		git_revparse_single(head_tree.out(), repo, "HEAD^{tree}"); // Stays null before the first commit.
		err = git_diff_tree_to_index(diff.out(), repo, (git_tree *)head_tree.get(), nullptr, &opts);
		if (err >= 0) {
			git_diff_find_similar(diff, nullptr);
		}
	} else {
		// Like get_status, unstaged changes aren't matched up as renames, so one path is enough.
		SinglePathspec pathspec(p_path);
		opts.pathspec = pathspec.array;
		opts.flags = GIT_DIFF_DISABLE_PATHSPEC_MATCH | GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS | GIT_DIFF_SHOW_UNTRACKED_CONTENT;
		err = git_diff_index_to_workdir(diff.out(), repo, nullptr, &opts);
	}
	if (err < 0) {
		return Dictionary();
	}
	return describe_file_diff(repo, diff, p_path);
}
