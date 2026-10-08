#include "git/git_scene_conflicts.h"

#include <git2/sys/errors.h>

#include <godot_cpp/variant/dictionary.hpp>

#include <vector>

#include "git/git_util.h"
#include "scene/scene_diff.h"
#include "scene/scene_merge.h"

namespace godot_git {

namespace {

String to_string(const std::string &p_text) {
	return String::utf8(p_text.data(), (int)p_text.size());
}

// The scene merge of three texts, when it settles everything: true, with the result.
bool merge_cleanly(const std::string &p_base, const std::string &p_ours, const std::string &p_theirs, std::string &r_merged) {
	const Dictionary merged = merge_scene(to_string(p_base), to_string(p_ours), to_string(p_theirs));
	if (!bool(merged.get("ok", false)) || int(merged.get("conflicts", 1)) != 0) {
		return false;
	}
	const CharString text = String(merged["text"]).utf8();
	r_merged.assign(text.get_data(), text.length());
	return true;
}

// p_entry's content, or "" when there's no entry (the side didn't have the file). False when it
// can't be read or isn't text (binary; an LFS pointer is text but no scene, so the merge declines it).
bool entry_text(git_repository *p_repo, const git_index_entry *p_entry, std::string &r_text) {
	if (!p_entry) {
		r_text.clear();
		return true;
	}
	BlobPtr blob;
	if (git_blob_lookup(blob.out(), p_repo, &p_entry->id) != 0 || git_blob_is_binary(blob)) {
		return false;
	}
	r_text = blob_text(blob);
	return true;
}

struct Settled {
	String path;
	std::string text;
};

// The conflicted scenes in p_index the scene merge settles, with their merged text. Both sides must
// have the file (a deletion against a change is a real decision); base may be missing (both added it).
std::vector<Settled> find_settleable(git_repository *p_repo, git_index *p_index) {
	std::vector<Settled> settled;
	IndexConflictIteratorPtr it;
	if (git_index_conflict_iterator_new(it.out(), p_index) != 0) {
		return settled;
	}
	const git_index_entry *ancestor = nullptr, *ours = nullptr, *theirs = nullptr;
	while (git_index_conflict_next(&ancestor, &ours, &theirs, it) == 0) {
		if (!ours || !theirs) {
			continue;
		}
		const String path = String::utf8(ours->path);
		if (!is_scene_path(path)) {
			continue;
		}
		std::string base, mine, other, merged;
		if (entry_text(p_repo, ancestor, base) && entry_text(p_repo, ours, mine) && entry_text(p_repo, theirs, other) && merge_cleanly(base, mine, other, merged)) {
			settled.push_back({ path, merged });
		}
	}
	git_error_clear();
	return settled;
}

} // namespace

PackedStringArray settleable_scenes(git_repository *p_repo, git_index *p_index) {
	PackedStringArray paths;
	for (const Settled &scene : find_settleable(p_repo, p_index)) {
		paths.push_back(scene.path);
	}
	return paths;
}

PackedStringArray settle_scene_conflicts(git_repository *p_repo, git_index *p_index) {
	PackedStringArray paths;
	const String workdir = String::utf8(git_repository_workdir(p_repo));
	for (const Settled &scene : find_settleable(p_repo, p_index)) {
		// Written the way a checkout would (CRLF where the user's git settings want it), then
		// staged like a hand resolution, which removes the conflict.
		std::string on_disk;
		const CharString path = scene.path.utf8();
		if (!apply_filters(p_repo, scene.path, scene.text.data(), scene.text.size(), GIT_FILTER_TO_WORKTREE, on_disk) ||
				!write_file(workdir.path_join(scene.path), on_disk.data(), on_disk.size()) ||
				git_index_add_bypath(p_index, path.get_data()) != 0) {
			continue; // Left as a conflict for the resolver.
		}
		paths.push_back(scene.path);
	}
	if (!paths.is_empty()) {
		git_index_write(p_index);
	}
	git_error_clear();
	return paths;
}

bool merge_scene_text(const String &p_path, const std::string &p_base, const std::string &p_ours, const std::string &p_theirs, std::string &r_merged) {
	return is_scene_path(p_path) && merge_cleanly(p_base, p_ours, p_theirs, r_merged);
}

} // namespace godot_git
