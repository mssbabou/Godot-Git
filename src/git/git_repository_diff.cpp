// GitRepository: the diffs for the Diff panel (one file's uncommitted changes, or its change in a
// commit), the files a commit changed, for History, and a file's content in any version, for
// previews of files that aren't text (images).

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include <cstring>

#include "git/git_lfs.h"
#include "git/git_util.h"
#include "git/settings_text.h"
#include "scene/scene_diff.h"

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

Array list_files(git_repository *repo, git_diff *diff); // Below, with get_commit_files.

// The Diff panel's options (see GitRepository::set_diff_options) on p_options.
void apply_diff_options(git_diff_options &r_options, int p_context, bool p_ignore_whitespace) {
	// "The whole file" as more context than any file the panel shows has lines (it stops at 2 MB).
	r_options.context_lines = p_context < 0 ? 1000000 : (uint32_t)p_context;
	if (p_ignore_whitespace) {
		r_options.flags |= GIT_DIFF_IGNORE_WHITESPACE;
	}
}

// What p_hash (a commit, full or short hash) changed: the diff from its first parent (or from
// nothing, for the first commit) to it, with renames found. A merge is compared with its first
// parent, like `git log --first-parent`: what merging brought into the branch.
// p_stash: p_hash is a stash, whose first parent is the commit it was made on and whose third
// parent (if any) holds the new, untracked files it took along; those are added as new files.
// p_options: context lines and whitespace for the Diff panel (see GitRepository::set_diff_options).
int commit_diff(git_repository *p_repo, const String &p_hash, git_diff **r_diff, bool p_stash = false, const git_diff_options *p_options = nullptr) {
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
	if (p_options) {
		opts = *p_options;
	}
	opts.max_size = MAX_DIFF_SIZE;
	err = git_diff_tree_to_tree(r_diff, p_repo, parent_tree, tree, &opts);
	if (err >= 0) {
		git_diff_find_similar(*r_diff, nullptr);
	}
	if (err >= 0 && p_stash && git_commit_parentcount(commit) > 2) {
		CommitPtr untracked;
		TreePtr untracked_tree;
		DiffPtr added;
		if (git_commit_parent(untracked.out(), commit, 2) == 0 && git_commit_tree(untracked_tree.out(), untracked) == 0 &&
				git_diff_tree_to_tree(added.out(), p_repo, nullptr, untracked_tree, &opts) == 0) {
			err = git_diff_merge(*r_diff, added);
		}
	}
	return err;
}

// If p_bytes is a Git LFS pointer (what git stores for an LFS file), the object id it names.
String lfs_pointer_oid(const PackedByteArray &p_bytes) {
	// Pointers are a few lines of text, well under 1 KB, and always start like this.
	// The prefix is compared as bytes first: decoding a small binary file (a 32×32 PNG) as UTF-8
	// makes Godot print a Unicode error to the Output panel.
	static const char prefix[] = "version https://git-lfs.github.com/spec/";
	const int64_t prefix_length = sizeof(prefix) - 1;
	if (p_bytes.size() > 1024 || p_bytes.size() < prefix_length || memcmp(p_bytes.ptr(), prefix, prefix_length) != 0) {
		return String();
	}
	const String text = String::utf8((const char *)p_bytes.ptr(), p_bytes.size());
	for (const String &line : text.split("\n")) {
		if (line.begins_with("oid sha256:")) {
			return line.trim_prefix("oid sha256:").strip_edges();
		}
	}
	return String();
}

// For a settings file (`.import`, `.uid`): which settings changed, read from both versions'
// whole text ("settings", see settings_changes), and the importer ("importer", for naming values).
void add_settings(const GitRepository *p_repo, Dictionary &r_diff, const String &p_old_version, const String &p_new_version) {
	const String path = r_diff["path"];
	if (String(r_diff["kind"]) != "text") {
		return;
	}
	if (is_scene_path(path)) {
		// A scene or resource: node by node (see scene_changes).
		const String old_text = PackedByteArray(Dictionary(p_repo->get_file_bytes(p_old_version, r_diff["old_path"]))["bytes"]).get_string_from_utf8();
		const String new_text = PackedByteArray(Dictionary(p_repo->get_file_bytes(p_new_version, path))["bytes"]).get_string_from_utf8();
		r_diff["scene"] = scene_changes(old_text, new_text);
		return;
	}
	if (!is_settings_path(path)) {
		return;
	}
	const String old_text = PackedByteArray(Dictionary(p_repo->get_file_bytes(p_old_version, r_diff["old_path"]))["bytes"]).get_string_from_utf8();
	const String new_text = PackedByteArray(Dictionary(p_repo->get_file_bytes(p_new_version, path))["bytes"]).get_string_from_utf8();
	r_diff["settings"] = settings_changes(path, old_text, new_text);
	const String importer = settings_value(new_text.is_empty() ? old_text : new_text, "remap", "importer");
	r_diff["importer"] = importer.trim_prefix("\"").trim_suffix("\"");
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
// A `.import` or `.uid` file also has "settings" and "importer" (see add_settings).
Dictionary GitRepository::get_diff(const String &p_path, bool p_staged) const {
	ERR_FAIL_NULL_V_MSG(repo, Dictionary(), "Repository is not open.");

	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	apply_diff_options(opts, diff_context_lines, diff_ignore_whitespace);
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
		opts.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH | GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS | GIT_DIFF_SHOW_UNTRACKED_CONTENT;
		err = git_diff_index_to_workdir(diff.out(), repo, nullptr, &opts);
	}
	if (err < 0) {
		return Dictionary();
	}
	Dictionary result = describe_file_diff(repo, diff, p_path);
	add_settings(this, result, p_staged ? "HEAD" : "index", p_staged ? "index" : "workdir");
	result["whitespace_ignored"] = diff_ignore_whitespace;
	return result;
}

// The files commit p_hash changed (see commit_diff), in git's order:
// [{ "path", "old_path", "status" (as in get_status), "added", "removed" }, ...]. "added" and
// "removed" are -1 for binary, LFS and very large files, and for every file of a commit that
// changed more than 300 (counting lines means diffing each one, and nobody reads that many).
Array GitRepository::get_commit_files(const String &p_hash) const {
	ERR_FAIL_NULL_V_MSG(repo, Array(), "Repository is not open.");
	DiffPtr diff;
	if (commit_diff(repo, p_hash, diff.out()) < 0) {
		return Array();
	}
	return list_files(repo, diff);
}

// The files stash p_hash changed, in the same form as get_commit_files; the new files it took
// along are "untracked".
Array GitRepository::get_stash_files(const String &p_hash) const {
	ERR_FAIL_NULL_V_MSG(repo, Array(), "Repository is not open.");
	DiffPtr diff;
	if (commit_diff(repo, p_hash, diff.out(), true) < 0) {
		return Array();
	}
	return list_files(repo, diff);
}

// How stash p_hash changes one file, in the same form as get_diff.
Dictionary GitRepository::get_stash_diff(const String &p_hash, const String &p_path) const {
	ERR_FAIL_NULL_V_MSG(repo, Dictionary(), "Repository is not open.");
	DiffPtr diff;
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	apply_diff_options(opts, diff_context_lines, diff_ignore_whitespace);
	if (commit_diff(repo, p_hash, diff.out(), true, &opts) < 0) {
		return Dictionary();
	}
	Dictionary result = describe_file_diff(repo, diff, p_path);
	result["whitespace_ignored"] = diff_ignore_whitespace;
	// A new file the stash took along is in its third parent, not in the stash itself.
	add_settings(this, result, vformat("%s^1", p_hash), has_file_at(p_hash, p_path) ? p_hash : vformat("%s^3", p_hash));
	return result;
}

namespace {

// The files in p_diff with their line counts, for History and Stashes (see get_commit_files).
Array list_files(git_repository *repo, git_diff *diff) {
	Array result;
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

} // namespace

// How commit p_hash changed one file, in the same form as get_diff.
Dictionary GitRepository::get_commit_diff(const String &p_hash, const String &p_path) const {
	ERR_FAIL_NULL_V_MSG(repo, Dictionary(), "Repository is not open.");
	DiffPtr diff;
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	apply_diff_options(opts, diff_context_lines, diff_ignore_whitespace);
	if (commit_diff(repo, p_hash, diff.out(), false, &opts) < 0) {
		return Dictionary();
	}
	Dictionary result = describe_file_diff(repo, diff, p_path);
	add_settings(this, result, vformat("%s^1", p_hash), p_hash);
	result["whitespace_ignored"] = diff_ignore_whitespace;
	return result;
}

// How the Diff panel's diffs are made (get_diff, get_commit_diff, get_stash_diff): p_context
// lines around each change (git's default is 3; -1 for the whole file), and whether changes to
// whitespace alone are left out (git diff -w). Line counts are always without these.
void GitRepository::set_diff_options(int p_context, bool p_ignore_whitespace) {
	diff_context_lines = p_context;
	diff_ignore_whitespace = p_ignore_whitespace;
}

// A file's content in one version: p_version is "workdir" (the file on disk), "index" (what's
// staged) or a revision ("HEAD", a commit hash, "<hash>^1" for its first parent), or for a
// conflicted file "mine" / "theirs" (its two sides; see get_conflict).
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
		if (p_version == "index" || p_version == "mine" || p_version == "theirs") {
			// A conflict's sides are index stages 2 (ours) and 3 (theirs), swapped in a rebase,
			// where "ours" is the branch being rebased onto.
			int stage = 0;
			if (p_version != "index") {
				const bool swapped = operation_in_progress(repo) == "rebase";
				stage = (p_version == "mine") != swapped ? 2 : 3;
			}
			IndexPtr index;
			const git_index_entry *entry = nullptr;
			if (git_repository_index(index.out(), repo) < 0 || !(entry = git_index_get_bypath(index, p_path.utf8().get_data(), stage))) {
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

// What changed between two versions of a text, line by line, for the script editor's change marks:
// [{ "old_start", "old_count", "new_start", "new_count" (1-based, git's hunk header numbers, no
// context lines), "old_lines" (the lines it replaced) }]. A deletion has new_count 0 and new_start
// the line it follows (0: the top); an addition has old_count 0. Line endings don't count (CRLF
// and a missing final newline would otherwise mark lines nobody changed).
Array GitRepository::diff_lines(const String &p_old, const String &p_new) {
	Array result;
	const auto normalized = [](const String &p_text) {
		String text = p_text.replace("\r\n", "\n");
		if (!text.is_empty() && !text.ends_with("\n")) {
			text += String("\n");
		}
		return text.utf8();
	};
	const CharString old_text = normalized(p_old);
	const CharString new_text = normalized(p_new);
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	opts.context_lines = 0;
	opts.interhunk_lines = 0;
	opts.flags |= GIT_DIFF_FORCE_TEXT;
	PatchPtr patch;
	if (git_patch_from_buffers(patch.out(), old_text.get_data(), old_text.length(), nullptr, new_text.get_data(), new_text.length(), nullptr, &opts) < 0 || !patch) {
		git_error_clear();
		return result;
	}
	for (size_t h = 0; h < git_patch_num_hunks(patch); h++) {
		const git_diff_hunk *hunk = nullptr;
		size_t line_count = 0;
		if (git_patch_get_hunk(&hunk, &line_count, patch, h) < 0) {
			continue;
		}
		PackedStringArray old_lines;
		for (size_t l = 0; l < line_count; l++) {
			const git_diff_line *line = nullptr;
			if (git_patch_get_line_in_hunk(&line, patch, h, l) == 0 && line->origin == GIT_DIFF_LINE_DELETION) {
				old_lines.push_back(String::utf8(line->content, (int)line->content_len).trim_suffix("\n"));
			}
		}
		Dictionary entry;
		entry["old_start"] = hunk->old_start;
		entry["old_count"] = hunk->old_lines;
		entry["new_start"] = hunk->new_start;
		entry["new_count"] = hunk->new_lines;
		entry["old_lines"] = old_lines;
		result.push_back(entry);
	}
	return result;
}
