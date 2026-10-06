#include "git/git_watcher.h"

#include <git2.h>

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <atomic>
#include <mutex>
#include <thread>

#include "git/git_util.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <limits.h>
#include <stdlib.h>
#else
#include <dirent.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace godot_git {

namespace {

constexpr uint64_t QUIET_MSEC = 250; // Report once nothing has changed for this long.
constexpr int MAX_PATHS = 300; // More is reported as "everything".

uint64_t now_msec() {
	return Time::get_singleton()->get_ticks_msec();
}

// git's own files that say something the dock shows: what's checked out, staged, the branches, an
// operation in progress (and the panel's own operation files). Not objects, logs or lock files.
bool is_relevant_git_file(const String &p_rest) {
	if (p_rest.ends_with(".lock")) {
		return false;
	}
	static const char *files[] = { "HEAD", "index", "packed-refs", "FETCH_HEAD", "MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "REBASE_HEAD", "BISECT_LOG" };
	for (const char *file : files) {
		if (p_rest == file) {
			return true;
		}
	}
	return p_rest.begins_with("refs/") || p_rest.begins_with("rebase-merge") || p_rest.begins_with("rebase-apply") || p_rest.begins_with("godot-git-");
}

} // namespace

// Everything one watcher's thread shares with the main thread.
struct WatcherState {
	String workdir; // Ends in "/".
	Callable callback;
	std::atomic<bool> stopping{ false };
	std::thread thread;
	git_repository *repo = nullptr; // Only touched by the watching thread (or FSEvents' queue).

	std::mutex mutex;
	HashSet<String> pending;
	bool everything = false;
	uint64_t first_msec = 0;
	uint64_t last_msec = 0;

#if defined(_WIN32)
	HANDLE stop_event = nullptr;
#elif defined(__APPLE__)
	FSEventStreamRef stream = nullptr;
	dispatch_queue_t queue = nullptr;
#else
	int stop_pipe[2] = { -1, -1 };
#endif

	// Whether a change to p_relative (a path relative to the working folder) is one the dock shows.
	bool relevant(const String &p_relative) {
		const String path = p_relative.replace("\\", "/");
		if (path.is_empty() || path == ".git" || path == ".godot" || path.begins_with(".godot/") || path.contains("/.godot/") || path.ends_with("/.godot")) {
			return false;
		}
		if (path.begins_with(".git/")) {
			return is_relevant_git_file(path.substr(5));
		}
		if (!repo && git_repository_open(&repo, workdir.utf8().get_data()) < 0) {
			repo = nullptr;
			return true;
		}
		int ignored = 0;
		return git_status_should_ignore(&ignored, repo, path.utf8().get_data()) < 0 || !ignored;
	}

	void add(const String &p_relative) {
		if (!relevant(p_relative)) {
			return;
		}
		std::lock_guard<std::mutex> lock(mutex);
		const uint64_t now = now_msec();
		if (pending.is_empty() && !everything) {
			first_msec = now;
		}
		last_msec = now;
		if (pending.size() < MAX_PATHS) {
			pending.insert(p_relative.replace("\\", "/"));
		} else {
			everything = true;
		}
	}

	void add_everything() {
		std::lock_guard<std::mutex> lock(mutex);
		const uint64_t now = now_msec();
		if (pending.is_empty() && !everything) {
			first_msec = now;
		}
		last_msec = now;
		everything = true;
	}

	// How long until what's pending can be reported (-1: nothing pending).
	int64_t quiet_wait() {
		std::lock_guard<std::mutex> lock(mutex);
		if (pending.is_empty() && !everything) {
			return -1;
		}
		const uint64_t quiet = now_msec() - last_msec;
		return quiet >= QUIET_MSEC ? 0 : (int64_t)(QUIET_MSEC - quiet);
	}

	// Hands what's pending to the main thread (once it's been quiet long enough, unless p_now).
	void deliver(bool p_now) {
		PackedStringArray paths;
		uint64_t first = 0;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if ((pending.is_empty() && !everything) || (!p_now && now_msec() - last_msec < QUIET_MSEC)) {
				return;
			}
			if (everything) {
				paths.push_back(String());
			} else {
				for (const String &path : pending) {
					paths.push_back(path);
				}
			}
			first = first_msec;
			pending.clear();
			everything = false;
		}
		callback.call_deferred(paths, (int64_t)first);
	}
};

namespace {

#if defined(_WIN32)

void watch(WatcherState *p_state) {
	const Char16String path = p_state->workdir.replace("/", "\\").utf16();
	HANDLE dir = CreateFileW((LPCWSTR)path.get_data(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
	if (dir == INVALID_HANDLE_VALUE) {
		return;
	}
	OVERLAPPED overlapped = {};
	overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	alignas(DWORD) static thread_local char buffer[64 * 1024];
	const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
	auto read = [&]() {
		ResetEvent(overlapped.hEvent);
		return ReadDirectoryChangesW(dir, buffer, sizeof(buffer), TRUE, filter, nullptr, &overlapped, nullptr);
	};
	if (!read()) {
		CloseHandle(overlapped.hEvent);
		CloseHandle(dir);
		return;
	}
	HANDLE handles[2] = { overlapped.hEvent, p_state->stop_event };
	while (!p_state->stopping) {
		const int64_t wait = p_state->quiet_wait();
		const DWORD result = WaitForMultipleObjects(2, handles, FALSE, wait < 0 ? INFINITE : (DWORD)wait);
		if (result == WAIT_OBJECT_0) {
			DWORD bytes = 0;
			if (GetOverlappedResult(dir, &overlapped, &bytes, FALSE)) {
				if (bytes == 0) {
					p_state->add_everything(); // More changed than the buffer holds.
				} else {
					for (const char *at = buffer;;) {
						const FILE_NOTIFY_INFORMATION *info = (const FILE_NOTIFY_INFORMATION *)at;
						p_state->add(String::utf16((const char16_t *)info->FileName, (int)(info->FileNameLength / sizeof(WCHAR))));
						if (info->NextEntryOffset == 0) {
							break;
						}
						at += info->NextEntryOffset;
					}
				}
			}
			if (!read()) {
				break;
			}
		} else if (result != WAIT_TIMEOUT) {
			break; // Stopping.
		}
		p_state->deliver(false);
	}
	CancelIoEx(dir, &overlapped);
	DWORD bytes = 0;
	GetOverlappedResult(dir, &overlapped, &bytes, TRUE);
	CloseHandle(overlapped.hEvent);
	CloseHandle(dir);
}

#elif defined(__APPLE__)

void on_fs_events(ConstFSEventStreamRef, void *p_info, size_t p_count, void *p_paths, const FSEventStreamEventFlags p_flags[], const FSEventStreamEventId[]) {
	WatcherState *state = (WatcherState *)p_info;
	if (state->stopping) {
		return;
	}
	char **paths = (char **)p_paths;
	for (size_t i = 0; i < p_count; i++) {
		if (p_flags[i] & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagKernelDropped | kFSEventStreamEventFlagUserDropped)) {
			state->add_everything();
			continue;
		}
		const String path = String::utf8(paths[i]);
		if (path.begins_with(state->workdir)) {
			state->add(path.substr(state->workdir.length()));
		}
	}
	// FSEvents already waits for things to settle (the stream's latency).
	state->deliver(true);
}

void drain(void *) {}

#else

void add_watches(WatcherState *p_state, int p_fd, HashMap<int, String> &r_dirs, const String &p_relative) {
	const String name = p_relative.get_file();
	if (name == ".godot" || p_relative.begins_with(".git/objects") || p_relative.begins_with(".git/logs") || p_relative.begins_with(".git/lfs") || p_relative.begins_with(".git/modules")) {
		return;
	}
	const bool in_git = p_relative == ".git" || p_relative.begins_with(".git/");
	if (!p_relative.is_empty() && !in_git && !p_state->relevant(p_relative + String("/"))) {
		return; // A folder git ignores, and everything in it.
	}
	const String absolute = p_state->workdir + p_relative;
	const int wd = inotify_add_watch(p_fd, absolute.utf8().get_data(), IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_ONLYDIR);
	if (wd < 0) {
		return; // Out of watches (fs.inotify.max_user_watches), or gone already.
	}
	r_dirs[wd] = p_relative;
	DIR *dir = opendir(absolute.utf8().get_data());
	if (!dir) {
		return;
	}
	while (dirent *entry = readdir(dir)) {
		const String child = String::utf8(entry->d_name);
		if (child == "." || child == ".." || entry->d_type != DT_DIR) {
			continue;
		}
		if (p_relative == ".git" && child != "refs") {
			continue; // Of git's folders only refs/ matters (and rebase-*, which come and go as files change).
		}
		add_watches(p_state, p_fd, r_dirs, p_relative.is_empty() ? child : vformat("%s/%s", p_relative, child));
	}
	closedir(dir);
}

void watch(WatcherState *p_state) {
	const int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0) {
		return;
	}
	HashMap<int, String> dirs;
	add_watches(p_state, fd, dirs, String());
	alignas(inotify_event) char buffer[64 * 1024];
	while (!p_state->stopping) {
		const int64_t wait = p_state->quiet_wait();
		pollfd fds[2] = { { fd, POLLIN, 0 }, { p_state->stop_pipe[0], POLLIN, 0 } };
		const int ready = poll(fds, 2, wait < 0 ? -1 : (int)wait);
		if (ready < 0 || (fds[1].revents & POLLIN)) {
			break;
		}
		if (fds[0].revents & POLLIN) {
			ssize_t length;
			while ((length = read(fd, buffer, sizeof(buffer))) > 0) {
				for (char *at = buffer; at < buffer + length;) {
					const inotify_event *event = (const inotify_event *)at;
					at += sizeof(inotify_event) + event->len;
					if (event->mask & IN_Q_OVERFLOW) {
						p_state->add_everything();
						continue;
					}
					if (event->mask & IN_IGNORED) {
						dirs.erase(event->wd);
						continue;
					}
					const String *parent = dirs.getptr(event->wd);
					if (!parent) {
						continue;
					}
					const String name = event->len > 0 ? String::utf8(event->name) : String();
					const String relative = parent->is_empty() ? name : (name.is_empty() ? *parent : vformat("%s/%s", *parent, name));
					if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO))) {
						add_watches(p_state, fd, dirs, relative); // A new folder: watch it too.
					}
					p_state->add(relative);
				}
			}
		}
		p_state->deliver(false);
	}
	close(fd);
}

#endif

} // namespace

} // namespace godot_git

using namespace godot_git;

void GitWatcher::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "workdir", "callback"), &GitWatcher::start);
	ClassDB::bind_method(D_METHOD("stop"), &GitWatcher::stop);
	ClassDB::bind_method(D_METHOD("is_watching"), &GitWatcher::is_watching);
}

// Starts watching p_workdir (a repository's working folder), calling p_callback(paths,
// first_change_msec) on the main thread after changes. Stops a watch already running first.
Error GitWatcher::start(const String &p_workdir, const Callable &p_callback) {
	stop();
	state = new WatcherState();
	state->workdir = p_workdir.replace("\\", "/").trim_suffix("/") + String("/");
	state->callback = p_callback;
#if defined(_WIN32)
	state->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	state->thread = std::thread(watch, state);
#elif defined(__APPLE__)
	// FSEvents reports real paths ("/private/var/..." for "/var/..."), so compare against that.
	char real[PATH_MAX];
	if (realpath(state->workdir.utf8().get_data(), real)) {
		state->workdir = String::utf8(real).trim_suffix("/") + String("/");
	}
	CFStringRef path = CFStringCreateWithCString(nullptr, state->workdir.utf8().get_data(), kCFStringEncodingUTF8);
	CFArrayRef paths = CFArrayCreate(nullptr, (const void **)&path, 1, &kCFTypeArrayCallBacks);
	FSEventStreamContext context = { 0, state, nullptr, nullptr, nullptr };
	state->stream = FSEventStreamCreate(nullptr, &on_fs_events, &context, paths, kFSEventStreamEventIdSinceNow, QUIET_MSEC / 1000.0, kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
	CFRelease(paths);
	CFRelease(path);
	if (!state->stream) {
		delete state;
		state = nullptr;
		return FAILED;
	}
	state->queue = dispatch_queue_create("godot_git.watcher", DISPATCH_QUEUE_SERIAL);
	FSEventStreamSetDispatchQueue(state->stream, state->queue);
	FSEventStreamStart(state->stream);
#else
	if (pipe(state->stop_pipe) != 0) {
		delete state;
		state = nullptr;
		return FAILED;
	}
	state->thread = std::thread(watch, state);
#endif
	return OK;
}

void GitWatcher::stop() {
	if (!state) {
		return;
	}
	state->stopping = true;
#if defined(_WIN32)
	SetEvent(state->stop_event);
	if (state->thread.joinable()) {
		state->thread.join();
	}
	CloseHandle(state->stop_event);
#elif defined(__APPLE__)
	FSEventStreamStop(state->stream);
	FSEventStreamInvalidate(state->stream);
	FSEventStreamRelease(state->stream);
	dispatch_sync_f(state->queue, nullptr, drain); // Any callback still running finishes first.
	dispatch_release(state->queue);
#else
	const char byte = 0;
	(void)!write(state->stop_pipe[1], &byte, 1);
	if (state->thread.joinable()) {
		state->thread.join();
	}
	close(state->stop_pipe[0]);
	close(state->stop_pipe[1]);
#endif
	if (state->repo) {
		git_repository_free(state->repo);
	}
	delete state;
	state = nullptr;
}

bool GitWatcher::is_watching() const {
	return state != nullptr;
}

GitWatcher::~GitWatcher() {
	stop();
}
