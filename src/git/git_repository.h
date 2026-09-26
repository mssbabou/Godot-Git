#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

struct git_oid;
struct git_repository;

using namespace godot;

// Thin wrapper around a libgit2 repository handle.
// Paths passed in and returned are relative to the repository's working directory.
//
// A GitRepository must only be used from one thread at a time. For network operations
// (fetch/pull/push) open a separate instance on the worker thread.
class GitRepository : public RefCounted {
	GDCLASS(GitRepository, RefCounted)

	git_repository *repo = nullptr;
	String notice;
	int pulled_commits = 0;
	bool pull_merged = false;
	Callable progress_callback;
	bool login_prompts_allowed = true;

	void close();
	Error _fetch_remote(const String &p_remote);
	Error _fetch_lfs_files(const char *p_refname, const git_oid *p_commit);
	Error _commit_with_git(const String &p_message, bool p_amend);
	Error _push_with_git(const String &p_remote, const String &p_refspec, bool p_ssh);
	Error _fetch_with_git(const String &p_remote);

protected:
	static void _bind_methods();

public:
	Error open(const String &p_path);
	bool is_open() const;

	String get_workdir() const;
	String get_current_branch() const;
	PackedStringArray get_branches() const;
	PackedStringArray get_remote_branches() const;
	PackedStringArray get_remotes() const;
	Dictionary get_sync_status() const;
	Array get_status() const;
	Dictionary get_line_stats(bool p_staged) const;
	Dictionary get_diff(const String &p_path, bool p_staged) const;
	Array get_commits(int p_max_count) const;
	bool uses_lfs() const;
	bool has_file_at(const String &p_revision, const String &p_path) const;
	Dictionary get_git_needs() const;

	Error stage(const String &p_path);
	Error unstage(const String &p_path);
	Error stage_all();
	Error unstage_all();
	Error discard(const String &p_path);
	Error commit(const String &p_message);
	Error amend(const String &p_message);
	bool commit_runs_git(bool p_amend) const;
	bool is_head_pushed() const;
	Error checkout_branch(const String &p_branch);
	Error create_branch(const String &p_name);
	Error add_remote(const String &p_name, const String &p_url);
	Dictionary get_identity() const;
	Error set_identity(const String &p_name, const String &p_email, bool p_global);

	Error fetch();
	Error pull();
	Error push();
	String get_notice() const;
	Dictionary get_pull_result() const;
	void set_progress_callback(const Callable &p_callback);
	void set_login_prompts_allowed(bool p_allowed);

	static Error init_repository(const String &p_path, const String &p_project_path);
	static void set_config_home(const String &p_path);

	static void cancel_network();
	static bool is_git_installed();
	static bool check_git_installed();
	static void set_git_program(const String &p_program);
	static String get_last_error();
	static String get_libgit2_version();

	~GitRepository();
};
