#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/thread.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <atomic>
#include <mutex>
#include <vector>

using namespace godot;

// Authors' GitHub profile pictures for History. A commit only names an email, so each email is
// tied to its GitHub account once: GitHub's no-reply addresses name the account themselves, any
// other email is asked about through GitHub's API (one of that author's pushed commits; with git's
// saved login for GitHub, which a private repository needs). The account never changes, so that's
// remembered; the picture is checked once a day with an ETag, which costs a "not modified" when
// it's the same. Kept in the editor's cache folder, for every project. Up to MAX_WORKERS lookups
// run at once; History draws initials until (and unless) a picture arrives.
class GitAvatars : public Node {
	GDCLASS(GitAvatars, Node)

	struct Request {
		String email;
		String hash; // A pushed commit of theirs, for the API.
		String repository; // "owner/repo" on github.com, or "".
		String picture; // Their picture's address, once known.
		String etag; // Of the picture we have ("" to download it whatever it is).
	};

	String workdir;
	String repository;
	String cache_dir;
	bool enabled = true;
	Dictionary textures; // email -> Texture2D, round already.
	// email -> { "time": int (unix, last checked), "file": String ("" when they have no picture),
	//            "picture": String (address), "etag": String }
	Dictionary index;
	bool index_loaded = false;
	Dictionary asked; // email -> true while it's being looked up.
	Dictionary retry_after; // email -> unix time: the lookup failed (offline?), not asked again before.

	std::mutex mutex; // pending, running.
	std::vector<Request> pending;
	int running = 0;
	std::vector<Ref<Thread>> threads;
	std::atomic<bool> stopping{ false };
	std::atomic<int64_t> paused_until{ 0 }; // Unix time; GitHub's rate limit was reached.

	std::mutex login_mutex; // The workers share one login.
	bool login_read = false;
	String token;

	void _load_index();
	void _save_index();
	Ref<Texture2D> _load_texture(const String &p_file);
	void _start_workers();
	void _worker(const String &p_workdir);
	Dictionary _look_up(const Request &p_request, const String &p_workdir);
	String _token(const String &p_workdir);
	void _lookup_done(const Dictionary &p_result);
	void _stop_workers();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	// From the repository's remote URL; emails are only asked about for github.com.
	void set_repository(const String &p_workdir, const String &p_remote_url);
	void set_enabled(bool p_enabled);
	// The author's picture, or null (draw initials). Asking starts the lookup when needed;
	// "avatars_changed" is emitted when pictures arrive.
	Ref<Texture2D> get_avatar(const Dictionary &p_commit);
};
