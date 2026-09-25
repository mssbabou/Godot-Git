#include "git/git_lfs.h"

#include <git2/sys/errors.h>
#include <git2/sys/filter.h>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "git/git_cli.h"
#include "git/git_util.h"

namespace godot_git {

namespace {

std::atomic<int> lfs_state{ -1 }; // -1 unknown, 0 missing, 1 installed.

bool is_windows() {
	return OS::get_singleton()->get_name() == "Windows";
}

String workdir_of(git_repository *p_repo) {
	const char *workdir = git_repository_workdir(p_repo);
	return workdir ? String::utf8(workdir) : String();
}

// Last non-empty line of p_text, for error messages.
String last_line(const String &p_text) {
	const PackedStringArray lines = p_text.replace("\r", "\n").split("\n", false);
	return lines.is_empty() ? String() : lines[lines.size() - 1].strip_edges();
}

// Reads exactly p_length bytes. Godot's pipe reads return whatever is available, so it loops.
bool read_exact(FileAccess *p_io, uint8_t *p_dst, uint64_t p_length) {
	uint64_t done = 0;
	while (done < p_length) {
		const uint64_t got = p_io->get_buffer(p_dst + done, p_length - done);
		if (got == 0 || got > p_length - done) {
			return false;
		}
		done += got;
	}
	return true;
}

// A running `git lfs filter-process`: git's protocol for a filter that stays running and handles
// one file after another (pkt-lines, see gitattributes' "Long Running Filter Process"). One per
// repository, used only from that repository's thread. Its stderr goes to a log file: an unread
// stderr pipe could fill up and block it, and the log explains failures.
class FilterProcess {
	Ref<FileAccess> io;
	String log_path;

	void write_packet(const uint8_t *p_data, size_t p_length) {
		const std::string header = String::num_int64((int64_t)p_length + 4, 16).lpad(4, "0").utf8().get_data();
		io->store_buffer((const uint8_t *)header.data(), 4);
		io->store_buffer(p_data, p_length);
	}

	void write_text(const String &p_line) {
		const CharString utf8 = (p_line + String("\n")).utf8();
		write_packet((const uint8_t *)utf8.get_data(), utf8.length());
	}

	void write_flush() {
		io->store_buffer((const uint8_t *)"0000", 4);
	}

	// One packet into r_data; false on a flush packet or a broken pipe (r_ok says which).
	bool read_packet(std::string &r_data, bool &r_ok) {
		char header[5] = {};
		r_ok = read_exact(io.ptr(), (uint8_t *)header, 4);
		if (!r_ok) {
			return false;
		}
		const int64_t length = String(header).hex_to_int();
		if (length == 0) {
			return false; // Flush.
		}
		if (length < 4) {
			r_ok = false;
			return false;
		}
		r_data.resize(length - 4);
		r_ok = read_exact(io.ptr(), (uint8_t *)r_data.data(), length - 4);
		return r_ok;
	}

	// Text packets up to the next flush, as "key=value" lines without the newline.
	bool read_list(PackedStringArray &r_lines) {
		std::string data;
		bool ok = true;
		while (read_packet(data, ok)) {
			r_lines.push_back(String::utf8(data.data(), data.size()).strip_edges(false, true));
		}
		return ok;
	}

public:
	bool broken = false;

	Error start(git_repository *p_repo) {
		const String workdir = workdir_of(p_repo);
		log_path = String::utf8(git_repository_path(p_repo)).path_join("godot-git-lfs.log");
		// A download from inside a checkout must never open a sign-in window: nobody is there
		// to see it. Files are fetched beforehand (lfs_fetch), with sign-in if allowed.
		const String git = vformat("%s -c credential.interactive=never lfs filter-process", git_program());
		PackedStringArray args;
		String program;
		if (is_windows()) {
			// cmd gets one string (Godot quotes each argument; see CLAUDE.md gotcha 24).
			program = "cmd";
			args.push_back("/c");
			args.push_back(vformat("cd /d \"%s\" && %s 2>\"%s\"", workdir.replace("/", "\\"), git, log_path.replace("/", "\\")));
		} else {
			program = "sh";
			args.push_back("-c");
			args.push_back(vformat("cd \"$1\" && exec %s 2>\"$2\"", git));
			args.push_back("sh");
			args.push_back(workdir);
			args.push_back(log_path);
		}
		Dictionary process = OS::get_singleton()->execute_with_pipe(program, args, true);
		io = process.get("stdio", Variant());
		if (io.is_null()) {
			return fail("Couldn't start git-lfs.");
		}

		write_text("git-filter-client");
		write_text("version=2");
		write_flush();
		PackedStringArray welcome;
		if (!read_list(welcome) || !welcome.has("git-filter-server") || !welcome.has("version=2")) {
			return fail(vformat("git-lfs didn't start: %s", log_tail()));
		}
		write_text("capability=clean");
		write_text("capability=smudge");
		write_flush();
		PackedStringArray capabilities;
		if (!read_list(capabilities)) {
			return fail(vformat("git-lfs didn't start: %s", log_tail()));
		}
		return OK;
	}

	String log_tail() const {
		const String tail = last_line(FileAccess::get_file_as_string(log_path));
		return tail.is_empty() ? String("it stopped unexpectedly.") : tail;
	}

	// Runs "clean" (file -> pointer for git) or "smudge" (pointer -> file) on one file.
	Error run(const String &p_command, const String &p_path, const std::string &p_input, std::string &r_output) {
		write_text("command=" + p_command);
		write_text("pathname=" + p_path);
		write_flush();
		for (size_t offset = 0; offset < p_input.size(); offset += 65516) {
			const size_t length = MIN((size_t)65516, p_input.size() - offset);
			write_packet((const uint8_t *)p_input.data() + offset, length);
		}
		write_flush();

		PackedStringArray status;
		if (!read_list(status)) {
			broken = true;
			return fail(vformat("git-lfs stopped while handling %s: %s", p_path, log_tail()));
		}
		if (!status.has("status=success")) {
			return fail(vformat("git-lfs couldn't handle %s: %s", p_path, log_tail()));
		}
		std::string data;
		bool ok = true;
		while (read_packet(data, ok)) {
			r_output += data;
		}
		PackedStringArray final_status; // Empty means "still success".
		if (!ok || !read_list(final_status)) {
			broken = true;
			return fail(vformat("git-lfs stopped while handling %s: %s", p_path, log_tail()));
		}
		if (!final_status.is_empty() && !final_status.has("status=success")) {
			return fail(vformat("git-lfs couldn't handle %s: %s", p_path, log_tail()));
		}
		return OK;
	}
};

// Filter processes by repository. Each is only used on its repository's thread; the lock only
// guards the map itself.
std::mutex processes_lock;
std::map<git_repository *, FilterProcess *> processes;

FilterProcess *process_for(git_repository *p_repo) {
	std::lock_guard<std::mutex> guard(processes_lock);
	auto found = processes.find(p_repo);
	if (found != processes.end()) {
		if (!found->second->broken) {
			return found->second;
		}
		delete found->second; // Closing its pipes ends it.
		processes.erase(found);
	}
	FilterProcess *process = new FilterProcess();
	if (process->start(p_repo) != OK) {
		delete process;
		return nullptr;
	}
	processes[p_repo] = process;
	return process;
}

// libgit2 streams a file's contents through a filter. git-lfs needs the whole file per request,
// so this collects it and runs the request on close.
struct LfsStream {
	git_writestream base; // First, so a git_writestream * is an LfsStream *.
	git_writestream *next = nullptr;
	git_repository *repo = nullptr;
	String path;
	bool smudge = false;
	std::string data;
};

int stream_write(git_writestream *p_stream, const char *p_buffer, size_t p_length) {
	((LfsStream *)p_stream)->data.append(p_buffer, p_length);
	return 0;
}

int stream_close(git_writestream *p_stream) {
	LfsStream *stream = (LfsStream *)p_stream;
	FilterProcess *process = process_for(stream->repo);
	if (!process) {
		return -1; // start() set the error.
	}
	std::string output;
	if (process->run(stream->smudge ? "smudge" : "clean", stream->path, stream->data, output) != OK) {
		return -1;
	}
	const int err = stream->next->write(stream->next, output.data(), output.size());
	return err < 0 ? err : stream->next->close(stream->next);
}

void stream_free(git_writestream *p_stream) {
	delete (LfsStream *)p_stream;
}

int filter_check(git_filter *p_self, void **p_payload, const git_filter_source *p_src, const char **p_attr_values) {
	// Without git-lfs, pass files through unchanged, as libgit2 would anyway. The operations that
	// would then do the wrong thing (checkout, commit, push) refuse up front; see require_lfs().
	if (!git_filter_source_repo(p_src) || !lfs_installed()) {
		return GIT_PASSTHROUGH;
	}
	return 0;
}

int filter_stream(git_writestream **r_out, git_filter *p_self, void **p_payload, const git_filter_source *p_src, git_writestream *p_next) {
	LfsStream *stream = new LfsStream();
	stream->base.write = stream_write;
	stream->base.close = stream_close;
	stream->base.free = stream_free;
	stream->next = p_next;
	stream->repo = git_filter_source_repo(p_src);
	stream->path = String::utf8(git_filter_source_path(p_src));
	stream->smudge = git_filter_source_mode(p_src) == GIT_FILTER_TO_WORKTREE;
	*r_out = &stream->base;
	return 0;
}

git_filter lfs_filter;

// Runs `git lfs <p_command> <p_args>` in the repository, showing its progress lines ("Downloading
// LFS objects: 45% (9/20), 12 MB") in the status strip. Cancellable.
Error run_lfs_command(git_repository *p_repo, RemoteContext &p_ctx, const String &p_command, const PackedStringArray &p_args, const String &p_step) {
	// git-lfs only prints progress for a terminal unless told to.
	OS::get_singleton()->set_environment("GIT_LFS_FORCE_PROGRESS", "1");
	PackedStringArray args;
	args.push_back("-c");
	args.push_back(vformat("credential.interactive=%s", p_ctx.login_prompts_allowed ? "always" : "never"));
	args.push_back("lfs");
	args.push_back(p_command);
	args.append_array(p_args);
	String output;
	int exit_code = 0;
	const Error err = run_git_command(p_repo, p_ctx, args, p_step, output, exit_code);
	if (err == ERR_SKIP) {
		git_error_set_str(GIT_ERROR_NET, "Canceled. Nothing was changed.");
	}
	if (err != OK) {
		return err;
	}
	if (exit_code != 0) {
		const String reason = last_line(output);
		return fail(vformat("Git LFS couldn't %s. %s", p_command == "push" ? String("upload files") : String("download files"), reason.is_empty() ? String("It stopped without saying why.") : reason));
	}
	return OK;
}

} // namespace

void register_lfs_filter() {
	git_filter_init(&lfs_filter, GIT_FILTER_VERSION);
	lfs_filter.attributes = "filter=lfs";
	lfs_filter.check = filter_check;
	lfs_filter.stream = filter_stream;
	git_filter_register("lfs", &lfs_filter, GIT_FILTER_DRIVER_PRIORITY);
}

void shutdown_lfs() {
	std::lock_guard<std::mutex> guard(processes_lock);
	for (auto &entry : processes) {
		delete entry.second;
	}
	processes.clear();
}

void release_lfs(git_repository *p_repo) {
	std::lock_guard<std::mutex> guard(processes_lock);
	auto found = processes.find(p_repo);
	if (found != processes.end()) {
		delete found->second;
		processes.erase(found);
	}
}

void forget_lfs_check() {
	lfs_state = -1;
}

bool lfs_installed() {
	if (lfs_state < 0) {
		if (!git_installed()) {
			lfs_state = 0;
			return false;
		}
		PackedStringArray args;
		args.push_back("lfs");
		args.push_back("version");
		Array output;
		const int code = OS::get_singleton()->execute(git_program(), args, output);
		lfs_state = (code == 0 && !output.is_empty() && String(output[0]).begins_with("git-lfs/")) ? 1 : 0;
	}
	return lfs_state == 1;
}

bool repo_uses_lfs(git_repository *p_repo) {
	// The usual place, where `git lfs track` writes. Nested .gitattributes files aren't checked.
	const String workdir = workdir_of(p_repo);
	return !workdir.is_empty() && FileAccess::get_file_as_string(workdir.path_join(".gitattributes")).contains("filter=lfs");
}

Error require_lfs(git_repository *p_repo, const String &p_action) {
	if (repo_uses_lfs(p_repo) && !lfs_installed()) {
		if (!git_installed()) {
			return require_git(vformat("This project stores some files with Git LFS, which needs git and git-lfs, so %s would damage them.", p_action));
		}
		return fail(vformat("This project stores some files with Git LFS, which isn't installed, so %s would damage them. Install Git LFS (git-lfs.com), then restart the editor.", p_action));
	}
	return OK;
}

Error lfs_fetch(git_repository *p_repo, RemoteContext &p_ctx, const String &p_remote, const String &p_commit) {
	if (!repo_uses_lfs(p_repo)) {
		return OK;
	}
	const Error err = require_lfs(p_repo, "updating files");
	if (err != OK) {
		return err;
	}
	PackedStringArray args;
	args.push_back(p_remote);
	args.push_back(p_commit);
	return run_lfs_command(p_repo, p_ctx, "fetch", args, "Downloading LFS files...");
}

Error lfs_push(git_repository *p_repo, RemoteContext &p_ctx, const String &p_remote, const String &p_ref) {
	if (!repo_uses_lfs(p_repo)) {
		return OK;
	}
	const Error err = require_lfs(p_repo, "pushing");
	if (err != OK) {
		return err;
	}
	PackedStringArray args;
	args.push_back(p_remote);
	args.push_back(p_ref);
	return run_lfs_command(p_repo, p_ctx, "push", args, "Uploading LFS files...");
}

bool is_lfs_path(git_repository *p_repo, const char *p_path) {
	const char *value = nullptr;
	return git_attr_get(&value, p_repo, 0, p_path, "filter") == 0 && value && strcmp(value, "lfs") == 0;
}

} // namespace godot_git
