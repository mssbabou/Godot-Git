#pragma once

// Small helpers around libgit2 shared by the GitRepository sources. Not part of the API.

#include <git2.h>

#include <initializer_list>
#include <utility>

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace godot_git {

// libgit2 result (< 0 is an error) as a Godot Error.
Error to_error(int p_git_error);

// Sets the message GitRepository::get_last_error() returns, for failures libgit2 doesn't describe well.
Error fail(const String &p_message);

// FAILED if git doesn't know the user's name and email (user.name, user.email), which every
// commit records; p_what says what couldn't be done ("Nothing was pulled: ..."). OK otherwise.
// libgit2's own error ("config value 'user.name' was not found") means nothing to most people.
Error require_identity(git_repository *p_repo, const String &p_what);

// A git_buf's contents as a String; frees the buffer.
String buf_to_string(git_buf &p_buf);

// Owns one libgit2 object and frees it when it goes out of scope. Pass out() where libgit2
// creates the object; it converts to the raw pointer everywhere else.
template <typename T, void (*Free)(T *)>
class Owned {
	T *ptr = nullptr;

public:
	Owned() = default;
	~Owned() { Free(ptr); } // libgit2's *_free functions accept null.
	Owned(const Owned &) = delete;
	Owned &operator=(const Owned &) = delete;

	T **out() {
		Free(ptr);
		ptr = nullptr;
		return &ptr;
	}
	T *get() const { return ptr; }
	operator T *() const { return ptr; }
};

using AnnotatedCommitPtr = Owned<git_annotated_commit, git_annotated_commit_free>;
using BranchIteratorPtr = Owned<git_branch_iterator, git_branch_iterator_free>;
using CommitPtr = Owned<git_commit, git_commit_free>;
using ConfigPtr = Owned<git_config, git_config_free>;
using DiffPtr = Owned<git_diff, git_diff_free>;
using IndexPtr = Owned<git_index, git_index_free>;
using ObjectPtr = Owned<git_object, git_object_free>;
using PatchPtr = Owned<git_patch, git_patch_free>;
using ReferencePtr = Owned<git_reference, git_reference_free>;
using ReferenceIteratorPtr = Owned<git_reference_iterator, git_reference_iterator_free>;
using RemotePtr = Owned<git_remote, git_remote_free>;
using RevwalkPtr = Owned<git_revwalk, git_revwalk_free>;
using SignaturePtr = Owned<git_signature, git_signature_free>;
using StatusListPtr = Owned<git_status_list, git_status_list_free>;
using TreePtr = Owned<git_tree, git_tree_free>;
using TreeEntryPtr = Owned<git_tree_entry, git_tree_entry_free>;

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

// Makes a checkout all or nothing. libgit2 changes files one at a time and stops at the first it
// can't change (on Windows: a file another program has open, e.g. Godot importing it or an
// antivirus scanning it), leaving the ones before it changed. The guard records every file a
// checkout plans to write or has changed; restore() puts them back as they are in HEAD's tree.
// That's lossless with GIT_CHECKOUT_SAFE, which only touches files without uncommitted changes.
class CheckoutGuard {
	HashSet<String> paths;
	git_checkout_progress_cb chained_progress = nullptr;
	void *chained_payload = nullptr;

	static void progress_cb(const char *p_path, size_t p_completed, size_t p_total, void *p_payload);
	static int notify_cb(git_checkout_notify_t p_why, const char *p_path, const git_diff_file *p_baseline, const git_diff_file *p_target, const git_diff_file *p_workdir, void *p_payload);

public:
	// Call after setting up r_opts (its progress callback is kept, and still called).
	void attach(git_checkout_options &r_opts);
	// Puts the touched files back as they are in p_tree. Returns the paths that couldn't be.
	PackedStringArray restore(git_repository *p_repo, const git_tree *p_tree);
};

// git_checkout_tree, all or nothing (see CheckoutGuard), retrying a moment later if a file is in
// use. On failure nothing has changed, and the error names the file in use if that was it.
int checkout_all_or_nothing(git_repository *p_repo, const git_object *p_target, git_checkout_options &p_opts);

// After a failed checkout was undone: sets the error to show (naming the file libgit2 couldn't
// change) from libgit2's p_error, and the paths restore() couldn't put back, if any.
Error explain_checkout_failure(git_repository *p_repo, const String &p_error, const PackedStringArray &p_not_restored);

// Waits for a process started with execute_with_pipe to end and returns its exit code, or -1 if
// it's still running after 10 seconds. Its pipes close a moment before it has fully exited, and
// OS::get_process_exit_code() returns -1 until then (read too early, a git that succeeded looked
// like it failed; seen on CI's Linux arm64 runner).
int wait_for_exit_code(int64_t p_pid);

// Sets environment variables while alive, then puts the old values back. Scope it around
// starting a process: a child copies the environment when it starts, and Godot can't pass an
// environment to one process. Left set, they'd leak into games started from the editor.
class ScopedEnvironment {
	struct Saved {
		String name;
		bool existed = false;
		String value;
	};
	LocalVector<Saved> saved;

public:
	ScopedEnvironment(std::initializer_list<std::pair<const char *, const char *>> p_variables);
	~ScopedEnvironment();
	ScopedEnvironment(const ScopedEnvironment &) = delete;
	ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;
};

// libgit2's current error message and class, before something else overwrites them.
String last_git_error(int *r_class = nullptr);

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
