#include "git/git_util.h"

#include <git2/sys/errors.h>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/templates/local_vector.hpp>

#include <cstring>

namespace godot_git {

Error to_error(int p_git_error) {
	if (p_git_error >= 0) {
		return OK;
	}
	return p_git_error == GIT_ENOTFOUND ? ERR_FILE_NOT_FOUND : FAILED;
}

Error fail(const String &p_message) {
	git_error_set_str(GIT_ERROR_INVALID, p_message.utf8().get_data());
	return FAILED;
}

Error require_identity(git_repository *p_repo, const String &p_what) {
	SignaturePtr signature;
	if (git_signature_default(signature.out(), p_repo) == 0) {
		return OK;
	}
	return fail(vformat("%s Every commit records who made it, and git doesn't know your name and email yet. Set them by pressing Commit in the Git panel, or in a terminal with `git config --global user.name \"Your Name\"` and `git config --global user.email you@example.com`.", p_what));
}

String buf_to_string(git_buf &p_buf) {
	const String result = p_buf.ptr ? String::utf8(p_buf.ptr) : String();
	git_buf_dispose(&p_buf);
	return result;
}

int head_branch(git_reference **r_ref, git_repository *p_repo) {
	int err = git_repository_head(r_ref, p_repo);
	if (err == GIT_EUNBORNBRANCH) {
		fail("This branch has no commits yet.");
		return err;
	}
	if (err >= 0 && !git_reference_is_branch(*r_ref)) {
		git_reference_free(*r_ref);
		*r_ref = nullptr;
		fail("You're not on a branch (detached HEAD).");
		return GIT_ERROR;
	}
	return err;
}

HashSet<String> changed_paths(git_repository *p_repo, const git_oid *p_from, const git_oid *p_to) {
	HashSet<String> paths;
	CommitPtr from;
	TreePtr from_tree;
	ObjectPtr to; // A commit.
	TreePtr to_tree;
	DiffPtr diff;

	bool ok = git_commit_lookup(from.out(), p_repo, p_from) == 0 && git_commit_tree(from_tree.out(), from) == 0;
	if (ok && p_to) {
		ok = git_object_lookup(to.out(), p_repo, p_to, GIT_OBJECT_COMMIT) == 0;
	} else if (ok) {
		ok = git_revparse_single(to.out(), p_repo, "HEAD^{commit}") == 0;
	}
	ok = ok && git_commit_tree(to_tree.out(), (git_commit *)to.get()) == 0 && git_diff_tree_to_tree(diff.out(), p_repo, from_tree, to_tree, nullptr) == 0;
	if (ok) {
		for (size_t i = 0; i < git_diff_num_deltas(diff); i++) {
			const git_diff_delta *delta = git_diff_get_delta(diff, i);
			paths.insert(String::utf8(delta->old_file.path));
			paths.insert(String::utf8(delta->new_file.path));
		}
	}
	return paths;
}

PackedStringArray stashed_paths(git_repository *p_repo, const git_oid *p_stash) {
	PackedStringArray result;
	CommitPtr stash;
	if (git_commit_lookup(stash.out(), p_repo, p_stash) == 0 && git_commit_parentcount(stash) > 0) {
		for (const String &path : changed_paths(p_repo, git_commit_parent_id(stash, 0), p_stash)) {
			result.push_back(path);
		}
	}
	return result;
}

PackedStringArray uncommitted_paths(git_repository *p_repo) {
	PackedStringArray result;
	git_status_options opts = GIT_STATUS_OPTIONS_INIT;
	opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
	StatusListPtr status;
	if (git_status_list_new(status.out(), p_repo, &opts) == 0) {
		for (size_t i = 0; i < git_status_list_entrycount(status); i++) {
			const git_status_entry *entry = git_status_byindex(status, i);
			for (const git_diff_delta *delta : { entry->head_to_index, entry->index_to_workdir }) {
				if (delta) {
					result.push_back(String::utf8(delta->old_file.path));
					if (strcmp(delta->old_file.path, delta->new_file.path) != 0) {
						result.push_back(String::utf8(delta->new_file.path));
					}
				}
			}
		}
	}
	return result;
}

void CheckoutGuard::progress_cb(const char *p_path, size_t p_completed, size_t p_total, void *p_payload) {
	CheckoutGuard *guard = static_cast<CheckoutGuard *>(p_payload);
	if (p_path) {
		guard->paths.insert(String::utf8(p_path)); // Includes removals, which aren't notified.
	}
	if (guard->chained_progress) {
		guard->chained_progress(p_path, p_completed, p_total, guard->chained_payload);
	}
}

int CheckoutGuard::notify_cb(git_checkout_notify_t p_why, const char *p_path, const git_diff_file *p_baseline, const git_diff_file *p_target, const git_diff_file *p_workdir, void *p_payload) {
	// Called while planning, before anything changes: also covers a file whose write fails halfway.
	static_cast<CheckoutGuard *>(p_payload)->paths.insert(String::utf8(p_path));
	return 0;
}

void CheckoutGuard::attach(git_checkout_options &r_opts) {
	chained_progress = r_opts.progress_cb;
	chained_payload = r_opts.progress_payload;
	r_opts.progress_cb = progress_cb;
	r_opts.progress_payload = this;
	r_opts.notify_flags |= GIT_CHECKOUT_NOTIFY_UPDATED;
	r_opts.notify_cb = notify_cb;
	r_opts.notify_payload = this;
}

PackedStringArray CheckoutGuard::restore(git_repository *p_repo, const git_tree *p_tree) {
	PackedStringArray failed;
	if (paths.is_empty()) {
		return failed;
	}
	const String workdir = String::utf8(git_repository_workdir(p_repo));
	IndexPtr index;
	if (git_repository_index(index.out(), p_repo) < 0) {
		for (const String &path : paths) {
			failed.push_back(path);
		}
		return failed;
	}
	// The failed checkout may have changed the index in memory, but only writes it on success.
	git_index_read(index, true);

	// Files HEAD has: checked out again. Files it doesn't have were created by the checkout
	// (a safe checkout never overwrites an untracked file): removed again.
	LocalVector<CharString> tracked;
	for (const String &path : paths) {
		const CharString utf8 = path.utf8();
		TreeEntryPtr entry;
		if (git_tree_entry_bypath(entry.out(), p_tree, utf8.get_data()) == 0) {
			tracked.push_back(utf8);
			continue;
		}
		const String absolute = workdir.path_join(path);
		if (FileAccess::file_exists(absolute) && DirAccess::remove_absolute(absolute) != OK) {
			failed.push_back(path);
		}
		git_index_remove_bypath(index, utf8.get_data());
	}
	git_index_write(index);

	if (!tracked.is_empty()) {
		LocalVector<char *> pointers;
		for (CharString &path : tracked) {
			pointers.push_back(const_cast<char *>(path.get_data()));
		}
		git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
		opts.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;
		opts.paths = { pointers.ptr(), pointers.size() };
		if (git_checkout_tree(p_repo, (const git_object *)p_tree, &opts) < 0) {
			for (const CharString &path : tracked) {
				failed.push_back(String::utf8(path.get_data()));
			}
		}
	}
	return failed;
}

int wait_for_exit_code(int64_t p_pid) {
	OS *os = OS::get_singleton();
	for (int waited = 0; os->is_process_running(p_pid) && waited < 10000; waited += 5) {
		os->delay_msec(5);
	}
	return os->get_process_exit_code(p_pid);
}

ScopedEnvironment::ScopedEnvironment(std::initializer_list<std::pair<const char *, const char *>> p_variables) {
	OS *os = OS::get_singleton();
	for (const std::pair<const char *, const char *> &variable : p_variables) {
		Saved entry;
		entry.name = variable.first;
		entry.existed = os->has_environment(entry.name);
		entry.value = entry.existed ? os->get_environment(entry.name) : String();
		saved.push_back(entry);
		os->set_environment(entry.name, variable.second);
	}
}

ScopedEnvironment::~ScopedEnvironment() {
	OS *os = OS::get_singleton();
	for (const Saved &entry : saved) {
		if (entry.existed) {
			os->set_environment(entry.name, entry.value);
		} else {
			os->unset_environment(entry.name);
		}
	}
}

String last_git_error(int *r_class) {
	const git_error *err = git_error_last();
	const bool real = err && err->klass != GIT_ERROR_NONE && err->message;
	if (r_class) {
		*r_class = real ? err->klass : GIT_ERROR_NONE;
	}
	return real ? String::utf8(err->message) : String();
}

int checkout_all_or_nothing(git_repository *p_repo, const git_object *p_target, git_checkout_options &p_opts) {
	ObjectPtr head_tree;
	if (git_revparse_single(head_tree.out(), p_repo, "HEAD^{tree}") < 0) {
		return git_checkout_tree(p_repo, p_target, &p_opts); // No commits yet: nothing to go back to.
	}
	for (int attempt = 1;; attempt++) {
		CheckoutGuard guard;
		git_checkout_options opts = p_opts;
		guard.attach(opts);
		const int err = git_checkout_tree(p_repo, p_target, &opts);
		if (err >= 0 || err == GIT_ECONFLICT) {
			return err; // Conflicts are found while planning, before anything changes.
		}
		int error_class = GIT_ERROR_NONE;
		const String error = last_git_error(&error_class);
		const PackedStringArray not_restored = guard.restore(p_repo, (const git_tree *)head_tree.get());
		// A file in use (an import, an antivirus scan) is usually free again a moment later.
		if (error_class == GIT_ERROR_OS && not_restored.is_empty() && attempt < 3) {
			OS::get_singleton()->delay_msec(500);
			continue;
		}
		explain_checkout_failure(p_repo, error, not_restored);
		return err;
	}
}

Error explain_checkout_failure(git_repository *p_repo, const String &p_error, const PackedStringArray &p_not_restored) {
	// libgit2 names the file in quotes, as an absolute path.
	String reason = p_error;
	const int open = p_error.find("'");
	const int close = open >= 0 ? p_error.find("'", open + 1) : -1;
	if (close > open) {
		const String workdir = String::utf8(git_repository_workdir(p_repo));
		const String path = p_error.substr(open + 1, close - open - 1).trim_prefix(workdir);
		const String cause = p_error.substr(close + 1).trim_prefix(":").strip_edges();
		reason = vformat("%s couldn't be changed (%s). Another program may have it open, such as Godot importing it or an antivirus scanning it; try again in a moment.", path, cause.trim_suffix("."));
	}
	if (!p_not_restored.is_empty()) {
		return fail(vformat("Stopped partway, and these files couldn't be put back as they were: %s. Check them with `git status`. %s", String(", ").join(p_not_restored), reason));
	}
	return fail(vformat("Nothing was changed: %s", reason));
}

} // namespace godot_git
