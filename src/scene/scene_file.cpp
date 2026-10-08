#include "scene/scene_file.h"

namespace godot_git {

namespace {

// Where a value starting at p_from ends: at a line break, or (p_in_tag) a space or the tag's "]",
// outside strings and brackets.
int value_end(const String &p_text, int p_from, bool p_in_tag) {
	int depth = 0;
	bool in_string = false;
	const int length = p_text.length();
	for (int i = p_from; i < length; i++) {
		const char32_t c = p_text[i];
		if (in_string) {
			if (c == '\\') {
				i++;
			} else if (c == '"') {
				in_string = false;
			}
			continue;
		}
		if (c == '"') {
			in_string = true;
		} else if (c == '(' || c == '[' || c == '{') {
			depth++;
		} else if (c == ')' || c == ']' || c == '}') {
			if (depth == 0) {
				return i; // The tag's own "]".
			}
			depth--;
		} else if (depth == 0 && (c == '\n' || (p_in_tag && c == ' '))) {
			return i;
		}
	}
	return length;
}

} // namespace

bool scene_blank_line_between(const String &p_previous, const String &p_next) {
	// Godot's writer puts no blank line between two of these in a row.
	const bool together = p_next == "ext_resource" || p_next == "connection" || p_next == "editable";
	return !(together && p_previous == p_next);
}

String scene_unquote(const String &p_value) {
	String value = p_value.strip_edges();
	if (value.length() >= 2 && value.begins_with("\"") && value.ends_with("\"")) {
		value = value.substr(1, value.length() - 2).replace("\\\"", "\"").replace("\\\\", "\\");
	}
	return value;
}

String SceneSection::field(const String &p_name) const {
	const String *value = fields.getptr(p_name);
	return value ? scene_unquote(*value) : String();
}

SceneFile read_scene_file(const String &p_text) {
	SceneFile file;
	file.crlf = p_text.contains("\r\n");
	const String text = file.crlf ? p_text.replace("\r\n", "\n") : p_text;
	const int length = text.length();
	LocalVector<int> starts; // Where each section's "[" is.
	int i = 0;
	while (i < length) {
		// At the start of a line.
		while (i < length && (text[i] == ' ' || text[i] == '\t')) {
			i++;
		}
		if (i >= length) {
			break;
		}
		if (text[i] == '\n') {
			i++;
			continue;
		}
		if (text[i] == ';') { // A comment.
			const int end = text.find("\n", i);
			i = end < 0 ? length : end + 1;
			continue;
		}
		if (text[i] == '[') {
			SceneSection section;
			starts.push_back(i);
			int at = i + 1;
			while (at < length && text[at] != ' ' && text[at] != ']' && text[at] != '\n') {
				at++;
			}
			section.tag = text.substr(i + 1, at - i - 1);
			while (at < length && text[at] != ']' && text[at] != '\n') {
				if (text[at] == ' ') {
					at++;
					continue;
				}
				const int equals = text.find("=", at);
				if (equals < 0) {
					break;
				}
				const String key = text.substr(at, equals - at).strip_edges();
				const int end = value_end(text, equals + 1, true);
				section.fields[key] = text.substr(equals + 1, end - equals - 1);
				at = end;
			}
			const int line_end = text.find("\n", at);
			i = line_end < 0 ? length : line_end + 1;
			file.sections.push_back(section);
			continue;
		}
		// "key = value" under the last section. A name with "=", quotes or spaces in it is
		// written in quotes (String::property_name_encode), so its "=" comes after the closing quote.
		const int key_end = text[i] == '"' ? value_end(text, i, true) : i;
		const int equals = text.find("=", key_end);
		const int line_end = text.find("\n", key_end);
		if (equals < 0 || (line_end >= 0 && equals > line_end)) {
			i = line_end < 0 ? length : line_end + 1; // Not a property.
			continue;
		}
		const int end = value_end(text, equals + 1, false);
		if (!file.sections.is_empty()) {
			SceneProperty property;
			property.key = scene_unquote(text.substr(i, equals - i));
			property.value = text.substr(equals + 1, end - equals - 1).strip_edges();
			file.sections[file.sections.size() - 1].properties.push_back(property);
		}
		i = end + 1;
	}

	// Each section's text runs to the next one's "["; the line breaks after it are the layout's.
	if (starts.is_empty()) {
		file.preamble = text;
		return file;
	}
	file.preamble = text.substr(0, starts[0]);
	for (uint32_t s = 0; s < starts.size(); s++) {
		const int end = s + 1 < starts.size() ? starts[s + 1] : length;
		String chunk = text.substr(starts[s], end - starts[s]);
		int kept = chunk.length();
		while (kept > 0 && chunk[kept - 1] == '\n') {
			kept--;
		}
		file.sections[s].gap = chunk.substr(kept);
		if (s + 1 == starts.size()) {
			file.ending = file.sections[s].gap;
		}
		file.sections[s].text = chunk.left(kept);
	}
	return file;
}

String SceneFile::write() const {
	String text = preamble;
	for (uint32_t s = 0; s < sections.size(); s++) {
		if (s > 0) {
			text += scene_blank_line_between(sections[s - 1].tag, sections[s].tag) ? String("\n\n") : String("\n");
		}
		text += sections[s].text;
	}
	text += ending;
	return crlf ? text.replace("\n", "\r\n") : text;
}

} // namespace godot_git
