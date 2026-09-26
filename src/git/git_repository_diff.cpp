// GitRepository: the diffs for the Diff panel (one file's uncommitted changes, or its change in a
// commit), the files a commit changed, for History, and a file's content in any version, for
// previews of files that aren't text (images).

#include "git/git_repository.h"

#include <git2.h>

#include <godot_cpp/classes/file_access.hpp>

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
		if (repo_uses_lfs(p_repo) && is_lfs_path(p_repo, new_path)) {
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

// What p_hash (a commit, full or short hash) changed: the diff from its first parent (or from
// nothing, for the first commit) to it, with renames found. A merge is compared with its first
// parent, like `git log --first-parent`: what merging brought into the branch.
int commit_diff(git_repository *p_repo, const String &p_hash, git_diff **r_diff) {
	ObjectPtr object;
	int err = git_revparse_single(object.out(), p_repo, p_hash.utf8().get_data());
	CommitPtr commit;
	if (err >= 0) {
		err = git_commit_lookup(commit.out(), p_repo, git_object_id(object));
	}
	TreePtr tree;
	if (err >= 0) {
		err = git_commit_tree(tree.out(), commit);
	}
	TreePtr parent_tree;
	if (err >= 0 && git_commit_parentcount(commit) > 0) {
		CommitPtr parent;
		err = git_commit_parent(parent.out(), commit, 0);
		if (err >= 0) {
			err = git_commit_tree(parent_tree.out(), parent);
		}
	}
	if (err < 0) {
		return err;
	}
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	opts.max_size = MAX_DIFF_SIZE;
	err = git_diff_tree_to_tree(r_diff, p_repo, parent_tree, tree, &opts);
	if (err >= 0) {
		git_diff_find_similar(*r_diff, nullptr);
	}
	return err;
}

// If p_bytes is a Git LFS pointer (what git stores for an LFS file), the object id it names.
String lfs_pointer_oid(const PackedByteArray &p_bytes) {
	// Pointers are a few lines of text, well under 1 KB, and always start like this.
	if (p_bytes.size() > 1024) {
		return String();
	}
	const String text = String::utf8((const char *)p_bytes.ptr(), p_bytes.size());
	if (!text.begins_with("version https://git-lfs.github.com/spec/")) {
		return String();
	}
	for (const String &line : text.split("\n")) {
		if (line.begins_with("oid sha256:")) {
		return line.trim_prefix("oid sha256:").strip_edges();
		}
}
return String();
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

// The files commit p_hash changed (see commit_diff), in git's order:
// [{ "path", "old_path", "status" (as in get_status), "added", "removed" }, ...]. "added" and
// "removed" are -1 for binary, LFS and very large files, and for every file of a commit that
// changed more than 300 (counting lines means diffing each one, and nobody reads that many).
Array GitRepository::get_commit_files(const String &p_hash) const {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	DiffPtr diff;
	if (commit_diff(repo, p_hash, diff.out()) < 0) {
		return result;
	}
	const bool uses_lfs = repo_uses_lfs(repo); // See get_line_stats.
	const size_t count = git_diff_num_deltas(diff);
	for (size_t i = 0; i < count; i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);
		const char *path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;
		Dictionary file;
		file["path"] = String::utf8(path);
		file["old_path"] = String::utf8(delta->old_file.path ? delta->old_file.path : path);
		file["status"] = delta_status_name(delta->status);
		file["added"] = -1;
		file["removed"] = -1;
		PatchPtr patch;
		if (count <= 300 && !(uses_lfs && is_lfs_path(repo, path)) && git_patch_from_diff(patch.out(), diff, i) == 0 && patch && !(git_patch_get_delta(patch)->flags & GIT_DIFF_FLAG_BINARY)) {
			size_t added = 0, removed = 0;
			git_patch_line_stats(nullptr, &added, &removed, patch);
			file["added"] = (int64_t)added;
			file["removed"] = (int64_t)removed;
		}
		result.push_back(file);
	}
	return result;
}

// How commit p_hash changed one file, in the same form as get_diff.
Dictionary GitRepository::get_commit_diff(const String &p_hash, const String &p_path) const {
	ERR_FAIL_NULL_V_MSG(repo, Dictionary(), "Repository is not open.");
	DiffPtr diff;
	if (commit_diff(repo, p_hash, diff.out()) < 0) {
		return Dictionary();
	}
	return describe_file_diff(repo, diff, p_path);
}

// A file's content in one version: p_version is "workdir" (the file on disk), "index" (what's
// staged) or a revision ("HEAD", a commit hash, "<hash>^1" for its first parent).
// { "exists": bool, "bytes": PackedByteArray, "lfs": "" | "cached" | "missing" }.
// Git stores an LFS file as a small pointer; its real content is read from git-lfs's local cache,
// which holds every version that was checked out or pulled. "missing": not in the cache (a version
// never fetched), so "bytes" is empty. The file on disk is already the real content.
Dictionary GitRepository::get_file_bytes(const String &p_version, const String &p_path) const {
	Dictionary result;
	result["exists"] = false;
	result["bytes"] = PackedByteArray();
	result["lfs"] = String();
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");

	PackedByteArray bytes;
	if (p_version == "workdir") {
		const String path = get_workdir().path_join(p_path);
		if (!FileAccess::file_exists(path)) {
			return result;
		}
		bytes = FileAccess::get_file_as_bytes(path);
	} else {
		git_oid blob_id;
		if (p_version == "index") {
			IndexPtr index;
			const git_index_entry *entry = nullptr;
			if (git_repository_index(index.out(), repo) < 0 || !(entry = git_index_get_bypath(index, p_path.utf8().get_data(), 0))) {
				return result;
			}
			git_oid_cpy(&blob_id, &entry->id);
		} else {
			ObjectPtr object;
			if (git_revparse_single(object.out(), repo, vformat("%s:%s", p_version, p_path).utf8().get_data()) < 0 || git_object_type(object) != GIT_OBJECT_BLOB) {
				return result;
			}
			git_oid_cpy(&blob_id, git_object_id(object));
		}
		BlobPtr blob;
		if (git_blob_lookup(blob.out(), repo, &blob_id) < 0) {
			return result;
		}
		bytes.resize(git_blob_rawsize(blob));
		memcpy(bytes.ptrw(), git_blob_rawcontent(blob), bytes.size());
	}
	result["exists"] = true;

	const String oid = lfs_pointer_oid(bytes);
	if (!oid.is_empty() && oid.length() > 4) {
		const String object = String::utf8(git_repository_commondir(repo)).path_join("lfs/objects").path_join(oid.substr(0, 2)).path_join(oid.substr(2, 2)).path_join(oid);
		if (FileAccess::file_exists(object)) {
			result["lfs"] = "cached";
			bytes = FileAccess::get_file_as_bytes(object);
		} else {
			result["lfs"] = "missing";
			bytes = PackedByteArray();
		}
	}
	result["bytes"] = bytes;
	return result;
}
