#include "scene/scene_merge.h"

#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "scene/scene_file.h"

namespace godot_git {

namespace {

// Where a section belongs in the file. Godot writes them in this order, and a loader reads a
// resource only after the ones it refers to, so a section taken from theirs stays in its group.
enum Group {
	GROUP_HEADER,
	GROUP_EXT,
	GROUP_SUB,
	GROUP_MAIN, // [node] or a .tres file's [resource].
	GROUP_CONNECTION,
	GROUP_EDITABLE,
	GROUP_OTHER,
};

Group group_of(const String &p_tag) {
	if (p_tag == "gd_scene" || p_tag == "gd_resource") {
		return GROUP_HEADER;
	}
	if (p_tag == "ext_resource") {
		return GROUP_EXT;
	}
	if (p_tag == "sub_resource") {
		return GROUP_SUB;
	}
	if (p_tag == "node" || p_tag == "resource") {
		return GROUP_MAIN;
	}
	if (p_tag == "connection") {
		return GROUP_CONNECTION;
	}
	if (p_tag == "editable") {
		return GROUP_EDITABLE;
	}
	return GROUP_OTHER;
}

// A node's path as other sections name it: "." for the root, "Player", "Player/Sprite".
String node_path(const SceneSection &p_node) {
	if (!p_node.fields.has("parent")) {
		return ".";
	}
	const String parent = p_node.field("parent");
	return parent == "." ? p_node.field("name") : vformat("%s/%s", parent, p_node.field("name"));
}

// What a section is, the same in every version of the file.
String key_of(const SceneSection &p_section) {
	const String &tag = p_section.tag;
	switch (group_of(tag)) {
		case GROUP_HEADER:
			return "header";
		case GROUP_EXT:
			return vformat("ext:%s", p_section.field("path"));
		case GROUP_SUB:
			return vformat("sub:%s", p_section.field("id"));
		case GROUP_MAIN:
			if (tag == "resource") {
				return "resource";
			}
			// A scene has one root, whatever its id: the same file added on both sides then
			// compares root to root instead of ending up with two.
			if (!p_section.fields.has("parent")) {
				return "root";
			}
			// unique_id is a bare number (Godot 4.6+); older files have none.
			return p_section.fields.has("unique_id") ? vformat("node:%s", p_section.fields["unique_id"]) : vformat("path:%s", node_path(p_section));
		case GROUP_CONNECTION:
			return vformat("connection:%s|%s|%s|%s", p_section.field("signal"), p_section.field("from"), p_section.field("to"), p_section.field("method"));
		case GROUP_EDITABLE:
			return vformat("editable:%s", p_section.field("path"));
		default:
			return vformat("other:%s", p_section.text);
	}
}

// p_text with every Kind("id") whose id is in p_map replaced by Kind("<mapped>").
String map_references(const String &p_text, const String &p_kind, const HashMap<String, String> &p_map) {
	const String opener = vformat("%s(\"", p_kind);
	if (p_map.is_empty() || !p_text.contains(opener)) {
		return p_text;
	}
	String result;
	int at = 0;
	while (true) {
		const int start = p_text.find(opener, at);
		const int id_start = start + opener.length();
		const int id_end = start < 0 ? -1 : p_text.find("\")", id_start);
		if (id_end < 0) {
			result += p_text.substr(at);
			return result;
		}
		const String id = p_text.substr(id_start, id_end - id_start);
		const String *mapped = p_map.getptr(id);
		result += p_text.substr(at, id_start - at);
		result += mapped ? *mapped : id;
		at = id_end;
	}
}

// Every Kind("id") id p_text refers to.
void collect_references(const String &p_text, const String &p_kind, HashSet<String> &r_ids) {
	const String opener = vformat("%s(\"", p_kind);
	int at = 0;
	while (true) {
		const int start = p_text.find(opener, at);
		if (start < 0) {
			return;
		}
		const int id_start = start + opener.length();
		const int id_end = p_text.find("\")", id_start);
		if (id_end < 0) {
			return;
		}
		r_ids.insert(p_text.substr(id_start, id_end - id_start));
		at = id_end;
	}
}

// One version of the file, with its sections findable by key.
struct Version {
	SceneFile file;
	HashMap<String, int> by_key;
	HashMap<String, String> ext_by_path; // ExtResource path -> its id here.
	HashMap<String, String> ext_as_path; // ExtResource id -> "@path", for comparing.
	String duplicate; // A key two sections have (then this can't be merged by section).

	explicit Version(const String &p_text) {
		file = read_scene_file(p_text);
		for (uint32_t s = 0; s < file.sections.size(); s++) {
			const SceneSection &section = file.sections[s];
			const String key = key_of(section);
			if (by_key.has(key)) {
				duplicate = key;
			}
			by_key[key] = s;
			if (section.tag == "ext_resource") {
				ext_by_path[section.field("path")] = section.field("id");
				ext_as_path[section.field("id")] = vformat("@%s", section.field("path"));
			}
		}
	}

	const SceneSection *find(const String &p_key) const {
		const int *index = by_key.getptr(p_key);
		return index ? &file.sections[*index] : nullptr;
	}

	// The section as it compares across versions: resource ids Godot renumbers on save don't
	// count, nor the header's load_steps (recounted when the file is written).
	String normalized(const String &p_key) const {
		const SceneSection *section = find(p_key);
		if (!section) {
			return String();
		}
		if (section->tag == "ext_resource") {
			return vformat("%s|%s|%s", section->field("type"), section->field("uid"), section->field("path"));
		}
		if (group_of(section->tag) == GROUP_HEADER) {
			PackedStringArray fields;
			for (const KeyValue<String, String> &field : section->fields) {
				if (field.key != "load_steps") {
					fields.push_back(vformat("%s=%s", field.key, field.value));
				}
			}
			fields.sort();
			return vformat("%s %s", section->tag, String(" ").join(fields));
		}
		return map_references(section->text, "ExtResource", ext_as_path);
	}
};

enum Choice {
	CHOICE_MINE,
	CHOICE_THEIRS,
	CHOICE_DROP,
	CHOICE_CONFLICT,
};

struct Item {
	String key;
	String tag;
	Choice choice;
	int mine_index; // Where mine has it, or -1.
};

int find_item(const LocalVector<Item> &p_items, const String &p_key) {
	for (uint32_t i = 0; i < p_items.size(); i++) {
		if (p_items[i].key == p_key) {
			return i;
		}
	}
	return -1;
}

// A new id for theirs' resource at p_path where mine uses its id for another one, in Godot's
// form ("12_ab3cd"); taken from the path, so the same merge gives the same file.
String new_ext_id(const String &p_path, const HashSet<String> &p_taken) {
	int highest = 0;
	for (const String &id : p_taken) {
		highest = MAX(highest, (int)id.get_slice("_", 0).to_int());
	}
	const char *digits = "abcdefghijklmnopqrstuvwxyz0123456789";
	uint32_t hash = p_path.hash();
	for (int attempt = 0;; attempt++) {
		String suffix;
		uint32_t value = hash + attempt;
		for (int i = 0; i < 5; i++) {
			suffix += String::chr(digits[value % 36]);
			value /= 36;
		}
		const String id = vformat("%d_%s", highest + 1, suffix);
		if (!p_taken.has(id)) {
			return id;
		}
	}
}

// Why the merged file would be broken, or "": every node's parent comes before it, no two nodes
// share a path, every resource it refers to is in it, every connection's nodes exist. Paths into
// an instanced scene (its own nodes aren't in this file) count when the instance is here.
String check_merged(const String &p_text) {
	const SceneFile file = read_scene_file(p_text);
	HashSet<String> paths;
	HashSet<String> instances;
	HashSet<String> ext_ids;
	HashSet<String> sub_ids;
	HashSet<String> ext_used;
	HashSet<String> sub_used;
	auto exists = [&](const String &p_path) {
		if (paths.has(p_path)) {
			return true;
		}
		// Inside an instanced scene: some node above it is an instance in this file.
		String prefix = p_path;
		while (prefix.contains("/")) {
			prefix = prefix.get_base_dir();
			if (instances.has(prefix)) {
				return paths.has(prefix);
			}
		}
		return false;
	};
	for (const SceneSection &section : file.sections) {
		collect_references(section.text, "ExtResource", ext_used);
		collect_references(section.text, "SubResource", sub_used);
		if (section.tag == "ext_resource") {
			ext_ids.insert(section.field("id"));
		} else if (section.tag == "sub_resource") {
			sub_ids.insert(section.field("id"));
		} else if (section.tag == "node") {
			const String path = node_path(section);
			if (paths.has(path)) {
				return vformat("Two nodes would be at %s.", path);
			}
			if (path != "." && !paths.has(".")) {
				return vformat("%s would come before the scene's root.", path);
			}
			const String parent = section.fields.has("parent") ? section.field("parent") : String();
			if (!parent.is_empty() && parent != "." && !exists(parent)) {
				return vformat("%s's parent, %s, wouldn't be in the scene.", path, parent);
			}
			paths.insert(path);
			if (section.fields.has("instance")) {
				instances.insert(path);
			}
		} else if (section.tag == "connection") {
			for (const char *end : { "from", "to" }) {
				const String path = section.field(end);
				if (path != "." && !exists(path)) {
					return vformat("A connection would refer to %s, which wouldn't be in the scene.", path);
				}
			}
		}
	}
	for (const String &id : ext_used) {
		if (!ext_ids.has(id)) {
			return vformat("It would refer to a resource (ExtResource \"%s\") that wouldn't be in it.", id);
		}
	}
	for (const String &id : sub_used) {
		if (!sub_ids.has(id)) {
			return vformat("It would refer to a resource (SubResource \"%s\") that wouldn't be in it.", id);
		}
	}
	return String();
}

Dictionary refuse(const String &p_reason) {
	Dictionary result;
	result["ok"] = false;
	result["reason"] = p_reason;
	return result;
}

} // namespace

Dictionary merge_scene(const String &p_base, const String &p_mine, const String &p_theirs) {
	const Version base(p_base);
	const Version mine(p_mine);
	const Version theirs(p_theirs);
	for (const Version *version : { &mine, &theirs }) {
		if (version->file.sections.is_empty() || group_of(version->file.sections[0].tag) != GROUP_HEADER) {
			return refuse("It isn't a scene or resource file.");
		}
	}
	// Godot raises format= when sections start working differently (4.7 writes 4). A newer file
	// may refer between sections in ways the check above doesn't know, so it's left to git's line
	// merge until this is updated for it. The weekly CI job against Godot's newest preview is where
	// a new format first shows.
	constexpr int NEWEST_FORMAT = 4;
	for (const Version *version : { &base, &mine, &theirs }) {
		if (!version->file.sections.is_empty() && version->file.sections[0].field("format").to_int() > NEWEST_FORMAT) {
			return refuse(vformat("It was saved by a newer Godot (format %s), which this merge doesn't know yet.", version->file.sections[0].field("format")));
		}
	}
	for (const Version *version : { &base, &mine, &theirs }) {
		if (!version->duplicate.is_empty()) {
			return refuse(vformat("Two sections are the same thing (%s).", version->duplicate));
		}
	}

	// What to take for each section, by comparing the three versions.
	HashMap<String, Choice> choices;
	int from_mine = 0;
	int from_theirs = 0;
	auto decide = [&](const String &p_key) {
		if (choices.has(p_key)) {
			return;
		}
		const bool in_base = base.by_key.has(p_key);
		const bool in_mine = mine.by_key.has(p_key);
		const bool in_theirs = theirs.by_key.has(p_key);
		const String b = base.normalized(p_key);
		const String m = mine.normalized(p_key);
		const String t = theirs.normalized(p_key);
		Choice choice;
		if (in_mine && in_theirs) {
			if (m == t) {
				choice = CHOICE_MINE;
				from_mine += (in_base && m == b) ? 0 : 1;
			} else if (in_base && m == b) {
				choice = CHOICE_THEIRS;
				from_theirs++;
			} else if (in_base && t == b) {
				choice = CHOICE_MINE;
				from_mine++;
			} else {
				choice = CHOICE_CONFLICT;
			}
		} else if (in_mine) {
			// Theirs doesn't have it: mine added it, or theirs removed it.
			if (!in_base) {
				choice = CHOICE_MINE;
				from_mine++;
			} else if (m == b) {
				choice = CHOICE_DROP;
				from_theirs++;
			} else {
				choice = CHOICE_CONFLICT; // Mine changed what theirs removed.
			}
		} else if (in_theirs) {
			if (!in_base) {
				choice = CHOICE_THEIRS;
				from_theirs++;
			} else if (t == b) {
				choice = CHOICE_DROP;
				from_mine++;
			} else {
				choice = CHOICE_CONFLICT;
			}
		} else {
			choice = CHOICE_DROP; // Both removed it.
		}
		choices[p_key] = choice;
	};
	for (const Version *version : { &mine, &theirs }) {
		for (const SceneSection &section : version->file.sections) {
			decide(key_of(section));
		}
	}

	// In mine's order; what only theirs has goes after the section before it in theirs (in the
	// same group), past what mine added there, so additions on both sides end up mine first.
	LocalVector<Item> items;
	for (uint32_t m = 0; m < mine.file.sections.size(); m++) {
		const SceneSection &section = mine.file.sections[m];
		const String key = key_of(section);
		if (choices[key] != CHOICE_DROP) {
			items.push_back({ key, section.tag, choices[key], (int)m });
		}
	}
	for (uint32_t s = 0; s < theirs.file.sections.size(); s++) {
		const SceneSection &section = theirs.file.sections[s];
		const String key = key_of(section);
		if (mine.by_key.has(key) || choices[key] == CHOICE_DROP) {
			continue;
		}
		const Group group = group_of(section.tag);
		int at = -1;
		for (int p = (int)s - 1; p >= 0 && at < 0; p--) {
			const SceneSection &before = theirs.file.sections[p];
			if (group_of(before.tag) == group) {
				const int index = find_item(items, key_of(before));
				at = index < 0 ? -1 : index + 1;
			}
		}
		if (at >= 0) {
			while (at < (int)items.size() && group_of(items[at].tag) == group && !theirs.by_key.has(items[at].key)) {
				at++;
			}
		} else {
			at = items.size();
			for (uint32_t i = 0; i < items.size(); i++) {
				if (group_of(items[i].tag) >= group) {
					at = i;
					break;
				}
			}
		}
		items.insert(at, { key, section.tag, choices[key], -1 });
	}

	// Theirs' resource ids as the merged file has them: mine's id for a resource both have, theirs'
	// own otherwise, unless mine uses that id for something else.
	HashSet<String> taken;
	for (const KeyValue<String, String> &ext : mine.ext_by_path) {
		taken.insert(ext.value);
	}
	HashMap<String, String> theirs_ids; // Theirs' id -> the merged file's, where they differ.
	for (const KeyValue<String, String> &ext : theirs.ext_by_path) {
		const String *mine_id = mine.ext_by_path.getptr(ext.key);
		String id = mine_id ? *mine_id : ext.value;
		if (!mine_id && taken.has(id)) {
			id = new_ext_id(ext.key, taken);
		}
		taken.insert(id);
		if (id != ext.value) {
			theirs_ids[ext.value] = id;
		}
	}
	auto theirs_text = [&](const String &p_key) {
		const SceneSection *section = theirs.find(p_key);
		if (!section) {
			return String();
		}
		String text = map_references(section->text, "ExtResource", theirs_ids);
		const String *id = section->tag == "ext_resource" ? theirs_ids.getptr(section->field("id")) : nullptr;
		if (id) {
			text = text.replace(vformat("id=\"%s\"", section->field("id")), vformat("id=\"%s\"", *id));
		}
		return text;
	};
	auto text_of = [&](const Version &p_version, const String &p_key) {
		const SceneSection *section = p_version.find(p_key);
		return section ? section->text : String();
	};

	// The header's load_steps (only in older files) counts the resources plus the scene.
	int resources = 1;
	for (const Item &item : items) {
		resources += (item.tag == "ext_resource" || item.tag == "sub_resource") ? 1 : 0;
	}
	auto with_load_steps = [&](const String &p_header) {
		const int at = p_header.find("load_steps=");
		if (at < 0) {
			return p_header;
		}
		int end = at + 11;
		while (end < p_header.length() && p_header[end] >= '0' && p_header[end] <= '9') {
			end++;
		}
		return vformat("%sload_steps=%d%s", p_header.left(at), resources, p_header.substr(end));
	};

	// Each section's lines end with a line break, and a conflict holds whole sections, so its
	// sides are whole lines too.
	Array segments;
	String plain = mine.file.preamble;
	int conflicts = 0;
	for (uint32_t i = 0; i < items.size(); i++) {
		const Item &item = items[i];
		if (i > 0) {
			// Neighbors in mine keep mine's spacing (not every file was written by Godot); new
			// neighbors are spaced the way Godot's writer does (scene_file.cpp).
			const Item &previous = items[i - 1];
			if (previous.mine_index >= 0 && item.mine_index == previous.mine_index + 1) {
				plain += mine.file.sections[previous.mine_index].gap.substr(1);
			} else if (scene_blank_line_between(previous.tag, item.tag)) {
				plain += "\n";
			}
		}
		if (item.choice == CHOICE_CONFLICT) {
			segments.push_back(plain);
			plain = String();
			Dictionary block;
			const String base_text = text_of(base, item.key);
			const String mine_text = text_of(mine, item.key);
			const String their_text = theirs_text(item.key);
			block["base"] = base_text.is_empty() ? String() : vformat("%s\n", base_text);
			block["mine"] = mine_text.is_empty() ? String() : vformat("%s\n", mine_text);
			block["theirs"] = their_text.is_empty() ? String() : vformat("%s\n", their_text);
			segments.push_back(block);
			conflicts++;
			continue;
		}
		String text = item.choice == CHOICE_MINE ? text_of(mine, item.key) : theirs_text(item.key);
		if (group_of(item.tag) == GROUP_HEADER) {
			text = with_load_steps(text);
		}
		plain += vformat("%s\n", text);
	}
	// After the last section, what mine had there (its line break is already written).
	const String &ending = mine.file.ending;
	if (ending.is_empty()) {
		if (plain.ends_with("\n")) {
			plain = plain.left(plain.length() - 1);
		}
	} else {
		plain += ending.substr(1);
	}
	if (!plain.is_empty() || segments.is_empty()) {
		segments.push_back(plain);
	}
	if (mine.file.crlf) {
		for (int s = 0; s < segments.size(); s++) {
			if (segments[s].get_type() == Variant::STRING) {
				segments[s] = String(segments[s]).replace("\n", "\r\n");
			} else {
				Dictionary block = segments[s];
				for (const char *side : { "base", "mine", "theirs" }) {
					block[side] = String(block[side]).replace("\n", "\r\n");
				}
			}
		}
	}

	Dictionary result;
	result["ok"] = true;
	result["segments"] = segments;
	result["conflicts"] = conflicts;
	result["from_mine"] = from_mine;
	result["from_theirs"] = from_theirs;
	if (conflicts == 0) {
		String text;
		for (int s = 0; s < segments.size(); s++) {
			text += String(segments[s]);
		}
		const String broken = check_merged(text);
		if (!broken.is_empty()) {
			return refuse(broken);
		}
		result["text"] = text;
	}
	return result;
}

} // namespace godot_git
