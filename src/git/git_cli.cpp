#include "git/git_cli.h"

#include <git2/sys/errors.h>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>

#include <atomic>
#include <mutex>
#include <string>

#include "git/git_lfs.h"
#include "git/git_util.h"

namespace godot_git {

namespace {

// Not a String: no String may exist before the GDExtension interface does (CLAUDE.md gotcha 3).
std::mutex git_program_lock;
std::string git_program_name = "git";
std::atomic<int> git_state{ -1 }; // -1 unknown, 0 missing, 1 installed.

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

String git_program() {
	std::lock_guard<std::mutex> guard(git_program_lock);
	return String::utf8(git_program_name.c_str());
}

void set_git_program(const String &p_program) {
	{
		std::lock_guard<std::mutex> guard(git_program_lock);
		git_program_name = p_program.utf8().get_data();
	}
	check_git();
}

bool git_installed() {
	return git_state < 0 ? check_git() : git_state == 1;
}

namespace {

// Whether p_program can be found the way starting it would: on the PATH, or as a path. Running
// a program that isn't there makes Godot print an error to the editor's Output, every time.
bool on_path(const String &p_program) {
	OS *os = OS::get_singleton();
	const bool windows = os->get_name() == "Windows";
	PackedStringArray names;
	names.push_back(p_program);
	if (windows && p_program.get_extension().is_empty()) {
		for (const String &extension : os->get_environment("PATHEXT").split(";", false)) {
			names.push_back(p_program + extension.to_lower());
		}
	}
	PackedStringArray dirs;
	if (p_program.contains("/") || p_program.contains("\\")) {
		dirs.push_back(String());
	} else {
		dirs = os->get_environment("PATH").split(windows ? ";" : ":", false);
	}
	for (const String &dir : dirs) {
		for (const String &name : names) {
			if (FileAccess::file_exists(dir.is_empty() ? name : dir.trim_suffix("\\").path_join(name))) {
				return true;
			}
		}
	}
	return false;
}

} // namespace

bool check_git() {
	if (!on_path(git_program())) {
		git_state = 0;
		forget_lfs_check();
		return false;
	}
	PackedStringArray args;
	args.push_back("--version");
	Array output;
	const int code = OS::get_singleton()->execute(git_program(), args, output);
	git_state = (code == 0 && !output.is_empty() && String(output[0]).begins_with("git version")) ? 1 : 0;
	forget_lfs_check();
	return git_state == 1;
}

Error require_git(const String &p_what) {
	// Checked again: it may have been installed since.
	if (git_installed() || check_git()) {
		return OK;
	}
	return fail(vformat("%s Git isn't installed (or isn't on the PATH). Install it from git-scm.com, then try again.", p_what));
}

bool has_hook(git_repository *p_repo, const char *p_name) {
	const String dir = hooks_dir(p_repo);
	return !dir.is_empty() && FileAccess::file_exists(dir.path_join(p_name));
}

String commit_git_reason(git_repository *p_repo, CommitKind p_kind) {
	if (signs_commits(p_repo)) {
		return "this repository signs its commits (commit.gpgsign)";
	}
	for (const char *hook : { "pre-commit", "prepare-commit-msg", "commit-msg", "post-commit" }) {
		if (has_hook(p_repo, hook)) {
			return vformat("this repository has a %s hook", hook);
		}
	}
	if (p_kind == COMMIT_AMEND && has_hook(p_repo, "post-rewrite")) {
		return "this repository has a post-rewrite hook";
	}
	return String();
}

bool commit_needs_git(git_repository *p_repo, CommitKind p_kind) {
	return !commit_git_reason(p_repo, p_kind).is_empty();
}

Error run_git_command(git_repository *p_repo, RemoteContext &p_ctx, const PackedStringArray &p_args, const String &p_step, String &r_output, int &r_exit_code) {
	// Callers say what needed git; this is the backstop.
	const Error missing = require_git("This needs git.");
	if (missing != OK) {
		return missing;
	}
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
		String command = vformat("%s -C \"%s\"", git_program(), workdir);
		for (const String &arg : p_args) {
			command += vformat(" \"%s\"", arg);
		}
		program = "cmd";
		args.push_back("/c");
		args.push_back(command + " 2>&1");
	} else {
		program = "sh";
		args.push_back("-c");
		args.push_back(vformat("exec %s \"$@\" 2>&1", git_program()));
		args.push_back("sh");
		args.push_back("-C");
		args.push_back(workdir);
		args.append_array(p_args);
	}

	report_progress(&p_ctx, p_step, String(), -1);
	OS *os = OS::get_singleton();
	// Non-blocking: see the loop below.
	Dictionary process = os->execute_with_pipe(program, args, false);
	Ref<FileAccess> io = process.get("stdio", Variant());
	if (io.is_null()) {
		return fail("Couldn't start git.");
	}
	const int64_t pid = process.get("pid", -1);
	track_process(pid);

	// Progress lines end in \r, everything else in \n.
	std::string bytes;
	const auto take_line = [&]() {
		const String line = String::utf8(bytes.data(), bytes.size()).strip_edges();
		bytes.clear();
		if (line.is_empty()) {
			return;
		}
		r_output += line + String("\n");
		const double fraction = progress_fraction(line);
		const int colon = line.find(":");
		if (fraction >= 0 && colon > 0) {
			report_progress(&p_ctx, line.substr(0, colon + 1), line.substr(colon + 1).strip_edges(), fraction);
		} else {
			report_progress(&p_ctx, p_step, line, -1);
		}
	};
	const auto read_available = [&]() {
		uint8_t buffer[4096];
		uint64_t total = 0;
		uint64_t got;
		while ((got = io->get_buffer(buffer, sizeof(buffer))) > 0 && got <= sizeof(buffer)) {
			total += got;
			for (uint64_t i = 0; i < got; i++) {
				if (buffer[i] == '\r' || buffer[i] == '\n') {
					take_line();
				} else {
					bytes += (char)buffer[i];
				}
			}
		}
		return total;
	};
	// Done when git exits (or on Cancel), not when the pipe closes: a process git started can
	// outlive it and keep the pipe open. On Windows an ssh (or hook) started through git's shell
	// isn't even in git's process tree, so Cancel can't end it; it's left to time out.
	while (true) {
		if (read_available() > 0) {
			continue;
		}
		if (is_cancel_requested()) {
			break;
		}
		if (!os->is_process_running(pid)) {
			read_available();
			break;
		}
		os->delay_msec(10);
	}
	take_line();
	track_process(0);
	r_exit_code = is_cancel_requested() ? -1 : wait_for_exit_code(pid);

	if (is_cancel_requested()) {
		git_error_set_str(GIT_ERROR_NET, "Canceled.");
		return ERR_SKIP;
	}
	return OK;
}

bool is_ssh_url(const String &p_url) {
	if (p_url.begins_with("ssh://") || p_url.begins_with("git+ssh://") || p_url.begins_with("ssh+git://")) {
		return true;
	}
	if (p_url.contains("://")) {
		return false;
	}
	// scp-like "[user@]host:path". Not "C:/path" (a Windows drive) or a plain local path.
	const int colon = p_url.find(":");
	const int slash = p_url.find("/");
	return colon > 1 && (slash < 0 || colon < slash);
}

PackedStringArray ssh_args(git_repository *p_repo) {
	PackedStringArray args;
	OS *os = OS::get_singleton();
	if (!os->get_environment("GIT_SSH_COMMAND").is_empty() || !os->get_environment("GIT_SSH").is_empty()) {
		return args;
	}
	ConfigPtr config;
	git_buf command = GIT_BUF_INIT;
	const bool configured = git_repository_config_snapshot(config.out(), p_repo) == 0 && git_config_get_string_buf(&command, config, "core.sshCommand") == 0;
	git_buf_dispose(&command);
	if (!configured) {
		// Plain "ssh" is git's own on Windows too (it runs this through its shell). ConnectTimeout:
		// after a Cancel, an ssh stuck connecting may be out of reach (see run_git_command).
		args.push_back("-c");
		args.push_back("core.sshCommand=ssh -o BatchMode=yes -o ConnectTimeout=15");
	}
	return args;
}

Error network_failure(const String &p_output, const String &p_action, bool p_ssh) {
	const String tail = output_tail(p_output, 6);
	if (p_ssh) {
		if (p_output.contains("Host key verification failed")) {
			return fail(vformat("SSH doesn't know this server yet, so it didn't connect. Connect to it once from a terminal (e.g. `ssh -T git@github.com`) and confirm its fingerprint, then try again.\n%s", tail));
		}
		if (p_output.contains("Permission denied") || p_output.contains("passphrase")) {
			return fail(vformat("SSH couldn't log in. Check that `ssh -T` to this server works in a terminal; a key with a passphrase must be added to ssh-agent, since the panel can't ask for it.\n%s", tail));
		}
		if (p_output.contains("Could not resolve hostname") || p_output.contains("Connection refused") || p_output.contains("timed out") || p_output.contains("Network is unreachable")) {
			return fail(vformat("Couldn't reach the server over SSH. Check your connection and the remote's address.\n%s", tail));
		}
	}
	return fail(vformat("Git couldn't %s:\n%s", p_action, tail.is_empty() ? String("(no details)") : tail));
}

String output_tail(const String &p_output, int p_count) {
	const PackedStringArray lines = p_output.split("\n", false);
	return String("\n").join(lines.slice(MAX(0, lines.size() - p_count)));
}

Error commit_with_git(git_repository *p_repo, RemoteContext &p_ctx, const String &p_message, CommitKind p_kind) {
	const Error missing = require_git(vformat("Committing needs git here: %s.", commit_git_reason(p_repo, p_kind)));
	if (missing != OK) {
		return missing;
	}
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
