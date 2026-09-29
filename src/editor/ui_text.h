#pragma once

// Small wording helpers shared by the Git dock's sources.

#include <godot_cpp/classes/font.hpp>
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

// "a", "a and b", "a, b and c". With p_max_shown, only that many: "a, b, c and 2 more".
String join_list(const PackedStringArray &p_items, int p_max_shown = 0);

// p_text shortened in the middle ("final_boss…frame_012.png") to fit p_width in p_font, keeping a
// bit more of the end than the start: that's where file names usually differ.
String trim_middle(const String &p_text, const Ref<Font> &p_font, int p_font_size, float p_width);

// A remote's URL (https, ssh:// or scp-style git@host:owner/repo) as the repository's web page,
// e.g. "https://github.com/owner/repo". Only for GitHub, GitLab and Bitbucket; "" otherwise.
String web_repository_url(const String &p_remote_url);

// "GitHub", "GitLab" or "Bitbucket" for a web_repository_url().
String web_host_name(const String &p_web_url);

} // namespace godot_git
