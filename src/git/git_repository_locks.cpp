// GitRepository: Git LFS file locking (`git lfs lock`), for files that can't be merged (scenes
// with binary parts, art, audio). Locks live on the LFS server, so each of these talks to it
// through git-lfs, with git's own login; run them on a worker thread.

#include "git/git_repository.h"

#include <git2.h>
#include <git2/sys/errors.h>

#include <godot_cpp/classes/json.hpp>

#include "git/git_cli.h"
#include "git/git_lfs.h"
#include "git/git_remote_callbacks.h"
#include "git/git_util.h"

using namespace godot_git;

namespace {

// git-lfs's JSON from merged output (a warning line may come before it).
Variant parse_json_output(const String &p_output) {
	const int from = p_output.find("{");
	const int to = p_output.rfind("}");
	if (from < 0 || to < from) {
		return Variant();
	}
	return JSON::parse_string(p_output.substr(from, to - from + 1));
}

Dictionary lock_entry(const Dictionary &p_lock, bool p_mine) {
	Dictionary entry;
	entry["id"] = p_lock.get("id", String());
	entry["path"] = p_lock.get("path", String());
	entry["owner"] = Dictionary(p_lock.get("owner", Dictionary())).get("name", String());
	entry["locked_at"] = p_lock.get("locked_at", String());
	entry["mine"] = p_mine;
	return entry;
}

} // namespace

// Runs `git lfs <p_args>` for the lock commands; r_output is what it printed.
Error GitRepository::_run_lfs_lock_command(const PackedStringArray &p_args, const String &p_step, String &r_output) {
	if (require_git("Locking files needs git and Git LFS.") != OK) {
		return FAILED;
	}
	if (!lfs_installed()) {
		return fail("Locking files needs Git LFS, which isn't installed. Get it from git-lfs.com.");
	}
	begin_network_operation();
	RemoteContext ctx;
	ctx.progress = progress_callback;
	int exit_code = 0;
	PackedStringArray args;
	args.push_back("-c");
	args.push_back(login_prompts_allowed ? String("credential.interactive=always") : String("credential.interactive=never"));
	args.push_back("lfs");
	args.append_array(p_args);
	const Error err = run_git_command(repo, ctx, args, p_step, r_output, exit_code, true);
	if (err != OK) {
		return err;
	}
	if (exit_code != 0) {
		const String tail = output_tail(r_output, 4);
		if (tail.contains("Not Found") || tail.contains("404") || tail.contains("not supported")) {
			return fail("The LFS server of this repository doesn't support locking files.");
		}
		return fail(tail.is_empty() ? String("Git LFS refused, without saying why.") : tail);
	}
	return OK;
}

// The files locked on the LFS server: [{ "path", "owner", "id", "locked_at", "mine": locked by
// you }]. Empty, with get_last_error() set, when they can't be read (offline, no LFS, a server
// without locking).
Array GitRepository::get_lfs_locks() {
	Array result;
	ERR_FAIL_NULL_V_MSG(repo, result, "Repository is not open.");
	git_error_clear();
	String output;
	if (_run_lfs_lock_command(PackedStringArray({ "locks", "--verify", "--json" }), "Reading locks...", output) != OK) {
		return result;
	}
	const Variant parsed = parse_json_output(output);
	if (parsed.get_type() != Variant::DICTIONARY) {
		fail("Git LFS answered with something that isn't a list of locks.");
		return result;
	}
	const Dictionary locks = parsed;
	for (const char *side : { "ours", "theirs" }) {
		const Array list = locks.get(side, Array());
		for (int i = 0; i < list.size(); i++) {
			result.push_back(lock_entry(list[i], String(side) == "ours"));
		}
	}
	return result;
}

// Whether p_path is stored with Git LFS (only those can be locked here).
bool GitRepository::is_lfs_file(const String &p_path) const {
	ERR_FAIL_NULL_V_MSG(repo, false, "Repository is not open.");
	return repo_uses_lfs(repo) && is_lfs_path(repo, p_path.utf8().get_data());
}

// Locks p_path on the LFS server, so nobody else can push changes to it until you unlock it.
Error GitRepository::lock_file(const String &p_path) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	String output;
	const Error err = _run_lfs_lock_command(PackedStringArray({ "lock", "--json", p_path }), "Locking...", output);
	if (err != OK && output.contains("already created")) {
		return fail(vformat("%s is already locked.", p_path.get_file()));
	}
	return err;
}

// Unlocks p_path. p_force also takes someone else's lock away (the server may refuse).
Error GitRepository::unlock_file(const String &p_path, bool p_force) {
	ERR_FAIL_NULL_V_MSG(repo, ERR_UNCONFIGURED, "Repository is not open.");
	git_error_clear();
	PackedStringArray args({ "unlock", "--json" });
	if (p_force) {
		args.push_back("--force");
	}
	args.push_back(p_path);
	String output;
	return _run_lfs_lock_command(args, "Unlocking...", output);
}
