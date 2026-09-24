#include "git/git_cli.h"

#include <git2/sys/errors.h>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>

#include <string>

#include "git/git_util.h"

namespace godot_git {

namespace {

String hooks_dir(git_repository *p_repo) {
	ConfigPtr config;
	git_buf path = GIT_BUF_INIT;
	if (git_repository_config_snapshot(config.out(), p_repo) == 0 && git_config_get_path(&path, config, "core.hooksPath") == 0) {
		const String dir = buf_to_string(path);
		// Relative to where hooks run: the top of the working tree.
		const char *workdir = git_repository_workdir(p_repo);
		return dir.is_relative_path() && workdir ? String::utf8(workdir).path_join(dir) : dir;
	}
	git_buf_dispose(&path);
	if (git_repository_item_path(&path, p_repo, GIT_REPOSITORY_ITEM_HOOKS) == 0) {
		return buf_to_string(path);
	}
	git_buf_dispose(&path);
	return String();
}

bool signs_commits(git_repository *p_repo) {
	ConfigPtr config;
	int sign = 0;
	return git_repository_config_snapshot(config.out(), p_repo) == 0 && git_config_get_bool(&sign, config, "commit.gpgsign") == 0 && sign;
}

// "Writing objects:  45% (9/20), 1.2 KiB" -> 0.45; -1 for a line that isn't progress.
double progress_fraction(const String &p_line) {
	const int open = p_line.rfind("(");
	const int slash = p_line.find("/", open);
	const int close = p_line.find(")", slash);
	if (open < 0 || slash < 0 || close < 0 || !p_line.contains("%")) {
		return -1;
	}
	const int64_t done = p_line.substr(open + 1, slash - open - 1).to_int();
	const int64_t total = p_line.substr(slash + 1, close - slash - 1).to_int();
	return total > 0 ? (double)done / total : -1;
}

} // namespace

bool has_hook(git_repository *p_repo, const char *p_name) {
	const String dir = hooks_dir(p_repo);
	return !dir.is_empty() && FileAccess::file_exists(dir.path_join(p_name));
}

bool commit_needs_git(git_repository *p_repo, CommitKind p_kind) {
	if (signs_commits(p_repo)) {
		return true;
	}
	for (const char *hook : { "pre-commit", "prepare-commit-msg", "commit-msg", "post-commit" }) {
		if (has_hook(p_repo, hook)) {
			return true;
		}
	}
	return p_kind == COMMIT_AMEND && has_hook(p_repo, "post-rewrite");
}

Error run_git_command(git_repository *p_repo, RemoteContext &p_ctx, const PackedStringArray &p_args, const String &p_step, String &r_output, int &r_exit_code) {
	// No terminal to ask anything in.
	OS::get_singleton()->set_environment("GIT_TERMINAL_PROMPT", "0");
	const char *workdir_path = git_repository_workdir(p_repo);
	const String workdir = workdir_path ? String::utf8(workdir_path) : String();

	// stdout and stderr merged by a shell: Godot gives stderr its own pipe, and a pipe nobody
	// reads fills up and blocks the process for good.
	PackedStringArray args;
	String program;
	if (OS::get_singleton()->get_name() == "Windows") {
		// cmd gets one string (Godot quotes each argument; see CLAUDE.md gotcha 24).
		// Forward slashes: a quoted path ending in a backslash would read as an escaped quote.
		String command = vformat("git -C \"%s\"", workdir);
		for (const String &arg : p_args) {
			command += vformat(" \"%s\"", arg);
		}
		program = "cmd";
		args.push_back("/c");
		args.push_back(command + " 2>&1");
	} else {
		program = "sh";
		args.push_back("-c");
		args.push_back("exec git \"$@\" 2>&1");
		args.push_back("sh");
		args.push_back("-C");
		args.push_back(workdir);
		args.append_array(p_args);
	}

	report_progress(&p_ctx, p_step, String(), -1);
	Dictionary process = OS::get_singleton()->execute_with_pipe(program, args, true);
	Ref<FileAccess> io = process.get("stdio", Variant());
	if (io.is_null()) {
		return fail("Couldn't start git.");
	}
	const int64_t pid = process.get("pid", -1);
	track_process(pid);

	// Progress lines end in \r, everything else in \n.
	std::string bytes;
	while (true) {
		const uint8_t byte = io->get_8();
		const bool closed = io->get_error() != OK;
		if (!closed && byte != '\r' && byte != '\n') {
			bytes += (char)byte;
			continue;
		}
		const String line = String::utf8(bytes.data(), bytes.size()).strip_edges();
		bytes.clear();
		if (!line.is_empty()) {
			r_output += line + String("\n");
			const double fraction = progress_fraction(line);
			const int colon = line.find(":");
			if (fraction >= 0 && colon > 0) {
				report_progress(&p_ctx, line.substr(0, colon + 1), line.substr(colon + 1).strip_edges(), fraction);
			} else {
				report_progress(&p_ctx, p_step, line, -1);
			}
		}
		if (closed) {
			break;
		}
	}
	track_process(0);
	r_exit_code = OS::get_singleton()->get_process_exit_code(pid);

	if (is_cancel_requested()) {
		git_error_set_str(GIT_ERROR_NET, "Canceled.");
		return ERR_SKIP;
	}
	return OK;
}

String output_tail(const String &p_output, int p_count) {
	const PackedStringArray lines = p_output.split("\n", false);
	return String("\n").join(lines.slice(MAX(0, lines.size() - p_count)));
}

Error commit_with_git(git_repository *p_repo, RemoteContext &p_ctx, const String &p_message, CommitKind p_kind) {
	// The message goes through a file: exact, whatever it contains.
	const String message_path = String::utf8(git_repository_path(p_repo)).path_join("GODOT_GIT_COMMIT_MSG");
	{
		Ref<FileAccess> file = FileAccess::open(message_path, FileAccess::WRITE);
		if (file.is_null()) {
			return fail("Couldn't write the commit message for git.");
		}
		file->store_string(p_message);
	}
	PackedStringArray args;
	args.push_back("commit");
	args.push_back("--quiet"); // Only git's own summary; hooks still print.
	args.push_back("-F");
	args.push_back(message_path);
	if (p_kind == COMMIT_AMEND) {
		args.push_back("--amend");
	}
	String output;
	int exit_code = 0;
	const Error err = run_git_command(p_repo, p_ctx, args, p_kind == COMMIT_AMEND ? String("Amending...") : String("Committing..."), output, exit_code);
	DirAccess::remove_absolute(message_path);
	if (err != OK) {
		return err;
	}
	if (exit_code != 0) {
		const String tail = output_tail(output, 8);
		return fail(tail.is_empty() ? String("Git didn't make the commit, without saying why.") : vformat("Git didn't make the commit (a hook or signing stopped it):\n%s", tail));
	}
	return OK;
}

} // namespace godot_git
