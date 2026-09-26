#pragma once

// Small wording helpers shared by the Git dock's sources.

#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace godot_git {

// Typographic minus, same width as "+".
String minus();

// One letter for a file state from GitRepository::get_status(): "M", "A", "D", "U", ...
String status_letter(const String &p_state);

// A file state as a word for tooltips: "Modified", "Added", ...
String status_name(const String &p_state);

// Compact age of a unix time: "now", "5m", "3h", "2d", "4mo", "1y".
String relative_time(int64_t p_unix_time);

// A unix time in the computer's time zone: "2026-09-26 14:32:05".
String local_date_time(int64_t p_unix_time);

// "just now", "5m ago", ...
String time_ago(int64_t p_unix_time);

// "1 file", "3 files".
String plural(int p_count, const String &p_singular, const String &p_plural);

} // namespace godot_git
