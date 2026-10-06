#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

using namespace godot;

namespace godot_git {
struct WatcherState;
}

// Watches a repository's working folder for changes made outside the panel (a terminal commit, a
// save in another editor) so the dock can refresh while Godot has focus, without polling. Uses the
// system's file notifications: ReadDirectoryChangesW (Windows), inotify (Linux), FSEvents (macOS).
//
// Changes the dock doesn't show are left out: Godot's .godot folder, files git ignores, and git's
// own internals (objects, logs, lock files); HEAD, the index, refs and an operation's state files
// count, so git commands from a terminal do. Changes come in bursts (a checkout writes hundreds of
// files), so they're reported once things have been quiet for a moment: the callback gets
// (paths: PackedStringArray, first_change_msec: int), on the main thread. The paths are relative to
// the working folder, at most a few hundred (more is one empty path: "everything").
//
// Exposed to GDScript only for the test suite, like GitRepository.
class GitWatcher : public RefCounted {
	GDCLASS(GitWatcher, RefCounted)

	godot_git::WatcherState *state = nullptr;

protected:
	static void _bind_methods();

public:
	Error start(const String &p_workdir, const Callable &p_callback);
	void stop();
	bool is_watching() const;

	~GitWatcher();
};
