// GitRepository's name-based entry points (call_api, call_static_api). The tests call the library
// through them (project/tests/git_api.gd mirrors the old per-method bindings), so the library carries
// two bindings instead of one per method. The editor calls the C++ methods directly and never comes here.

#include "git/git_repository.h"

#include <godot_cpp/variant/utility_functions.hpp>

namespace {

// Every argument is always passed by the wrapper, defaults included.
bool arg_count(const String &p_name, const Array &p_args, int p_count) {
	if (p_args.size() == p_count) {
		return true;
	}
	UtilityFunctions::push_error(vformat("GitRepository.%s takes %d arguments, got %d.", p_name, p_count, p_args.size()));
	return false;
}

PackedStringArray instance_names() {
	PackedStringArray names;
	names.push_back("open");
	names.push_back("is_open");
	names.push_back("get_workdir");
	names.push_back("get_current_branch");
	names.push_back("get_branches");
	names.push_back("get_branch_list");
	names.push_back("get_remote_branches");
	names.push_back("get_remotes");
	names.push_back("get_sync_status");
	names.push_back("get_status");
	names.push_back("get_line_stats");
	names.push_back("get_diff");
	names.push_back("get_commit_files");
	names.push_back("get_commit_diff");
	names.push_back("get_file_bytes");
	names.push_back("get_commits");
	names.push_back("get_commit");
	names.push_back("get_line_commit");
	names.push_back("create_branch_at");
	names.push_back("restore_file_version");
	names.push_back("get_ignore_file");
	names.push_back("get_paths_ignored_by");
	names.push_back("add_ignore_lines");
	names.push_back("track_with_lfs");
	names.push_back("get_conflict");
	names.push_back("resolve_conflict");
	names.push_back("resolve_conflict_with");
	names.push_back("resolve_settings_conflict");
	names.push_back("undo_last_commit");
	names.push_back("get_undo");
	names.push_back("undo_last_operation");
	names.push_back("apply_line_changes");
	names.push_back("revert_commit");
	names.push_back("uses_lfs");
	names.push_back("has_file_at");
	names.push_back("get_git_needs");
	names.push_back("get_remote_url");
	names.push_back("get_large_staged_files");
	names.push_back("get_pull_blockers");
	names.push_back("get_stashes");
	names.push_back("get_stash_files");
	names.push_back("get_stash_diff");
	names.push_back("stash");
	names.push_back("restore_stash");
	names.push_back("get_stash_conflicts");
	names.push_back("get_lfs_locks");
	names.push_back("set_diff_options");
	names.push_back("get_pull_leftovers");
	names.push_back("resolve_pull_leftovers");
	names.push_back("get_commit_details");
	names.push_back("is_lfs_file");
	names.push_back("lock_file");
	names.push_back("unlock_file");
	names.push_back("delete_stash");
	names.push_back("get_operation");
	names.push_back("stage");
	names.push_back("unstage");
	names.push_back("stage_all");
	names.push_back("unstage_all");
	names.push_back("discard");
	names.push_back("commit");
	names.push_back("amend");
	names.push_back("is_head_pushed");
	names.push_back("commit_runs_git");
	names.push_back("checkout_branch");
	names.push_back("create_branch");
	names.push_back("rename_branch");
	names.push_back("delete_branch");
	names.push_back("get_branch_details");
	names.push_back("add_remote");
	names.push_back("get_identity");
	names.push_back("set_identity");
	names.push_back("fetch");
	names.push_back("pull");
	names.push_back("get_pull_conflicts");
	names.push_back("merge_branch");
	names.push_back("get_merge_branches");
	names.push_back("get_merge_preview");
	names.push_back("push");
	names.push_back("abort_operation");
	names.push_back("continue_operation");
	names.push_back("get_notice");
	names.push_back("get_pull_result");
	names.push_back("set_progress_callback");
	names.push_back("set_login_prompts_allowed");
	names.push_back("get_saved_login");
	return names;
}

PackedStringArray static_names() {
	PackedStringArray names;
	names.push_back("is_lfs_installed");
	names.push_back("init_repository");
	names.push_back("set_config_home");
	names.push_back("cancel_network");
	names.push_back("get_last_error");
	names.push_back("get_libgit2_version");
	names.push_back("diff_lines");
	names.push_back("is_git_installed");
	names.push_back("check_git_installed");
	names.push_back("set_git_program");
	return names;
}

} // namespace

Variant GitRepository::call_api(const String &p_name, const Array &p_args) {
	if (p_name == "api_names") {
		Dictionary names;
		names["instance"] = instance_names();
		names["static"] = static_names();
		return names;
	}

	if (p_name == "open") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return int64_t(open(String(p_args[0])));
	}
	if (p_name == "is_open") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return is_open();
	}
	if (p_name == "get_workdir") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_workdir();
	}
	if (p_name == "get_current_branch") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_current_branch();
	}
	if (p_name == "get_branches") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_branches();
	}
	if (p_name == "get_branch_list") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_branch_list();
	}
	if (p_name == "get_remote_branches") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_remote_branches();
	}
	if (p_name == "get_remotes") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_remotes();
	}
	if (p_name == "get_sync_status") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_sync_status();
	}
	if (p_name == "get_status") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_status();
	}
	if (p_name == "get_line_stats") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return get_line_stats(bool(p_args[0]));
	}
	if (p_name == "get_diff") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return get_diff(String(p_args[0]), bool(p_args[1]));
	}
	if (p_name == "get_commit_files") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return get_commit_files(String(p_args[0]));
	}
	if (p_name == "get_commit_diff") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return get_commit_diff(String(p_args[0]), String(p_args[1]));
	}
	if (p_name == "get_file_bytes") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return get_file_bytes(String(p_args[0]), String(p_args[1]));
	}
	if (p_name == "get_commits") {
		if (!arg_count(p_name, p_args, 3)) {
			return Variant();
		}
		return get_commits(int(int64_t(p_args[0])), String(p_args[1]), String(p_args[2]));
	}
	if (p_name == "get_commit") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return get_commit(String(p_args[0]));
	}
	if (p_name == "get_line_commit") {
		if (!arg_count(p_name, p_args, 3)) {
			return Variant();
		}
		return get_line_commit(String(p_args[0]), String(p_args[1]), int(int64_t(p_args[2])));
	}
	if (p_name == "create_branch_at") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(create_branch_at(String(p_args[0]), String(p_args[1])));
	}
	if (p_name == "restore_file_version") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(restore_file_version(String(p_args[0]), String(p_args[1])));
	}
	if (p_name == "get_ignore_file") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return get_ignore_file(String(p_args[0]));
	}
	if (p_name == "get_paths_ignored_by") {
		if (!arg_count(p_name, p_args, 3)) {
			return Variant();
		}
		return get_paths_ignored_by(String(p_args[0]), PackedStringArray(p_args[1]), PackedStringArray(p_args[2]));
	}
	if (p_name == "add_ignore_lines") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(add_ignore_lines(String(p_args[0]), PackedStringArray(p_args[1])));
	}
	if (p_name == "track_with_lfs") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(track_with_lfs(PackedStringArray(p_args[0]), PackedStringArray(p_args[1])));
	}
	if (p_name == "get_conflict") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return get_conflict(String(p_args[0]));
	}
	if (p_name == "resolve_conflict") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(resolve_conflict(String(p_args[0]), String(p_args[1])));
	}
	if (p_name == "resolve_conflict_with") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(resolve_conflict_with(String(p_args[0]), String(p_args[1])));
	}
	if (p_name == "resolve_settings_conflict") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(resolve_settings_conflict(String(p_args[0]), Dictionary(p_args[1])));
	}
	if (p_name == "undo_last_commit") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return int64_t(undo_last_commit());
	}
	if (p_name == "get_undo") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_undo();
	}
	if (p_name == "undo_last_operation") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return int64_t(undo_last_operation());
	}
	if (p_name == "apply_line_changes") {
		if (!arg_count(p_name, p_args, 4)) {
			return Variant();
		}
		return int64_t(apply_line_changes(String(p_args[0]), bool(p_args[1]), String(p_args[2]), Array(p_args[3])));
	}
	if (p_name == "revert_commit") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return int64_t(revert_commit(String(p_args[0])));
	}
	if (p_name == "uses_lfs") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return uses_lfs();
	}
	if (p_name == "has_file_at") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return has_file_at(String(p_args[0]), String(p_args[1]));
	}
	if (p_name == "get_git_needs") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_git_needs();
	}
	if (p_name == "get_remote_url") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return get_remote_url(String(p_args[0]));
	}
	if (p_name == "get_large_staged_files") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return get_large_staged_files(int64_t(p_args[0]));
	}
	if (p_name == "get_pull_blockers") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_pull_blockers();
	}
	if (p_name == "get_stashes") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_stashes();
	}
	if (p_name == "get_stash_files") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return get_stash_files(String(p_args[0]));
	}
	if (p_name == "get_stash_diff") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return get_stash_diff(String(p_args[0]), String(p_args[1]));
	}
	if (p_name == "stash") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(stash(bool(p_args[0]), String(p_args[1])));
	}
	if (p_name == "restore_stash") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(restore_stash(String(p_args[0]), bool(p_args[1])));
	}
	if (p_name == "get_stash_conflicts") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return get_stash_conflicts(String(p_args[0]));
	}
	if (p_name == "get_lfs_locks") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_lfs_locks();
	}
	if (p_name == "set_diff_options") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		set_diff_options(int(int64_t(p_args[0])), bool(p_args[1]));
		return Variant();
	}
	if (p_name == "get_pull_leftovers") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_pull_leftovers();
	}
	if (p_name == "resolve_pull_leftovers") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(resolve_pull_leftovers(String(p_args[0]), bool(p_args[1])));
	}
	if (p_name == "get_commit_details") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return get_commit_details(String(p_args[0]));
	}
	if (p_name == "is_lfs_file") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return is_lfs_file(String(p_args[0]));
	}
	if (p_name == "lock_file") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return int64_t(lock_file(String(p_args[0])));
	}
	if (p_name == "unlock_file") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(unlock_file(String(p_args[0]), bool(p_args[1])));
	}
	if (p_name == "delete_stash") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return int64_t(delete_stash(String(p_args[0])));
	}
	if (p_name == "get_operation") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_operation();
	}
	if (p_name == "stage") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return int64_t(stage(String(p_args[0])));
	}
	if (p_name == "unstage") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return int64_t(unstage(String(p_args[0])));
	}
	if (p_name == "stage_all") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return int64_t(stage_all());
	}
	if (p_name == "unstage_all") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return int64_t(unstage_all());
	}
	if (p_name == "discard") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return int64_t(discard(String(p_args[0])));
	}
	if (p_name == "commit") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return int64_t(commit(String(p_args[0])));
	}
	if (p_name == "amend") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return int64_t(amend(String(p_args[0])));
	}
	if (p_name == "is_head_pushed") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return is_head_pushed();
	}
	if (p_name == "commit_runs_git") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return commit_runs_git(bool(p_args[0]));
	}
	if (p_name == "checkout_branch") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return int64_t(checkout_branch(String(p_args[0])));
	}
	if (p_name == "create_branch") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return int64_t(create_branch(String(p_args[0])));
	}
	if (p_name == "rename_branch") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(rename_branch(String(p_args[0]), String(p_args[1])));
	}
	if (p_name == "delete_branch") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return int64_t(delete_branch(String(p_args[0])));
	}
	if (p_name == "get_branch_details") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return get_branch_details(String(p_args[0]));
	}
	if (p_name == "add_remote") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(add_remote(String(p_args[0]), String(p_args[1])));
	}
	if (p_name == "get_identity") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_identity();
	}
	if (p_name == "set_identity") {
		if (!arg_count(p_name, p_args, 3)) {
			return Variant();
		}
		return int64_t(set_identity(String(p_args[0]), String(p_args[1]), bool(p_args[2])));
	}
	if (p_name == "fetch") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return int64_t(fetch());
	}
	if (p_name == "pull") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return int64_t(pull(bool(p_args[0])));
	}
	if (p_name == "get_pull_conflicts") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_pull_conflicts();
	}
	if (p_name == "merge_branch") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return int64_t(merge_branch(String(p_args[0]), bool(p_args[1])));
	}
	if (p_name == "get_merge_branches") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_merge_branches();
	}
	if (p_name == "get_merge_preview") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return get_merge_preview(String(p_args[0]));
	}
	if (p_name == "push") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return int64_t(push());
	}
	if (p_name == "abort_operation") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return int64_t(abort_operation());
	}
	if (p_name == "continue_operation") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return int64_t(continue_operation());
	}
	if (p_name == "get_notice") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_notice();
	}
	if (p_name == "get_pull_result") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_pull_result();
	}
	if (p_name == "set_progress_callback") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		set_progress_callback(Callable(p_args[0]));
		return Variant();
	}
	if (p_name == "set_login_prompts_allowed") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		set_login_prompts_allowed(bool(p_args[0]));
		return Variant();
	}
	if (p_name == "get_saved_login") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		return get_saved_login(String(p_args[0]));
	}

	UtilityFunctions::push_error(vformat("GitRepository has no method %s.", p_name));
	return Variant();
}

Variant GitRepository::call_static_api(const String &p_name, const Array &p_args) {
	if (p_name == "api_names") {
		Dictionary names;
		names["instance"] = instance_names();
		names["static"] = static_names();
		return names;
	}

	if (p_name == "is_lfs_installed") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return is_lfs_installed();
	}
	if (p_name == "init_repository") {
		if (!arg_count(p_name, p_args, 3)) {
			return Variant();
		}
		return int64_t(init_repository(String(p_args[0]), String(p_args[1]), bool(p_args[2])));
	}
	if (p_name == "set_config_home") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		set_config_home(String(p_args[0]));
		return Variant();
	}
	if (p_name == "cancel_network") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		cancel_network();
		return Variant();
	}
	if (p_name == "get_last_error") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_last_error();
	}
	if (p_name == "get_libgit2_version") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return get_libgit2_version();
	}
	if (p_name == "diff_lines") {
		if (!arg_count(p_name, p_args, 2)) {
			return Variant();
		}
		return diff_lines(String(p_args[0]), String(p_args[1]));
	}
	if (p_name == "is_git_installed") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return is_git_installed();
	}
	if (p_name == "check_git_installed") {
		if (!arg_count(p_name, p_args, 0)) {
			return Variant();
		}
		return check_git_installed();
	}
	if (p_name == "set_git_program") {
		if (!arg_count(p_name, p_args, 1)) {
			return Variant();
		}
		set_git_program(String(p_args[0]));
		return Variant();
	}

	UtilityFunctions::push_error(vformat("GitRepository has no static method %s.", p_name));
	return Variant();
}
