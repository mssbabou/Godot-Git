#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

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
	Array get_commits(int p_max_count) const;

	Error stage(const String &p_path);
	Error unstage(const String &p_path);
	Error stage_all();
	Error unstage_all();
	Error discard(const String &p_path);
	Error commit(const String &p_message);
	Error checkout_branch(const String &p_branch);
	Error create_branch(const String &p_name);

	Error fetch();
	Error pull();
	Error push();
	String get_notice() const;
	Dictionary get_pull_result() const;
	void set_progress_callback(const Callable &p_callback);
	void set_login_prompts_allowed(bool p_allowed);

	static void cancel_network();
	static String get_last_error();
	static String get_libgit2_version();

	~GitRepository();
};
