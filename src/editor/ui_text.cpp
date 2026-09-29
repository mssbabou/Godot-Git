#include "editor/ui_text.h"

#include <godot_cpp/classes/time.hpp>

namespace godot_git {

// godot-cpp reads plain "..." literals as Latin-1, hence String::utf8. (A function, not a global:
// Strings can't be built before the extension is initialized.)
String minus() {
	return String::utf8("−");
}

String status_letter(const String &p_state) {
	if (p_state == "new") {
		return "A";
	}
	if (p_state == "modified") {
		return "M";
	}
	if (p_state == "deleted") {
		return "D";
	}
	if (p_state == "renamed") {
		return "R";
	}
	if (p_state == "copied") {
		return "C";
	}
	if (p_state == "typechange") {
		return "T";
	}
	if (p_state == "untracked") {
		return "U";
	}
	if (p_state == "conflicted") {
		return "!";
	}
	return "?";
}

String status_name(const String &p_state) {
	if (p_state == "new") {
		return "Added";
	}
	return p_state.capitalize();
}

String relative_time(int64_t p_unix_time) {
	const int64_t seconds = (int64_t)Time::get_singleton()->get_unix_time_from_system() - p_unix_time;
	if (seconds < 60) {
		return "now";
	}
	if (seconds < 3600) {
		return vformat("%dm", seconds / 60);
	}
	if (seconds < 86400) {
		return vformat("%dh", seconds / 3600);
	}
	if (seconds < 86400 * 30) {
		return vformat("%dd", seconds / 86400);
	}
	if (seconds < 86400 * 365) {
		return vformat("%dmo", seconds / (86400 * 30));
	}
	return vformat("%dy", seconds / (86400 * 365));
}

String local_date_time(int64_t p_unix_time) {
	const int64_t bias_minutes = Dictionary(Time::get_singleton()->get_time_zone_from_system()).get("bias", 0);
	return Time::get_singleton()->get_datetime_string_from_unix_time(p_unix_time + bias_minutes * 60, true);
}

String time_ago(int64_t p_unix_time) {
	const String when = relative_time(p_unix_time);
	return when == "now" ? String("just now") : vformat("%s ago", when);
}

String plural(int p_count, const String &p_singular, const String &p_plural) {
	return vformat("%d %s", p_count, p_count == 1 ? p_singular : p_plural);
}

String join_list(const PackedStringArray &p_items, int p_max_shown) {
	if (p_max_shown > 0 && p_items.size() > p_max_shown) {
		return vformat("%s and %d more", String(", ").join(p_items.slice(0, p_max_shown)), p_items.size() - p_max_shown);
	}
	if (p_items.size() <= 1) {
		return p_items.is_empty() ? String() : p_items[0];
	}
	return vformat("%s and %s", String(", ").join(p_items.slice(0, -1)), p_items[p_items.size() - 1]);
}

String trim_middle(const String &p_text, const Ref<Font> &p_font, int p_font_size, float p_width) {
	auto width = [&](const String &p_candidate) {
		return p_font->get_string_size(p_candidate, HORIZONTAL_ALIGNMENT_LEFT, -1, p_font_size).x;
	};
	if (width(p_text) <= p_width) {
		return p_text;
	}
	const String ellipsis = String::utf8("…");
	auto shortened = [&](int p_kept) {
		const int head = p_kept * 2 / 5;
		return p_text.left(head) + ellipsis + p_text.right(p_kept - head);
	};
	int low = 0; // The most characters that still fit, found by bisection.
	int high = p_text.length() - 1;
	while (low < high) {
		const int mid = (low + high + 1) / 2;
		if (width(shortened(mid)) <= p_width) {
			low = mid;
		} else {
			high = mid - 1;
		}
	}
	return low > 0 ? shortened(low) : ellipsis;
}

String web_repository_url(const String &p_remote_url) {
	String rest = p_remote_url.strip_edges();
	// "git@github.com:owner/repo.git" (scp-style) or "ssh://git@github.com/owner/repo.git".
	if (!rest.contains("://") && rest.contains(":")) {
		const int colon = rest.find(":");
		rest = vformat("%s/%s", rest.substr(0, colon), rest.substr(colon + 1));
	} else {
		rest = rest.get_slice("://", 1);
	}
	rest = rest.get_slice("@", rest.get_slice_count("@") - 1); // Drop "git@" or "user:token@".
	String host = rest.get_slice("/", 0).get_slice(":", 0).to_lower(); // No port.
	const String path = rest.substr(rest.find("/") + 1).trim_suffix("/").trim_suffix(".git");
	if (rest.find("/") < 0 || path.get_slice_count("/") < 2) {
		return String();
	}
	if (host == "ssh.github.com") {
		host = "github.com";
	}
	if (host != "github.com" && host != "gitlab.com" && host != "bitbucket.org") {
		return String();
	}
	return vformat("https://%s/%s", host, path);
}

String web_host_name(const String &p_web_url) {
	if (p_web_url.contains("://gitlab.com/")) {
		return "GitLab";
	}
	if (p_web_url.contains("://bitbucket.org/")) {
		return "Bitbucket";
	}
	return "GitHub";
}

} // namespace godot_git
