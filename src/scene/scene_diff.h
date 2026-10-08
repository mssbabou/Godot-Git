// The scene module (src/scene/): Godot's scene and resource text formats (.tscn, .tres), read,
// compared and (later) merged. Text in, text or plain data out: it never includes libgit2 or
// editor classes, so it can be tested without a repository. src/git/ adapts it to the index,
// src/editor/ shows its results.

#pragma once

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace godot_git {

// Whether the Diff panel shows p_path as a scene or resource (`.tscn`, `.tres`): node by node.
bool is_scene_path(const String &p_path);

// What changed between two versions of a scene or resource file, as nodes and properties rather
// than lines. Nodes are matched by their unique_id (Godot 4.6+), else by path, so a renamed or
// moved node is one node. References compare by what they point at: an ExtResource by its path
// (ids are renumbered on save), a SubResource by its contents (a changed one shows as its own
// properties: "shape › size"). The header (load_steps, uid) is left out: Godot rewrites it.
// { "nodes": [{ "path": "Player/Sprite", "type", "status": "added" | "removed" | "changed" |
//     "renamed" | "moved", "old_path" (renamed, moved), "instance" (an instanced scene's path),
//     "properties": [{ "name", "old", "new" }, ...] ("old" or "new" missing on the side that
//     doesn't set it; long values shortened) }, ...],
//   "connections": [{ "status": "added" | "removed", "text" }, ...],
//   "context": { path: { "type", "instance" } } for every node of both versions (to show the
//     changed ones in place, under their parents),
//   "generated_only": true when the text changed but nothing above did }.
// Nodes in the new file's order, removed ones after. A `.tres` file's own properties are one node,
// "path" "" and "type" the resource's.
Dictionary scene_changes(const String &p_old_text, const String &p_new_text);

} // namespace godot_git
