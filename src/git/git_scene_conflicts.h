#pragma once

// The scene merge (src/scene/scene_merge.h) applied where git leaves conflicts: a conflicted .tscn
// or .tres it merges cleanly is settled on its own (decided 2026-10-08: automatic), so only real
// clashes reach you. Internal to the GitRepository sources.

#include <git2.h>

#include <godot_cpp/variant/packed_string_array.hpp>

#include <string>

using namespace godot;

namespace godot_git {

// The conflicted scene files in p_index that the scene merge settles: their paths. Changes nothing.
PackedStringArray settleable_scenes(git_repository *p_repo, git_index *p_index);

// Settles those in the working folder and p_index (written through git's filters and staged,
// which removes the conflict), then writes p_index. Returns their paths.
PackedStringArray settle_scene_conflicts(git_repository *p_repo, git_index *p_index);

// p_base, p_ours and p_theirs (as git stores them) merged by the scene merge when p_path is a
// scene file and it merges cleanly: for uncommitted edits, which git_merge_file couldn't merge.
bool merge_scene_text(const String &p_path, const std::string &p_base, const std::string &p_ours, const std::string &p_theirs, std::string &r_merged);

} // namespace godot_git
