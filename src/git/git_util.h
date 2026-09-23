#pragma once

// Small helpers around libgit2 shared by the GitRepository sources. Not part of the API.

#include <git2.h>

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace godot_git {

// libgit2 result (< 0 is an error) as a Godot Error.
Error to_error(int p_git_error);

// Sets the message GitRepository::get_last_error() returns, for failures libgit2 doesn't describe well.
Error fail(const String &p_message);

// A git_buf's contents as a String; frees the buffer.
String buf_to_string(git_buf &p_buf);

// Owns the UTF-8 buffer a single-entry git_strarray points into.
struct SinglePathspec {
	CharString utf8;
	char *entry = nullptr;
	git_strarray array = { nullptr, 0 };

	explicit SinglePathspec(const String &p_path) :
			utf8(p_path.utf8()) {
		entry = const_cast<char *>(utf8.get_data());
		array.strings = &entry;
		array.count = 1;
	}
};

// Looks up the branch HEAD points at. Fails with a readable message on a detached HEAD
// or a branch with no commits yet.
int head_branch(git_reference **r_ref, git_repository *p_repo);

// Paths that differ between commit p_from and commit p_to (HEAD when p_to is null).
HashSet<String> changed_paths(git_repository *p_repo, const git_oid *p_from, const git_oid *p_to);

// Paths a stash entry changed, relative to the commit it was made on.
PackedStringArray stashed_paths(git_repository *p_repo, const git_oid *p_stash);

// Every path with uncommitted changes: staged, unstaged, or new (untracked) files.
PackedStringArray uncommitted_paths(git_repository *p_repo);

} // namespace godot_git
