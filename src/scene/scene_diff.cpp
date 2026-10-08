#include "scene/scene_diff.h"

#include "scene/scene_file.h"

#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/transform2d.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot_git {

namespace {

constexpr int MAX_VALUE_LENGTH = 160; // Longer values are shortened for display ("…").

// The file's sections, through the shared reader (scene_file.h).
using Property = SceneProperty;
using Entry = SceneSection;

String unquote(const String &p_value) {
	return scene_unquote(p_value);
}

LocalVector<Entry> parse(const String &p_text) {
	return read_scene_file(p_text).sections;
}

// A scene or resource file, read for comparing.
struct Node {
	String name;
	String type;
	String parent; // "" for the root, "." for its children, "A/B" further down.
	String instance; // An instanced scene's path.
	String unique_id;
	String groups;
	LocalVector<Property> properties;

	// Its path from the root's name: "Player", "Player/Sprite".
	String path(const String &p_root) const {
		if (parent.is_empty()) {
			return name;
		}
		return parent == "." ? vformat("%s/%s", p_root, name) : vformat("%s/%s/%s", p_root, parent, name);
	}
};

struct Scene {
	HashMap<String, String> ext_paths; // ExtResource id -> path.
	HashMap<String, String> ext_types;
	HashMap<String, int> subs; // SubResource id -> its index in entries (an index: the scene is copied around).
	LocalVector<Node> nodes;
	PackedStringArray connections; // As text, references resolved.
	HashSet<String> editable; // Instanced nodes whose children are editable.
	String resource_type; // A `.tres` file's type ("" for a scene).
	LocalVector<Entry> entries;

	String root_name() const {
		return nodes.is_empty() ? String() : nodes[0].name;
	}
};

// p_value with every ExtResource("id") replaced by its path and every SubResource("id") by its
// type and contents, so values compare by what they mean, not by ids Godot renumbers on save.
// p_display: sub-resources as their type only ("RectangleShape2D"), for showing.
String resolve(const Scene &p_scene, const String &p_value, bool p_display, int p_depth = 0) {
	if (!p_value.contains("Resource(\"")) {
		return p_value;
	}
	String result;
	int at = 0;
	while (true) {
		const int ext = p_value.find("ExtResource(\"", at);
		const int sub = p_value.find("SubResource(\"", at);
		int start = ext < 0 ? sub : (sub < 0 ? ext : MIN(ext, sub));
		if (start < 0) {
			result += p_value.substr(at);
			break;
		}
		const bool is_ext = start == ext;
		const int id_start = start + 13;
		const int id_end = p_value.find("\")", id_start);
		if (id_end < 0) {
			result += p_value.substr(at);
			break;
		}
		result += p_value.substr(at, start - at);
		const String id = p_value.substr(id_start, id_end - id_start);
		if (is_ext) {
			const String *path = p_scene.ext_paths.getptr(id);
			result += path ? *path : vformat("ExtResource(\"%s\")", id);
		} else {
			const int *index = p_scene.subs.getptr(id);
			const Entry *entry = index ? &p_scene.entries[*index] : nullptr;
			if (!entry) {
				result += vformat("SubResource(\"%s\")", id);
			} else {
				const String type = unquote(entry->fields.has("type") ? entry->fields["type"] : String());
				if (p_display || p_depth > 8) {
					result += type;
				} else {
					PackedStringArray parts;
					for (const Property &property : entry->properties) {
						parts.push_back(vformat("%s=%s", property.key, resolve(p_scene, property.value, false, p_depth + 1)));
					}
					result += vformat("%s{%s}", type, String(";").join(parts));
				}
			}
		}
		at = id_end + 2;
	}
	return result;
}

Scene read_scene(const String &p_text) {
	Scene scene;
	scene.entries = parse(p_text);
	for (uint32_t e = 0; e < scene.entries.size(); e++) {
		const Entry &entry = scene.entries[e];
		if (entry.tag == "ext_resource") {
			const String id = unquote(entry.fields.has("id") ? entry.fields["id"] : String());
			scene.ext_paths[id] = unquote(entry.fields.has("path") ? entry.fields["path"] : String());
			scene.ext_types[id] = unquote(entry.fields.has("type") ? entry.fields["type"] : String());
		} else if (entry.tag == "sub_resource") {
			scene.subs[unquote(entry.fields.has("id") ? entry.fields["id"] : String())] = e;
		} else if (entry.tag == "gd_resource") {
			scene.resource_type = unquote(entry.fields.has("script_class") ? entry.fields["script_class"] : (entry.fields.has("type") ? entry.fields["type"] : String()));
		}
	}
	for (const Entry &entry : scene.entries) {
		if (entry.tag == "node") {
			Node node;
			node.name = unquote(entry.fields.has("name") ? entry.fields["name"] : String());
			node.type = unquote(entry.fields.has("type") ? entry.fields["type"] : String());
			node.parent = entry.fields.has("parent") ? unquote(entry.fields["parent"]) : String();
			node.unique_id = entry.fields.has("unique_id") ? entry.fields["unique_id"] : String();
			node.groups = entry.fields.has("groups") ? entry.fields["groups"] : String();
			if (entry.fields.has("instance")) {
				node.instance = resolve(scene, entry.fields["instance"], true);
			}
			node.properties = entry.properties;
			scene.nodes.push_back(node);
		} else if (entry.tag == "resource") {
			Node node; // A `.tres` file's own properties.
			node.type = scene.resource_type;
			node.properties = entry.properties;
			scene.nodes.push_back(node);
		} else if (entry.tag == "connection") {
			auto field = [&](const char *p_name) { return entry.fields.has(p_name) ? unquote(entry.fields[p_name]) : String(); };
			// "." is the root: named, so it reads as a node like the others.
			auto node_name = [&](const String &p_path) { return p_path == "." ? scene.root_name() : p_path; };
			String text = vformat(String::utf8("%s: %s → %s.%s()"), field("signal"), node_name(field("from")), node_name(field("to")), field("method"));
			if (entry.fields.has("binds")) {
				text += vformat(" with %s", entry.fields["binds"]);
			}
			if (entry.fields.has("unbinds")) {
				text += vformat(", %s unbound", entry.fields["unbinds"]);
			}
			// Object::ConnectFlags (4.7.2), as the Connect dialog names them. Persist is always on.
			const int64_t flags = entry.fields.has("flags") ? entry.fields["flags"].to_int() : 0;
			PackedStringArray options;
			if (flags & 1) {
				options.push_back("deferred");
			}
			if (flags & 4) {
				options.push_back("one shot");
			}
			if (flags & 16) {
				options.push_back("append source");
			}
			if (!options.is_empty()) {
				text += vformat(" (%s)", String(", ").join(options));
			}
			scene.connections.push_back(text);
		} else if (entry.tag == "editable") {
			scene.editable.insert(unquote(entry.fields.has("path") ? entry.fields["path"] : String()));
		}
	}
	return scene;
}

String shorten(const String &p_value) {
	String value = p_value.replace("\n", " ");
	if (value.length() > MAX_VALUE_LENGTH) {
		value = vformat(String::utf8("%s…"), value.left(MAX_VALUE_LENGTH - 1));
	}
	return value;
}

// The id of the one sub-resource a value is, or "" ("SubResource(\"Shape_ab12c\")" -> "Shape_ab12c").
String single_sub(const String &p_value) {
	if (p_value.begins_with("SubResource(\"") && p_value.ends_with("\")") && p_value.count("\"") == 2) {
		return p_value.substr(13, p_value.length() - 15);
	}
	return String();
}

// p_value's numbers taken out (into r_numbers), each replaced by "#": what's left says whether two
// values have the same shape. A digit inside a name ("Vector3") isn't a number.
String split_numbers(const String &p_value, LocalVector<double> &r_numbers) {
	String shape;
	const int length = p_value.length();
	for (int i = 0; i < length;) {
		const char32_t c = p_value[i];
		const char32_t before = i > 0 ? p_value[i - 1] : ' ';
		const bool after_name = (before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') || before == '_' || (before >= '0' && before <= '9');
		const bool starts = !after_name && ((c >= '0' && c <= '9') || ((c == '-' || c == '.') && i + 1 < length && p_value[i + 1] >= '0' && p_value[i + 1] <= '9'));
		if (!starts) {
			shape += String::chr(c);
			i++;
			continue;
		}
		int end = i + 1;
		while (end < length) {
			const char32_t d = p_value[end];
			if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || ((d == '-' || d == '+') && (p_value[end - 1] == 'e'))) {
				end++;
			} else {
				break;
			}
		}
		r_numbers.push_back(p_value.substr(i, end - i).to_float());
		shape += String("#");
		i = end;
	}
	return shape;
}

bool close(double p_a, double p_b) {
	return Math::abs(p_a - p_b) <= 1e-4 * MAX(1.0, MAX(Math::abs(p_a), Math::abs(p_b)));
}

// Whether two values differ only by float noise (a transform recomputed when its node moved,
// 20.696024 -> 20.696026).
bool numbers_equal(const String &p_old, const String &p_new) {
	LocalVector<double> a, b;
	if (split_numbers(p_old, a) != split_numbers(p_new, b) || a.size() != b.size()) {
		return false;
	}
	for (uint32_t i = 0; i < a.size(); i++) {
		if (!close(a[i], b[i])) {
			return false;
		}
	}
	return true;
}

// A number as the Inspector would show it: up to 3 decimals, no "-0".
String number(double p_value) {
	double value = Math::snapped(p_value, 0.001);
	if (Math::abs(value) < 0.0005) {
		value = 0;
	}
	const String text = String::num(value, 3);
	return text.ends_with(".0") ? text.left(text.length() - 2) : text;
}

String vector_text(const Vector3 &p_vector) {
	return vformat("(%s, %s, %s)", number(p_vector.x), number(p_vector.y), number(p_vector.z));
}

String vector_text(const Vector2 &p_vector) {
	return vformat("(%s, %s)", number(p_vector.x), number(p_vector.y));
}

// A transform as the Inspector shows a node's: position, rotation (degrees) and scale, each as
// text; empty when p_value isn't a Transform3D or Transform2D.
PackedStringArray transform_parts(const String &p_value) {
	PackedStringArray parts;
	if (!p_value.begins_with("Transform3D(") && !p_value.begins_with("Transform2D(")) {
		return parts;
	}
	const Variant value = UtilityFunctions::str_to_var(p_value);
	if (value.get_type() == Variant::TRANSFORM3D) {
		const Transform3D transform = value;
		const Vector3 euler = transform.basis.get_euler() * (180.0 / Math::PI);
		parts.push_back(vector_text(transform.origin));
		parts.push_back(vformat(String::utf8("(%s°, %s°, %s°)"), number(euler.x), number(euler.y), number(euler.z)));
		parts.push_back(vector_text(transform.basis.get_scale()));
	} else if (value.get_type() == Variant::TRANSFORM2D) {
		const Transform2D transform = value;
		parts.push_back(vector_text(transform.get_origin()));
		parts.push_back(vformat(String::utf8("%s°"), number(transform.get_rotation() * (180.0 / Math::PI))));
		parts.push_back(vector_text(transform.get_scale()));
	}
	return parts;
}

const char *TRANSFORM_PART_NAMES[3] = { "position", "rotation", "scale" };

const Property *find_property(const LocalVector<Property> &p_properties, const String &p_key) {
	for (const Property &property : p_properties) {
		if (property.key == p_key) {
			return &property;
		}
	}
	return nullptr;
}

// The property changes between two lists of properties, appended to r_changes. A property that is
// a sub-resource of the same type on both sides shows as that sub-resource's own changes
// ("shape › size"), however its id changed.
void compare_properties(const Scene &p_old, const LocalVector<Property> &p_old_props, const Scene &p_new, const LocalVector<Property> &p_new_props, const String &p_prefix, Array &r_changes, int p_depth = 0) {
	PackedStringArray keys;
	for (const Property &property : p_new_props) {
		keys.push_back(property.key);
	}
	for (const Property &property : p_old_props) {
		if (!keys.has(property.key)) {
			keys.push_back(property.key);
		}
	}
	for (const String &key : keys) {
		const Property *before = find_property(p_old_props, key);
		const Property *after = find_property(p_new_props, key);
		const String old_value = before ? resolve(p_old, before->value, false) : String();
		const String new_value = after ? resolve(p_new, after->value, false) : String();
		if (before && after && (old_value == new_value || numbers_equal(old_value, new_value))) {
			continue;
		}
		const String name = p_prefix.is_empty() ? key : vformat(String::utf8("%s › %s"), p_prefix, key);
		// A transform as position, rotation and scale, only the parts that changed.
		const PackedStringArray old_parts = before ? transform_parts(old_value) : PackedStringArray();
		const PackedStringArray new_parts = after ? transform_parts(new_value) : PackedStringArray();
		if (!old_parts.is_empty() || !new_parts.is_empty()) {
			const int count = MAX(old_parts.size(), new_parts.size());
			for (int i = 0; i < count; i++) {
				const String old_part = i < old_parts.size() ? old_parts[i] : String();
				const String new_part = i < new_parts.size() ? new_parts[i] : String();
				if (old_part == new_part || (!old_part.is_empty() && !new_part.is_empty() && numbers_equal(old_part, new_part))) {
					continue;
				}
				// A new node's rotation and scale only when they aren't the defaults.
				if (old_part.is_empty() && i > 0) {
					LocalVector<double> values;
					split_numbers(new_part, values);
					bool is_default = true;
					for (double value : values) {
						is_default = is_default && close(value, i == 1 ? 0.0 : 1.0);
					}
					if (is_default) {
						continue;
					}
				}
				Dictionary change;
				change["name"] = vformat(String::utf8("%s › %s"), name, TRANSFORM_PART_NAMES[i]);
				if (!old_part.is_empty()) {
					change["old"] = old_part;
				}
				if (!new_part.is_empty()) {
					change["new"] = new_part;
				}
				r_changes.push_back(change);
			}
			continue;
		}
		if (before && after && p_depth < 4) {
			const String old_sub = single_sub(before->value);
			const String new_sub = single_sub(after->value);
			const int *old_index = old_sub.is_empty() ? nullptr : p_old.subs.getptr(old_sub);
			const int *new_index = new_sub.is_empty() ? nullptr : p_new.subs.getptr(new_sub);
			const Entry *old_entry = old_index ? &p_old.entries[*old_index] : nullptr;
			const Entry *new_entry = new_index ? &p_new.entries[*new_index] : nullptr;
			if (old_entry && new_entry && old_entry->fields.has("type") && new_entry->fields.has("type") && old_entry->fields["type"] == new_entry->fields["type"]) {
				compare_properties(p_old, old_entry->properties, p_new, new_entry->properties, name, r_changes, p_depth + 1);
				continue;
			}
		}
		Dictionary change;
		change["name"] = name;
		if (before) {
			change["old"] = shorten(resolve(p_old, before->value, true));
		}
		if (after) {
			change["new"] = shorten(resolve(p_new, after->value, true));
		}
		r_changes.push_back(change);
	}
}

} // namespace

bool is_scene_path(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	return extension == "tscn" || extension == "tres";
}

Dictionary scene_changes(const String &p_old_text, const String &p_new_text) {
	const Scene before = read_scene(p_old_text);
	const Scene after = read_scene(p_new_text);
	const String old_root = before.root_name();
	const String new_root = after.root_name();

	// Matched by unique_id where both have one, else by path, under the parent's old path when the
	// parent was matched (so a node under a renamed parent is still found without an id).
	HashMap<String, int> old_by_id, old_by_path, new_by_path;
	for (uint32_t i = 0; i < before.nodes.size(); i++) {
		if (!before.nodes[i].unique_id.is_empty()) {
			old_by_id[before.nodes[i].unique_id] = i;
		}
		old_by_path[before.nodes[i].path(old_root)] = i;
	}
	// The node a node's "parent" names, as an index (-1 for the root or one not in the file).
	auto parent_index = [](const Node &p_node, const String &p_root, const HashMap<String, int> &p_by_path) {
		if (p_node.parent.is_empty()) {
			return -1;
		}
		const String parent_path = p_node.parent == "." ? p_root : vformat("%s/%s", p_root, p_node.parent);
		const int *index = p_by_path.getptr(parent_path);
		return index ? *index : -1;
	};
	HashSet<int> matched_old;
	LocalVector<int> new_to_old; // Parents come before their children in the file.
	Array nodes;
	for (uint32_t n = 0; n < after.nodes.size(); n++) {
		const Node &node = after.nodes[n];
		const String path = node.path(new_root);
		new_by_path[path] = n;
		const int new_parent = parent_index(node, new_root, new_by_path);
		const int parent_match = new_parent >= 0 ? new_to_old[new_parent] : -1;
		const String old_guess = parent_match >= 0 ? vformat("%s/%s", before.nodes[parent_match].path(old_root), node.name) : path;
		int match = -1;
		if (!node.unique_id.is_empty() && old_by_id.has(node.unique_id)) {
			match = old_by_id[node.unique_id];
		} else if (old_by_path.has(old_guess) && !matched_old.has(old_by_path[old_guess])) {
			match = old_by_path[old_guess];
		}
		new_to_old.push_back(match);
		Dictionary change;
		change["path"] = path;
		change["type"] = node.type;
		if (!node.instance.is_empty()) {
			change["instance"] = node.instance;
		}
		Array properties;
		if (match < 0) {
			change["status"] = "added";
			// Its settings, as compared with nothing (a transform as position, rotation and scale).
			compare_properties(before, LocalVector<Property>(), after, node.properties, String(), properties);
		} else {
			matched_old.insert(match);
			const Node &old = before.nodes[match];
			const String old_path = old.path(old_root);
			compare_properties(before, old.properties, after, node.properties, String(), properties);
			auto changed = [&](const String &p_name, const String &p_old, const String &p_new) {
				if (p_old != p_new) {
					Dictionary entry;
					entry["name"] = p_name;
					if (!p_old.is_empty()) {
						entry["old"] = shorten(p_old);
					}
					if (!p_new.is_empty()) {
						entry["new"] = shorten(p_new);
					}
					properties.push_front(entry);
				}
			};
			changed("Groups", old.groups, node.groups);
			changed("Instance of", old.instance, node.instance);
			changed("Type", old.type, node.type);
			const bool old_editable = before.editable.has(old.parent.is_empty() ? String(".") : old_path.substr(old_root.length() + 1));
			const bool new_editable = after.editable.has(node.parent.is_empty() ? String(".") : path.substr(new_root.length() + 1));
			if (old_editable != new_editable) {
				changed("Editable Children", old_editable ? "On" : "Off", new_editable ? "On" : "Off");
			}
			// Moved when its parent is another node, not when the parent's name changed (that
			// changes every child's "parent=").
			const int old_parent = parent_index(old, old_root, old_by_path);
			if (!node.parent.is_empty() && (new_parent < 0 || old_parent < 0 ? old.parent != node.parent : parent_match != old_parent)) {
				change["status"] = "moved"; // Renamed too, maybe: the old path says.
				change["old_path"] = old_path;
			} else if (old.name != node.name) {
				change["status"] = "renamed";
				change["old_path"] = old_path;
			} else if (properties.is_empty()) {
				continue; // Unchanged.
			} else {
				change["status"] = "changed";
			}
		}
		change["properties"] = properties;
		nodes.push_back(change);
	}
	for (uint32_t i = 0; i < before.nodes.size(); i++) {
		if (!matched_old.has(i)) {
			Dictionary change;
			// Under the new root's name, so it sits in the same tree when the root was renamed.
			const String old_path = before.nodes[i].path(old_root);
			change["path"] = before.nodes[i].parent.is_empty() || new_root.is_empty() ? old_path : vformat("%s%s", new_root, old_path.substr(old_root.length()));
			change["type"] = before.nodes[i].type;
			change["status"] = "removed";
			change["properties"] = Array();
			if (!before.nodes[i].instance.is_empty()) {
				change["instance"] = before.nodes[i].instance;
			}
			nodes.push_back(change);
		}
	}

	Array connections;
	for (const String &connection : after.connections) {
		if (!before.connections.has(connection)) {
			Dictionary entry;
			entry["status"] = "added";
			entry["text"] = connection;
			connections.push_back(entry);
		}
	}
	for (const String &connection : before.connections) {
		if (!after.connections.has(connection)) {
			Dictionary entry;
			entry["status"] = "removed";
			entry["text"] = connection;
			connections.push_back(entry);
		}
	}

	Dictionary result;
	// The nodes around the changed ones, for showing them in place: every node's type (or the
	// scene it instances) by path, the new version's first, then removed ones' old parents.
	Dictionary context;
	auto add_context = [&](const Node &p_node, const String &p_path) {
		if (!context.has(p_path)) {
			Dictionary entry;
			entry["type"] = p_node.type;
			if (!p_node.instance.is_empty()) {
				entry["instance"] = p_node.instance;
			}
			context[p_path] = entry;
		}
	};
	for (const Node &node : after.nodes) {
		add_context(node, node.path(new_root));
	}
	for (const Node &node : before.nodes) {
		const String old_path = node.path(old_root);
		add_context(node, node.parent.is_empty() || new_root.is_empty() ? old_path : vformat("%s%s", new_root, old_path.substr(old_root.length())));
	}
	result["nodes"] = nodes;
	result["context"] = context;
	result["connections"] = connections;
	result["generated_only"] = nodes.is_empty() && connections.is_empty() && p_old_text != p_new_text;
	return result;
}

} // namespace godot_git
