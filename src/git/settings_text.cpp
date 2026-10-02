// Reading Godot's settings files (`.import`, `.uid`) well enough to say which settings changed.
// Only reads: values stay as written, and nothing is ever written back.

#include "git/settings_text.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot_git {

namespace {

// How much p_line opens brackets: a value like `metadata={` goes on until they're closed.
// r_in_string carries a string that spans lines.
int bracket_depth(const String &p_line, bool &r_in_string) {
	int depth = 0;
	for (int i = 0; i < p_line.length(); i++) {
		const char32_t c = p_line[i];
		if (r_in_string) {
			if (c == '\\') {
				i++;
			} else if (c == '"') {
				r_in_string = false;
			}
		} else if (c == '"') {
			r_in_string = true;
		} else if (c == '{' || c == '[' || c == '(') {
			depth++;
		} else if (c == '}' || c == ']' || c == ')') {
			depth--;
		}
	}
	return depth;
}

// { "section\nkey": value }, in the file's order.
Dictionary parse(const String &p_path, const String &p_text) {
	Dictionary result;
	const String text = p_text.replace("\r\n", "\n");
	if (p_path.ends_with(".uid")) {
		const String uid = text.strip_edges();
		if (!uid.is_empty()) {
			result["\nuid"] = uid;
		}
		return result;
	}
	const PackedStringArray lines = text.split("\n");
	String section;
	for (int i = 0; i < lines.size(); i++) {
		const String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with(";") || line.begins_with("#")) {
			continue;
		}
		if (line.begins_with("[") && line.ends_with("]")) {
			section = line.substr(1, line.length() - 2);
			continue;
		}
		const int equals = line.find("=");
		if (equals <= 0) {
			continue;
		}
		const String key = line.left(equals).strip_edges();
		String value = line.substr(equals + 1).strip_edges();
		bool in_string = false;
		int depth = bracket_depth(value, in_string);
		while ((depth > 0 || in_string) && i + 1 < lines.size()) {
			i++;
			value += "\n" + lines[i];
			depth += bracket_depth(lines[i], in_string);
		}
		result[vformat("%s\n%s", section, key)] = value;
	}
	return result;
}

Dictionary change(const String &p_id) {
	Dictionary result;
	const int split = p_id.find("\n");
	result["section"] = p_id.left(split);
	result["key"] = p_id.substr(split + 1);
	return result;
}

} // namespace

bool is_settings_path(const String &p_path) {
	const String name = p_path.get_file();
	return p_path.ends_with(".import") || p_path.ends_with(".uid") || name == "project.godot" || name == "export_presets.cfg" || name == "override.cfg";
}

Array settings_changes(const String &p_path, const String &p_old_text, const String &p_new_text) {
	const Dictionary old_values = parse(p_path, p_old_text);
	const Dictionary new_values = parse(p_path, p_new_text);
	Array result;
	for (const Variant &id : new_values.keys()) {
		if (old_values.has(id) && old_values[id] == new_values[id]) {
			continue;
		}
		Dictionary item = change(id);
		if (old_values.has(id)) {
			item["old"] = old_values[id];
		}
		item["new"] = new_values[id];
		result.push_back(item);
	}
	for (const Variant &id : old_values.keys()) {
		if (!new_values.has(id)) {
			Dictionary item = change(id);
			item["old"] = old_values[id];
			result.push_back(item);
		}
	}
	return result;
}

Array settings_units(const String &p_text, String &r_section) {
	Array units;
	const PackedStringArray lines = p_text.split("\n");
	// split() leaves one empty string after a final newline: that's not a line.
	const int count = p_text.ends_with("\n") ? lines.size() - 1 : lines.size();
	for (int i = 0; i < count; i++) {
		const bool last = i == lines.size() - 1;
		String text = last ? lines[i] : lines[i] + String("\n");
		const String line = lines[i].strip_edges();
		Dictionary unit;
		unit["id"] = String();
		if (line.begins_with("[") && line.ends_with("]")) {
			r_section = line.substr(1, line.length() - 2);
		} else if (!line.begins_with(";") && !line.begins_with("#") && line.find("=") > 0) {
			const int equals = line.find("=");
			String value = line.substr(equals + 1).strip_edges();
			bool in_string = false;
			int depth = bracket_depth(value, in_string);
			while ((depth > 0 || in_string) && i + 1 < count) {
				i++;
				value += "\n" + lines[i];
				text += i == lines.size() - 1 ? lines[i] : lines[i] + String("\n");
				depth += bracket_depth(lines[i], in_string);
			}
			unit["id"] = vformat("%s\n%s", r_section, line.left(equals).strip_edges());
			unit["value"] = value;
		}
		unit["text"] = text;
		units.push_back(unit);
	}
	return units;
}

String settings_value(const String &p_text, const String &p_section, const String &p_key) {
	return parse(String(), p_text).get(vformat("%s\n%s", p_section, p_key), String());
}

} // namespace godot_git
