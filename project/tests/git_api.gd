## Mirrors GitRepository's API for the tests. GitRepository used to bind one method per call to
## GDScript; the library now has two bindings, call_api and call_static_api, which take a method's
## name and its arguments in an Array. This file keeps the old names, parameters, defaults and return
## types, so the tests read the same as before. The editor never uses it.
extends RefCounted

var _r := GitRepository.new()


func open(path: String) -> Error:
	return _r.call_api("open", [path])

func is_open() -> bool:
	return _r.call_api("is_open", [])

func get_workdir() -> String:
	return _r.call_api("get_workdir", [])

func get_current_branch() -> String:
	return _r.call_api("get_current_branch", [])

func get_branches() -> PackedStringArray:
	return _r.call_api("get_branches", [])

func get_branch_list() -> Array:
	return _r.call_api("get_branch_list", [])

func get_remote_branches() -> PackedStringArray:
	return _r.call_api("get_remote_branches", [])

func get_remotes() -> PackedStringArray:
	return _r.call_api("get_remotes", [])

func get_sync_status() -> Dictionary:
	return _r.call_api("get_sync_status", [])

func get_status() -> Array:
	return _r.call_api("get_status", [])

func get_line_stats(staged: bool) -> Dictionary:
	return _r.call_api("get_line_stats", [staged])

func get_diff(path: String, staged: bool) -> Dictionary:
	return _r.call_api("get_diff", [path, staged])

func get_commit_files(hash: String) -> Array:
	return _r.call_api("get_commit_files", [hash])

func get_commit_diff(hash: String, path: String) -> Dictionary:
	return _r.call_api("get_commit_diff", [hash, path])

func get_file_bytes(version: String, path: String) -> Dictionary:
	return _r.call_api("get_file_bytes", [version, path])

func get_commits(max_count: int = 50, path: String = "", query: String = "") -> Array:
	return _r.call_api("get_commits", [max_count, path, query])

func get_commit(revision: String) -> Dictionary:
	return _r.call_api("get_commit", [revision])

func get_line_commit(path: String, text: String, line: int) -> Dictionary:
	return _r.call_api("get_line_commit", [path, text, line])

func create_branch_at(name: String, hash: String) -> Error:
	return _r.call_api("create_branch_at", [name, hash])

func restore_file_version(revision: String, path: String) -> Error:
	return _r.call_api("restore_file_version", [revision, path])

func get_ignore_file(dir: String) -> String:
	return _r.call_api("get_ignore_file", [dir])

func get_paths_ignored_by(ignore_file: String, lines: PackedStringArray, paths: PackedStringArray) -> PackedStringArray:
	return _r.call_api("get_paths_ignored_by", [ignore_file, lines, paths])

func add_ignore_lines(ignore_file: String, lines: PackedStringArray) -> Error:
	return _r.call_api("add_ignore_lines", [ignore_file, lines])

func track_with_lfs(patterns: PackedStringArray, paths: PackedStringArray) -> Error:
	return _r.call_api("track_with_lfs", [patterns, paths])

func get_conflict(path: String) -> Dictionary:
	return _r.call_api("get_conflict", [path])

func resolve_conflict(path: String, text: String) -> Error:
	return _r.call_api("resolve_conflict", [path, text])

func resolve_conflict_with(path: String, side: String) -> Error:
	return _r.call_api("resolve_conflict_with", [path, side])

func resolve_settings_conflict(path: String, choices: Dictionary) -> Error:
	return _r.call_api("resolve_settings_conflict", [path, choices])

func undo_last_commit() -> Error:
	return _r.call_api("undo_last_commit", [])

func get_undo() -> Dictionary:
	return _r.call_api("get_undo", [])

func undo_last_operation() -> Error:
	return _r.call_api("undo_last_operation", [])

func apply_line_changes(path: String, staged: bool, action: String, lines: Array) -> Error:
	return _r.call_api("apply_line_changes", [path, staged, action, lines])

func revert_commit(hash: String) -> Error:
	return _r.call_api("revert_commit", [hash])

func uses_lfs() -> bool:
	return _r.call_api("uses_lfs", [])

func has_file_at(revision: String, path: String) -> bool:
	return _r.call_api("has_file_at", [revision, path])

func get_git_needs() -> Dictionary:
	return _r.call_api("get_git_needs", [])

func get_remote_url(remote: String) -> String:
	return _r.call_api("get_remote_url", [remote])

func get_large_staged_files(min_size: int) -> Array:
	return _r.call_api("get_large_staged_files", [min_size])

func get_pull_blockers() -> PackedStringArray:
	return _r.call_api("get_pull_blockers", [])

func get_stashes() -> Array:
	return _r.call_api("get_stashes", [])

func get_stash_files(hash: String) -> Array:
	return _r.call_api("get_stash_files", [hash])

func get_stash_diff(hash: String, path: String) -> Dictionary:
	return _r.call_api("get_stash_diff", [hash, path])

func stash(staged: bool, message: String = "") -> Error:
	return _r.call_api("stash", [staged, message])

func restore_stash(hash: String, merge: bool = false) -> Error:
	return _r.call_api("restore_stash", [hash, merge])

func get_stash_conflicts(hash: String) -> PackedStringArray:
	return _r.call_api("get_stash_conflicts", [hash])

func get_lfs_locks() -> Array:
	return _r.call_api("get_lfs_locks", [])

func set_diff_options(context: int, ignore_whitespace: bool) -> void:
	_r.call_api("set_diff_options", [context, ignore_whitespace])

func get_pull_leftovers() -> Array:
	return _r.call_api("get_pull_leftovers", [])

func resolve_pull_leftovers(folder: String, put_back: bool) -> Error:
	return _r.call_api("resolve_pull_leftovers", [folder, put_back])

func get_commit_details(hash: String) -> Dictionary:
	return _r.call_api("get_commit_details", [hash])

func is_lfs_file(path: String) -> bool:
	return _r.call_api("is_lfs_file", [path])

func lock_file(path: String) -> Error:
	return _r.call_api("lock_file", [path])

func unlock_file(path: String, force: bool = false) -> Error:
	return _r.call_api("unlock_file", [path, force])

func delete_stash(hash: String) -> Error:
	return _r.call_api("delete_stash", [hash])

func get_operation() -> Dictionary:
	return _r.call_api("get_operation", [])

func stage(path: String) -> Error:
	return _r.call_api("stage", [path])

func unstage(path: String) -> Error:
	return _r.call_api("unstage", [path])

func stage_all() -> Error:
	return _r.call_api("stage_all", [])

func unstage_all() -> Error:
	return _r.call_api("unstage_all", [])

func discard(path: String) -> Error:
	return _r.call_api("discard", [path])

func commit(message: String) -> Error:
	return _r.call_api("commit", [message])

func amend(message: String) -> Error:
	return _r.call_api("amend", [message])

func is_head_pushed() -> bool:
	return _r.call_api("is_head_pushed", [])

func commit_runs_git(amend: bool) -> bool:
	return _r.call_api("commit_runs_git", [amend])

func checkout_branch(branch: String) -> Error:
	return _r.call_api("checkout_branch", [branch])

func create_branch(name: String) -> Error:
	return _r.call_api("create_branch", [name])

func rename_branch(branch: String, new_name: String) -> Error:
	return _r.call_api("rename_branch", [branch, new_name])

func delete_branch(branch: String) -> Error:
	return _r.call_api("delete_branch", [branch])

func get_branch_details(branch: String) -> Dictionary:
	return _r.call_api("get_branch_details", [branch])

func add_remote(name: String, url: String) -> Error:
	return _r.call_api("add_remote", [name, url])

func get_identity() -> Dictionary:
	return _r.call_api("get_identity", [])

func set_identity(name: String, email: String, global: bool) -> Error:
	return _r.call_api("set_identity", [name, email, global])

func fetch() -> Error:
	return _r.call_api("fetch", [])

func pull(start_merge: bool = false) -> Error:
	return _r.call_api("pull", [start_merge])

func get_pull_conflicts() -> PackedStringArray:
	return _r.call_api("get_pull_conflicts", [])

func merge_branch(branch: String, start_merge: bool = false) -> Error:
	return _r.call_api("merge_branch", [branch, start_merge])

func get_merge_branches() -> Array:
	return _r.call_api("get_merge_branches", [])

func get_merge_preview(branch: String) -> Dictionary:
	return _r.call_api("get_merge_preview", [branch])

func push() -> Error:
	return _r.call_api("push", [])

func abort_operation() -> Error:
	return _r.call_api("abort_operation", [])

func continue_operation() -> Error:
	return _r.call_api("continue_operation", [])

func get_notice() -> String:
	return _r.call_api("get_notice", [])

func get_pull_result() -> Dictionary:
	return _r.call_api("get_pull_result", [])

func set_progress_callback(callback: Callable) -> void:
	_r.call_api("set_progress_callback", [callback])

func set_login_prompts_allowed(allowed: bool) -> void:
	_r.call_api("set_login_prompts_allowed", [allowed])

func get_saved_login(url: String) -> Dictionary:
	return _r.call_api("get_saved_login", [url])


static func is_lfs_installed() -> bool:
	return GitRepository.call_static_api("is_lfs_installed", [])

static func init_repository(path: String, project_path: String, lfs: bool = false) -> Error:
	return GitRepository.call_static_api("init_repository", [path, project_path, lfs])

static func set_config_home(path: String) -> void:
	GitRepository.call_static_api("set_config_home", [path])

static func cancel_network() -> void:
	GitRepository.call_static_api("cancel_network", [])

static func get_last_error() -> String:
	return GitRepository.call_static_api("get_last_error", [])

static func get_libgit2_version() -> String:
	return GitRepository.call_static_api("get_libgit2_version", [])

static func merge_scene(base: String, mine: String, theirs: String) -> Dictionary:
	return GitRepository.call_static_api("merge_scene", [base, mine, theirs])


static func diff_lines(old: String, new: String) -> Array:
	return GitRepository.call_static_api("diff_lines", [old, new])

static func is_git_installed() -> bool:
	return GitRepository.call_static_api("is_git_installed", [])

static func check_git_installed() -> bool:
	return GitRepository.call_static_api("check_git_installed", [])

static func set_git_program(program: String) -> void:
	GitRepository.call_static_api("set_git_program", [program])
