#pragma once

// Reading and writing Godot's text scene and resource files (.tscn, .tres) as sections: the
// header, [ext_resource], [sub_resource], [node], [connection], [editable], [resource]. Shared by
// the scene diff and the scene merge. Part of the scene module (see scene_diff.h).

#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace godot_git {

// One "key = value" line under a section (the value may span lines: arrays, dictionaries,
// strings with line breaks).
struct SceneProperty {
	String key; // Unquoted ("a=b" for a name Godot wrote in quotes).
	String value; // As written.
};

struct SceneSection {
	String tag; // "gd_scene", "gd_resource", "ext_resource", "sub_resource", "node", ...
	HashMap<String, String> fields; // The tag's fields as written (strings keep their quotes).
	LocalVector<SceneProperty> properties;
	String text; // The section exactly as written, from its "[" up to the next one, without the blank lines after it.
	String gap; // The line break and blank lines after it, as written.

	String field(const String &p_name) const; // Unquoted, "" when missing.
};

struct SceneFile {
	String preamble; // Anything before the first section (normally nothing).
	LocalVector<SceneSection> sections;
	String ending; // What follows the last section's text (its line break and any blank lines).
	bool crlf = false;

	// The sections laid out the way Godot 4.7's writer does: no blank line between
	// [ext_resource]s, between [connection]s or between [editable]s, one blank line between any
	// other two sections. Line breaks as the file had them.
	String write() const;
};

// Whether Godot's writer leaves a blank line between a p_previous section and a p_next one.
bool scene_blank_line_between(const String &p_previous, const String &p_next);

// p_text read into sections. Never fails: what isn't a section or a property is kept in the
// sections' text as it is.
SceneFile read_scene_file(const String &p_text);

// A field's or property's value without its quotes ("\"res://a.png\"" -> "res://a.png").
String scene_unquote(const String &p_value);

} // namespace godot_git
