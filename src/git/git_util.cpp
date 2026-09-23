#include "git/git_util.h"

#include <git2/sys/errors.h>

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
	git_commit *from = nullptr;
	git_commit *to = nullptr;
	git_tree *from_tree = nullptr;
	git_tree *to_tree = nullptr;
	git_diff *diff = nullptr;

	bool ok = git_commit_lookup(&from, p_repo, p_from) == 0 && git_commit_tree(&from_tree, from) == 0;
	if (ok && p_to) {
		ok = git_commit_lookup(&to, p_repo, p_to) == 0;
	} else if (ok) {
		git_object *head = nullptr;
		ok = git_revparse_single(&head, p_repo, "HEAD^{commit}") == 0;
		to = (git_commit *)head;
	}
	ok = ok && git_commit_tree(&to_tree, to) == 0 && git_diff_tree_to_tree(&diff, p_repo, from_tree, to_tree, nullptr) == 0;
	if (ok) {
		for (size_t i = 0; i < git_diff_num_deltas(diff); i++) {
			const git_diff_delta *delta = git_diff_get_delta(diff, i);
			paths.insert(String::utf8(delta->old_file.path));
			paths.insert(String::utf8(delta->new_file.path));
		}
	}

	git_diff_free(diff);
	git_tree_free(to_tree);
	git_tree_free(from_tree);
	git_commit_free(to);
	git_commit_free(from);
	return paths;
}

PackedStringArray stashed_paths(git_repository *p_repo, const git_oid *p_stash) {
	PackedStringArray result;
	git_commit *stash = nullptr;
	if (git_commit_lookup(&stash, p_repo, p_stash) == 0 && git_commit_parentcount(stash) > 0) {
		for (const String &path : changed_paths(p_repo, git_commit_parent_id(stash, 0), p_stash)) {
			result.push_back(path);
		}
	}
	git_commit_free(stash);
	return result;
}

PackedStringArray uncommitted_paths(git_repository *p_repo) {
	PackedStringArray result;
	git_status_options opts = GIT_STATUS_OPTIONS_INIT;
	opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
	git_status_list *status = nullptr;
	if (git_status_list_new(&status, p_repo, &opts) == 0) {
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
	git_status_list_free(status);
	return result;
}

} // namespace godot_git
