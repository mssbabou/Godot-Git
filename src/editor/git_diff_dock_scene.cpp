// The Diff panel's scene view, laid out like Godot's Scene dock and Inspector: on the left the
// scene as a tree, the changed nodes in their status colors with their letter, under their
// (dimmed) parents; on the right what changed on the selected node, setting by setting
// ("shape › size   (32, 32) → (40, 32)"). The changes come from GitRepository::get_diff ("scene",
// see scene_changes in scene_text.cpp).

#include "editor/git_diff_dock.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/h_split_container.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include "editor/git_colors.h"
#include "editor/property_list.h"
#include "editor/ui_text.h"

using namespace godot_git;

namespace {

enum NodeColumn {
	NODE_COLUMN_NAME,
	NODE_COLUMN_LETTER,
	NODE_COLUMN_COUNT,
};

const char *CONNECTIONS_ROW = "#connections";

void cell(TreeItem *p_item, int p_column, const String &p_text, const Color &p_color) {
	p_item->set_text(p_column, p_text);
	p_item->set_custom_color(p_column, p_color);
	p_item->set_text_overrun_behavior(p_column, TextServer::OVERRUN_NO_TRIMMING); // Gotcha 4.
}

// "Level/Containers/Chair7" -> "Containers › Chair7".
String display_path(const String &p_path) {
	const int slash = p_path.find("/");
	return slash < 0 ? p_path : p_path.substr(slash + 1).replace("/", String::utf8(" › "));
}

// The dock's state name for a node's status, for its color and letter.
String node_state(const String &p_status) {
	if (p_status == "added") {
		return "new";
	}
	if (p_status == "removed") {
		return "deleted";
	}
	if (p_status == "renamed" || p_status == "moved") {
		return "renamed";
	}
	return "modified";
}

Ref<Texture2D> type_icon(const String &p_type, const String &p_instance) {
	const Ref<Theme> theme = EditorInterface::get_singleton()->get_editor_theme();
	const String name = !p_instance.is_empty() ? String("PackedScene") : p_type;
	return theme->has_icon(name, "EditorIcons") ? theme->get_icon(name, "EditorIcons") : theme->get_icon("Node", "EditorIcons");
}

} // namespace

void GitDiffDock::_make_scene_view(Control *p_parent) {
	HSplitContainer *split = memnew(HSplitContainer);
	split->set_v_size_flags(SIZE_EXPAND_FILL);
	p_parent->add_child(split);
	scene_view = split;

	// Left: the scene's nodes, like the Scene dock.
	PanelContainer *nodes_frame = memnew(PanelContainer);
	nodes_frame->set_custom_minimum_size(Vector2(220, 0) * EditorInterface::get_singleton()->get_editor_scale());
	split->add_child(nodes_frame);
	scene_tree = memnew(Tree);
	scene_tree->set_columns(NODE_COLUMN_COUNT);
	scene_tree->set_column_expand(NODE_COLUMN_NAME, true);
	scene_tree->set_column_expand(NODE_COLUMN_LETTER, false);
	scene_tree->set_hide_root(true);
	scene_tree->set_select_mode(Tree::SELECT_ROW);
	scene_tree->connect("item_selected", callable_mp(this, &GitDiffDock::_on_scene_node_selected));
	nodes_frame->add_child(scene_tree);

	// Right: what changed on the selected node, like the Inspector (the same list the settings
	// views use).
	PanelContainer *properties_frame = memnew(PanelContainer);
	properties_frame->set_h_size_flags(SIZE_EXPAND_FILL);
	split->add_child(properties_frame);
	scene_properties = memnew(GitPropertyList);
	scene_properties->set_v_size_flags(SIZE_EXPAND_FILL);
	properties_frame->add_child(scene_properties);
}

void GitDiffDock::_show_scene() {
	const String keep = scene_selected;
	scene_tree->clear();
	scene_properties->clear_list();
	if (_current_view() != VIEW_SCENE) {
		return;
	}
	const Dictionary scene = diff.get("scene", Dictionary());
	const Array nodes = scene.get("nodes", Array());
	const Array connections = scene.get("connections", Array());
	const Dictionary context = scene.get("context", Dictionary());
	const Color font_color = get_theme_color("font_color", "Tree");

	// The changed nodes, and every node above one of them (shown dimmed, so a change is in place).
	Dictionary changed;
	Dictionary needed;
	for (int i = 0; i < nodes.size(); i++) {
		const Dictionary node = nodes[i];
		const String path = node.get("path", String());
		changed[path] = node;
		const PackedStringArray parts = path.split("/");
		String prefix;
		for (int p = 0; p < parts.size(); p++) {
			prefix = p == 0 ? parts[0] : vformat("%s/%s", prefix, parts[p]);
			needed[prefix] = true;
		}
	}

	// In the scene's own order: context lists every node, the new version's first.
	TreeItem *root = scene_tree->create_item();
	Dictionary items;
	PackedStringArray order;
	const Array context_paths = context.keys();
	for (int i = 0; i < context_paths.size(); i++) {
		if (needed.has(context_paths[i])) {
			order.push_back(context_paths[i]);
		}
	}
	for (int i = 0; i < nodes.size(); i++) {
		const String path = Dictionary(nodes[i]).get("path", String());
		if (!order.has(path)) {
			order.push_back(path); // A `.tres` file's own properties (path "").
		}
	}
	TreeItem *first_changed = nullptr;
	TreeItem *kept = nullptr;
	for (const String &path : order) {
		const String parent_path = path.contains("/") ? path.get_base_dir() : String();
		TreeItem *parent = items.has(parent_path) ? Object::cast_to<TreeItem>(items[parent_path]) : root;
		TreeItem *item = scene_tree->create_item(parent);
		items[path] = item;
		item->set_metadata(0, path);
		const Dictionary info = context.get(path, Dictionary());
		const Dictionary node = changed.get(path, Dictionary());
		const String type = node.is_empty() ? String(info.get("type", String())) : String(node.get("type", String()));
		const String instance = node.is_empty() ? String(info.get("instance", String())) : String(node.get("instance", String()));
		const String name = path.is_empty() ? (type.is_empty() ? String("Resource") : type) : path.get_file();
		item->set_icon(NODE_COLUMN_NAME, type_icon(type, instance));
		const String kind = instance.is_empty() ? type : vformat("Instance of %s", instance.get_file());
		if (node.is_empty()) {
			cell(item, NODE_COLUMN_NAME, name, theme.dim);
			item->set_tooltip_text(NODE_COLUMN_NAME, vformat("%s\n%s\nUnchanged (shown for where the changes are).", path, kind));
			continue;
		}
		const String status = node.get("status", String());
		const String state = node_state(status);
		cell(item, NODE_COLUMN_NAME, name, status_color(state));
		cell(item, NODE_COLUMN_LETTER, status_letter(state), status_color(state));
		item->set_text_alignment(NODE_COLUMN_LETTER, HORIZONTAL_ALIGNMENT_RIGHT);
		String detail = status.capitalize();
		if (status == "renamed") {
			detail = vformat("Renamed from %s", String(node.get("old_path", String())).get_file());
		} else if (status == "moved") {
			detail = vformat("Moved from %s", display_path(node.get("old_path", String())));
		}
		item->set_tooltip_text(NODE_COLUMN_NAME, path.is_empty() ? kind : vformat("%s\n%s\n%s", path, kind, detail));
		if (!first_changed) {
			first_changed = item;
		}
		if (path == keep) {
			kept = item;
		}
	}

	if (!connections.is_empty()) {
		TreeItem *item = scene_tree->create_item(root);
		item->set_metadata(0, CONNECTIONS_ROW);
		cell(item, NODE_COLUMN_NAME, "Connections", status_color("modified"));
		cell(item, NODE_COLUMN_LETTER, status_letter("modified"), status_color("modified"));
		item->set_text_alignment(NODE_COLUMN_LETTER, HORIZONTAL_ALIGNMENT_RIGHT);
		item->set_icon(NODE_COLUMN_NAME, EditorInterface::get_singleton()->get_editor_theme()->get_icon("Signals", "EditorIcons"));
		item->set_tooltip_text(NODE_COLUMN_NAME, plural(connections.size(), "signal connection changed", "signal connections changed"));
		if (keep == CONNECTIONS_ROW) {
			kept = item;
		}
		if (!first_changed) {
			first_changed = item;
		}
	}

	if (!first_changed) {
		TreeItem *item = scene_tree->create_item(root);
		cell(item, NODE_COLUMN_NAME, "Nothing about the nodes changed.", theme.dim);
		item->set_selectable(NODE_COLUMN_NAME, false);
		item->set_selectable(NODE_COLUMN_LETTER, false);
		scene_properties->add_note("Only what Godot rewrites on its own changed.", "Resource ids, load_steps, or numbers that differ only in their last digits.");
		scene_selected = String();
		return;
	}
	TreeItem *selected = kept ? kept : first_changed;
	selected->select(0);
	scene_tree->scroll_to_item(selected);
	_on_scene_node_selected();
}

// The selected node's changes in the list on the right: a title row (the node, what it is, what
// happened to it), then its properties in sections like the Inspector's.
void GitDiffDock::_on_scene_node_selected() {
	scene_properties->clear_list();
	TreeItem *selected = scene_tree->get_selected();
	if (!selected) {
		return;
	}
	scene_selected = selected->get_metadata(0);
	const Dictionary scene = diff.get("scene", Dictionary());
	const Ref<Theme> editor_theme = EditorInterface::get_singleton()->get_editor_theme();

	if (scene_selected == CONNECTIONS_ROW) {
		const Array connections = scene.get("connections", Array());
		scene_properties->add_title(editor_theme->get_icon("Signals", "EditorIcons"), "Connections", String(), "Changed", status_color("modified"));
		for (int c = 0; c < connections.size(); c++) {
			const Dictionary connection = connections[c];
			const bool added = String(connection.get("status", String())) == "added";
			// "body_entered: Area → Player._on_body_entered()": the signal, then where it goes.
			const String text = connection.get("text", String());
			const int colon = text.find(": ");
			const String signal = colon < 0 ? text : text.left(colon);
			const String target = colon < 0 ? String() : text.substr(colon + 2);
			scene_properties->add_property(signal, target, !added, target, added, text);
		}
		return;
	}

	Dictionary node;
	const Array nodes = scene.get("nodes", Array());
	for (int i = 0; i < nodes.size(); i++) {
		if (String(Dictionary(nodes[i]).get("path", String())) == scene_selected) {
			node = nodes[i];
			break;
		}
	}
	const Dictionary info = Dictionary(scene.get("context", Dictionary())).get(scene_selected, Dictionary());
	const String type = node.is_empty() ? String(info.get("type", String())) : String(node.get("type", String()));
	const String instance = node.is_empty() ? String(info.get("instance", String())) : String(node.get("instance", String()));
	const String kind = instance.is_empty() ? type : vformat("Instance of %s", instance.get_file());
	const String name = selected->get_text(NODE_COLUMN_NAME);
	if (node.is_empty()) {
		scene_properties->add_title(type_icon(type, instance), name, kind, "Unchanged", theme.dim);
		scene_properties->add_note("Shown for where the changes below it are.");
		return;
	}
	const String status = node.get("status", String());
	String what = "Changed";
	if (status == "added") {
		what = "Added";
	} else if (status == "removed") {
		what = "Removed";
	} else if (status == "renamed") {
		what = vformat("Renamed from %s", String(node.get("old_path", String())).get_file());
	} else if (status == "moved") {
		what = vformat("Moved from %s", display_path(node.get("old_path", String())));
	}
	scene_properties->add_title(type_icon(type, instance), name, kind, what, status_color(node_state(status)));

	// In sections like the Inspector's: "transform › position" is Position under Transform, a
	// sub-resource's settings are under its property ("shape › size"), the node's own facts (type,
	// groups, editable children) under Node, and the rest under its type.
	const String separator = String::utf8(" › ");
	PackedStringArray sections;
	Dictionary rows; // Section -> Array of properties.
	const Array properties = node.get("properties", Array());
	for (int p = 0; p < properties.size(); p++) {
		Dictionary property = Dictionary(properties[p]).duplicate();
		const String full = property.get("name", String());
		const int cut = full.find(separator);
		String section;
		String shown;
		if (cut >= 0) {
			section = full.left(cut).capitalize();
			PackedStringArray rest = full.substr(cut + separator.length()).split(separator);
			for (int r = 0; r < rest.size(); r++) {
				rest.set(r, rest[r].capitalize());
			}
			shown = separator.join(rest);
		} else if (full == "Type" || full == "Groups" || full == "Instance of" || full == "Editable Children") {
			section = "Node";
			shown = full;
		} else {
			section = type.is_empty() ? String("Properties") : type;
			shown = full.capitalize();
		}
		property["shown"] = shown;
		if (!rows.has(section)) {
			sections.push_back(section);
			rows[section] = Array();
		}
		Array(rows[section]).push_back(property);
	}
	for (const String &section : sections) {
		scene_properties->add_section(section);
		const Array list = rows[section];
		for (int p = 0; p < list.size(); p++) {
			const Dictionary property = list[p];
			scene_properties->add_property(property["shown"], property.get("old", String()), property.has("old"), property.get("new", String()), property.has("new"), vformat("%s\n%s", property.get("name", String()), property.get(property.has("new") ? "new" : "old", String())));
		}
	}
	if (properties.is_empty()) {
		scene_properties->add_note(status == "removed" ? String("Removed with its children.") : String("No settings changed."));
	}
}
