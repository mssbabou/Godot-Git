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

} // namespace godot_git
