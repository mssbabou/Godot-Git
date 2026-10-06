#include "editor/avatars.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_paths.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/http_client.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/tls_options.hpp>
#include <godot_cpp/classes/x509_certificate.hpp>
#include <godot_cpp/core/math.hpp>

#include "editor/ui_text.h"
#include "git/git_repository.h"

using namespace godot_git;

namespace {

constexpr int PICTURE_SIZE = 96; // Pixels kept; drawn at 24 px times the editor scale.
constexpr int MAX_WORKERS = 4; // Lookups at once; a big team fills in about four times faster.
constexpr int64_t CHECK_AFTER = 24 * 3600; // Pictures (and emails without an account) checked daily.
constexpr int64_t RETRY_AFTER = 10 * 60; // Offline, or GitHub didn't answer.
constexpr uint64_t TIMEOUT_MSEC = 10000;

int64_t now() {
	return (int64_t)Time::get_singleton()->get_unix_time_from_system();
}

// The picture's address when the email itself names the GitHub account: the no-reply addresses
// GitHub gives out, "12345+name@users.noreply.github.com" (or "name@..." from before 2017).
String noreply_picture(const String &p_email) {
	const String suffix = "@users.noreply.github.com";
	if (!p_email.ends_with(suffix)) {
		return String();
	}
	const String local = p_email.substr(0, p_email.length() - suffix.length());
	const String id = local.get_slice("+", 0);
	if (local.contains("+") && id.is_valid_int()) {
		return vformat("https://avatars.githubusercontent.com/u/%s?s=%d", id, PICTURE_SIZE);
	}
	return vformat("https://github.com/%s.png?size=%d", local.uri_encode(), PICTURE_SIZE);
}

struct Response {
	int code = 0; // 0: no answer.
	PackedByteArray body;
	Dictionary headers; // Lowercase names.
};

// A blocking HTTPS GET, for the worker thread. Gives up after TIMEOUT_MSEC or when p_stop is set.
Response http_get(const String &p_url, const PackedStringArray &p_headers, const std::atomic<bool> &p_stop) {
	Response response;
	const String rest = p_url.trim_prefix("https://");
	const int slash = rest.find("/");
	const String host = slash < 0 ? rest : rest.substr(0, slash);
	const String path = slash < 0 ? String("/") : rest.substr(slash);

	Ref<HTTPClient> client;
	client.instantiate();
	if (client->connect_to_host("https://" + host, 443, TLSOptions::client()) != OK) {
		return response;
	}
	const uint64_t start = Time::get_singleton()->get_ticks_msec();
	auto timed_out = [&]() {
		return p_stop || Time::get_singleton()->get_ticks_msec() - start > TIMEOUT_MSEC;
	};
	while (client->get_status() == HTTPClient::STATUS_CONNECTING || client->get_status() == HTTPClient::STATUS_RESOLVING) {
		client->poll();
		if (timed_out()) {
			return response;
		}
		OS::get_singleton()->delay_msec(20);
	}
	if (client->get_status() != HTTPClient::STATUS_CONNECTED || client->request(HTTPClient::METHOD_GET, path, p_headers) != OK) {
		return response;
	}
	while (client->get_status() == HTTPClient::STATUS_REQUESTING) {
		client->poll();
		if (timed_out()) {
			return response;
		}
		OS::get_singleton()->delay_msec(20);
	}
	if (!client->has_response()) {
		return response;
	}
	const Dictionary headers = client->get_response_headers_as_dictionary();
	const Array names = headers.keys();
	for (int i = 0; i < names.size(); i++) {
		response.headers[String(names[i]).to_lower()] = headers[names[i]];
	}
	while (client->get_status() == HTTPClient::STATUS_BODY) {
		client->poll();
		const PackedByteArray chunk = client->read_response_body_chunk();
		if (chunk.is_empty()) {
			if (timed_out()) {
				return response;
			}
			OS::get_singleton()->delay_msec(10);
		} else {
			response.body.append_array(chunk);
		}
	}
	response.code = client->get_response_code();
	return response;
}

// A picture as a square of PICTURE_SIZE pixels, or null when it can't be read.
Ref<Image> decode_picture(const PackedByteArray &p_bytes) {
	Ref<Image> image;
	image.instantiate();
	Error err = ERR_FILE_UNRECOGNIZED;
	if (p_bytes.size() > 4 && p_bytes[0] == 0x89 && p_bytes[1] == 'P') {
		err = image->load_png_from_buffer(p_bytes);
	} else if (p_bytes.size() > 3 && p_bytes[0] == 0xFF && p_bytes[1] == 0xD8) {
		err = image->load_jpg_from_buffer(p_bytes);
	} else if (p_bytes.size() > 12 && p_bytes[8] == 'W' && p_bytes[9] == 'E') {
		err = image->load_webp_from_buffer(p_bytes);
	}
	if (err != OK || image->is_empty()) {
		return Ref<Image>();
	}
	const int side = MIN(image->get_width(), image->get_height());
	image = image->get_region(Rect2i((image->get_width() - side) / 2, (image->get_height() - side) / 2, side, side));
	image->resize(PICTURE_SIZE, PICTURE_SIZE, Image::INTERPOLATE_LANCZOS);
	return image;
}

} // namespace

void GitAvatars::_bind_methods() {
	ADD_SIGNAL(MethodInfo("avatars_changed"));
}

void GitAvatars::_notification(int p_what) {
	if (p_what == NOTIFICATION_EXIT_TREE) {
		_stop_workers();
	}
}

void GitAvatars::set_repository(const String &p_workdir, const String &p_remote_url) {
	workdir = p_workdir;
	const String site = web_repository_url(p_remote_url);
	repository = site.begins_with("https://github.com/") ? site.trim_prefix("https://github.com/") : String();
	if (cache_dir.is_empty()) {
		cache_dir = EditorInterface::get_singleton()->get_editor_paths()->get_cache_dir().path_join("godot_git_avatars");
	}
}

void GitAvatars::set_enabled(bool p_enabled) {
	enabled = p_enabled;
}

Ref<Texture2D> GitAvatars::get_avatar(const Dictionary &p_commit) {
	const String email = String(p_commit.get("email", String())).strip_edges().to_lower();
	if (!enabled || email.is_empty() || cache_dir.is_empty()) {
		return Ref<Texture2D>();
	}
	_load_index();
	Ref<Texture2D> texture = textures.get(email, Variant());
	const Dictionary entry = index.get(email, Dictionary());
	const String file = entry.get("file", String());
	if (texture.is_null() && !file.is_empty()) {
		texture = _load_texture(file);
		if (texture.is_valid()) {
			textures[email] = texture;
		}
	}

	// Checked once a day: a changed picture (or a new account) shows up the next day. A picture
	// whose file went missing is fetched again right away.
	const bool missing = !file.is_empty() && texture.is_null();
	bool ask = entry.is_empty() || missing || now() - (int64_t)entry.get("time", 0) > CHECK_AFTER;
	ask = ask && !asked.has(email) && paused_until.load() <= now() && (int64_t)retry_after.get(email, 0) <= now();
	const String picture = entry.get("picture", String());
	// Finding the account needs the API, which only knows pushed commits (an unpushed one reads
	// as "no such commit"); once found, checking the picture needs neither.
	ask = ask && (!picture.is_empty() || !noreply_picture(email).is_empty() || (!repository.is_empty() && !bool(p_commit.get("unpushed", false))));
	if (ask) {
		asked[email] = true;
		{
			std::lock_guard<std::mutex> lock(mutex);
			pending.push_back({ email, p_commit.get("hash", String()), repository, picture, missing ? String() : String(entry.get("etag", String())) });
		}
		_start_workers();
	}
	return texture;
}

// Starts workers for the waiting requests, up to MAX_WORKERS at a time.
void GitAvatars::_start_workers() {
	int start = 0;
	{
		std::lock_guard<std::mutex> lock(mutex);
		start = MIN(MAX_WORKERS - running, (int)pending.size() - running);
		if (start <= 0) {
			return; // The running ones take the new requests too.
		}
		running += start;
	}
	// Threads that have finished still need joining.
	for (int i = (int)threads.size() - 1; i >= 0; i--) {
		if (!threads[i]->is_alive()) {
			threads[i]->wait_to_finish();
			threads.erase(threads.begin() + i);
		}
	}
	for (int i = 0; i < start; i++) {
		Ref<Thread> thread;
		thread.instantiate();
		thread->start(callable_mp(this, &GitAvatars::_worker).bind(workdir));
		threads.push_back(thread);
	}
}

void GitAvatars::_stop_workers() {
	stopping = true;
	for (const Ref<Thread> &thread : threads) {
		if (thread->is_started()) {
			thread->wait_to_finish();
		}
	}
	threads.clear();
	stopping = false;
	std::lock_guard<std::mutex> lock(mutex);
	pending.clear();
	running = 0;
	asked.clear();
}

// On a worker thread: requests until none are left.
void GitAvatars::_worker(const String &p_workdir) {
	while (true) {
		Request request;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping || pending.empty()) {
				running--;
				return;
			}
			request = pending.front();
			pending.erase(pending.begin());
		}
		callable_mp(this, &GitAvatars::_lookup_done).call_deferred(_look_up(request, p_workdir));
	}
}

// git's saved login for GitHub, read once for all the workers. Its password is a token.
String GitAvatars::_token(const String &p_workdir) {
	std::lock_guard<std::mutex> lock(login_mutex);
	if (!login_read) {
		login_read = true;
		Ref<GitRepository> worker_repo;
		worker_repo.instantiate();
		worker_repo->open(p_workdir);
		token = Dictionary(worker_repo->get_saved_login("https://github.com")).get("password", String());
	}
	return token;
}

// One email: { "email", "result": "picture" (new or changed; "file", "picture", "etag"),
// "unchanged", "none" (no GitHub account), or "retry" (offline, rate limited, ...) }.
Dictionary GitAvatars::_look_up(const Request &p_request, const String &p_workdir) {
	Dictionary result;
	result["email"] = p_request.email;
	result["result"] = "retry";

	// 1. The account's picture address: known already, in a no-reply email, or from the API.
	String picture = p_request.picture;
	if (picture.is_empty()) {
		picture = noreply_picture(p_request.email);
	}
	if (picture.is_empty()) {
		// The commit list rather than the commit itself, which comes with every file's patch.
		const String url = vformat("https://api.github.com/repos/%s/commits?sha=%s&per_page=1", p_request.repository, p_request.hash);
		const PackedStringArray headers = { "User-Agent: godot-git", "Accept: application/vnd.github+json" };
		Response response;
		const String login = _token(p_workdir);
		if (!login.is_empty()) {
			PackedStringArray with_login = headers;
			with_login.push_back("Authorization: Bearer " + login);
			response = http_get(url, with_login, stopping);
			if (response.code == 401) {
				// Not a token GitHub's API takes; public repositories work without one.
				std::lock_guard<std::mutex> lock(login_mutex);
				token = String();
			}
		}
		if (login.is_empty() || response.code == 401) {
			response = http_get(url, headers, stopping);
		}
		if (response.code == 429 || (response.code == 403 && String(response.headers.get("x-ratelimit-remaining", "")) == "0")) {
			paused_until = now() + 3600;
			return result;
		}
		if (response.code == 404) {
			result["result"] = "none"; // A private repository without a login, most likely.
			return result;
		}
		if (response.code != 200) {
			return result; // Offline, or a commit GitHub doesn't have (yet).
		}
		const Variant commits = JSON::parse_string(response.body.get_string_from_utf8());
		const Dictionary first = commits.get_type() == Variant::ARRAY && !Array(commits).is_empty() ? Dictionary(Array(commits)[0]) : Dictionary();
		const Variant author = first.get("author", Variant());
		const String address = author.get_type() == Variant::DICTIONARY ? String(Dictionary(author).get("avatar_url", String())) : String();
		if (address.is_empty()) {
			result["result"] = "none"; // No GitHub account has that email.
			return result;
		}
		picture = vformat("%s?s=%d", address.get_slice("?", 0), PICTURE_SIZE);
	}

	// 2. The picture, unless it's the one we have (GitHub's picture server isn't rate limited).
	Response response;
	for (int redirect = 0; redirect < 4; redirect++) {
		PackedStringArray headers = { "User-Agent: godot-git" };
		if (!p_request.etag.is_empty()) {
			headers.push_back("If-None-Match: " + p_request.etag);
		}
		response = http_get(picture, headers, stopping);
		if (response.code < 300 || response.code >= 400 || response.code == 304 || !response.headers.has("location")) {
			break;
		}
		// "github.com/name.png" sends us on to the account's address, which is the one to keep.
		picture = vformat("%s?s=%d", String(response.headers["location"]).get_slice("?", 0), PICTURE_SIZE);
	}
	if (response.code == 304) {
		result["result"] = "unchanged";
		return result;
	}
	if (response.code == 404) {
		result["result"] = "none"; // The account is gone.
		return result;
	}
	const Ref<Image> image = response.code == 200 ? decode_picture(response.body) : Ref<Image>();
	if (image.is_null()) {
		return result;
	}
	const String file = p_request.email.md5_text() + ".png";
	DirAccess::make_dir_recursive_absolute(cache_dir);
	if (image->save_png(cache_dir.path_join(file)) != OK) {
		return result;
	}
	result["result"] = "picture";
	result["file"] = file;
	result["picture"] = picture;
	result["etag"] = response.headers.get("etag", String());
	return result;
}

void GitAvatars::_lookup_done(const Dictionary &p_result) {
	const String email = p_result["email"];
	const String kind = p_result["result"];
	asked.erase(email);
	if (kind == "retry") {
		// Not remembered on disk, but not asked again for a while either (History redraws often).
		retry_after[email] = now() + RETRY_AFTER;
		return;
	}
	Dictionary entry = index.get(email, Dictionary());
	entry["time"] = now();
	if (kind == "none") {
		entry = Dictionary();
		entry["time"] = now();
		entry["file"] = String();
	} else if (kind == "picture") {
		entry["file"] = p_result["file"];
		entry["picture"] = p_result["picture"];
		entry["etag"] = p_result["etag"];
	}
	index[email] = entry;
	_save_index();
	if (kind == "picture") {
		textures.erase(email);
		emit_signal("avatars_changed");
	}
}

void GitAvatars::_load_index() {
	if (index_loaded) {
		return;
	}
	index_loaded = true;
	const String text = FileAccess::get_file_as_string(cache_dir.path_join("index.json"));
	const Variant parsed = text.is_empty() ? Variant() : JSON::parse_string(text);
	if (parsed.get_type() == Variant::DICTIONARY) {
		index = parsed;
	}
}

void GitAvatars::_save_index() {
	DirAccess::make_dir_recursive_absolute(cache_dir);
	const Ref<FileAccess> file = FileAccess::open(cache_dir.path_join("index.json"), FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_string(JSON::stringify(index, "\t"));
	}
}

// The cached picture, cut to a circle with a soft edge.
Ref<Texture2D> GitAvatars::_load_texture(const String &p_file) {
	const Ref<Image> image = Image::load_from_file(cache_dir.path_join(p_file));
	if (image.is_null() || image->is_empty()) {
		return Ref<Texture2D>();
	}
	image->convert(Image::FORMAT_RGBA8);
	const int size = image->get_width();
	const float radius = size / 2.0f;
	for (int y = 0; y < image->get_height(); y++) {
		for (int x = 0; x < size; x++) {
			const float distance = Vector2(x + 0.5f - radius, y + 0.5f - radius).length();
			const float coverage = CLAMP(radius - distance + 0.5f, 0.0f, 1.0f);
			if (coverage < 1.0f) {
				Color color = image->get_pixel(x, y);
				color.a *= coverage;
				image->set_pixel(x, y, color);
			}
		}
	}
	return ImageTexture::create_from_image(image);
}
