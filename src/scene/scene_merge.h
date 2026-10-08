#pragma once

// Merging three versions of a scene or resource file (.tscn, .tres) section by section, where git
// merges lines: most scene conflicts are both sides adding different resources and nodes in the
// same place, which git can't tell from a real clash. Part of the scene module (see scene_diff.h).

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace godot_git {

// Merges mine and theirs, both descended from base ("" when both sides added the file). Sections
// are matched by what they are: [ext_resource] by path, [sub_resource] by id, [node] by unique_id
// (else its path), [connection] by signal, from, to and method, [editable] by path. A section
// only one side changed (added, removed, edited) takes that side; one both changed differently is
// a conflict. Theirs' resource ids are renamed where mine uses them for something else.
// { "ok": false, "reason": why it can't (not a scene, or the result would be broken: a node
//     without its parent, a reference to a resource that's gone); git's line merge is then the way,
//   "ok": true, "segments": Array of String (merged text) and Dictionary { "base", "mine",
//     "theirs" } (a section both sides changed differently, each side's text, "" for none), in
//     order; joined, with one side picked per conflict, they're the file,
//   "conflicts": the number of those, "text": the whole file when there are none,
//   "from_mine" / "from_theirs": how many sections each side changed (added, edited, removed) }.
// The result's line breaks are mine's (CRLF or LF).
Dictionary merge_scene(const String &p_base, const String &p_mine, const String &p_theirs);

} // namespace godot_git
