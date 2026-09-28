#include "editor/filesystem_colors.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_system_dock.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace {

// Rows this colored remember the color they had before, so it can be put back exactly (the main
// scene is drawn in the accent color, for example).
const char *META_PREVIOUS = "godot_git_previous_color";

template <typename T>
T *first_child_of(Node *p_parent, const char *p_class, const char *p_fallback) {
	TypedArray<Node> found = p_parent->find_children("*", p_class, true, false);
	if (found.is_empty()) {
		found = p_parent->find_children("*", p_fallback, true, false);
	}
	return found.is_empty() ? nullptr : Object::cast_to<T>(found[0]);
}

} // namespace

void GitFileSystemColors::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			_connect_dock();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_paint_tree(true);
			_paint_list(true);
			if (Tree *tree = Object::cast_to<Tree>(ObjectDB::get_instance(tree_id))) {
				tree->queue_redraw(); // Clears the letters.
			}
		} break;
	}
}

// Every lookup is guarded: these are the editor's internal controls, which may change between
// Godot versions. If they aren't found, files simply aren't colored.
void GitFileSystemColors::_connect_dock() {
	FileSystemDock *dock = EditorInterface::get_singleton()->get_file_system_dock();
	if (!dock) {
		return;
	}
	if (Tree *tree = first_child_of<Tree>(dock, "FileSystemTree", "Tree")) {
		tree_id = tree->get_instance_id();
		tree->connect("draw", callable_mp(this, &GitFileSystemColors::_on_tree_draw));
		tree->connect("item_collapsed", callable_mp(this, &GitFileSystemColors::_on_tree_item_collapsed));
	}
	if (ItemList *list = first_child_of<ItemList>(dock, "FileSystemList", "ItemList")) {
		list_id = list->get_instance_id();
		list->connect("draw", callable_mp(this, &GitFileSystemColors::_on_list_draw));
	}
}

void GitFileSystemColors::set_colors(const Dictionary &p_colors, const Dictionary &p_badges) {
	if (p_colors == colors && p_badges == badges) {
		return;
	}
	colors = p_colors;
	badges = p_badges;
	version++;
	// Right away; the draw callbacks catch the rows the dock rebuilds later.
	_paint_tree(false);
	painted_list = String();
	_on_list_draw();
	if (Tree *tree = Object::cast_to<Tree>(ObjectDB::get_instance(tree_id))) {
		tree->queue_redraw(); // For the letters.
	}
}

void GitFileSystemColors::_on_tree_draw() {
	Tree *tree = Object::cast_to<Tree>(ObjectDB::get_instance(tree_id));
	if (!tree || !tree->get_root()) {
		return;
	}
	const uint64_t root = tree->get_root()->get_instance_id();
	if (root != painted_root || version != painted_version) {
		_paint_tree(false);
	}
	_draw_badges();
}

// The status letters at the right edge of the rows, like the Git dock's. Drawn on the Tree's
// custom drawing layer, which it clears at the start of every redraw; "draw" comes right after.
void GitFileSystemColors::_draw_badges() {
	Tree *tree = Object::cast_to<Tree>(ObjectDB::get_instance(tree_id));
	if (!tree || painted_badges.is_empty() || !is_inside_tree()) {
		return;
	}
	const RID canvas = tree->get_custom_drawing_canvas_item();
	const Ref<Font> font = tree->get_theme_font("font");
	const int font_size = tree->get_theme_font_size("font_size");
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	const float slot = font->get_string_size("M", HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
	float right = tree->get_size().x - tree->get_theme_stylebox("panel")->get_margin(SIDE_RIGHT) - 4 * scale;
	for (int i = 0; i < tree->get_child_count(true); i++) {
		if (VScrollBar *bar = Object::cast_to<VScrollBar>(tree->get_child(i, true))) {
			right -= bar->is_visible() ? bar->get_size().x : 0.0f;
		}
	}
	for (const Badge &badge : painted_badges) {
		TreeItem *item = Object::cast_to<TreeItem>(ObjectDB::get_instance(badge.item));
		if (!item || !item->is_visible_in_tree()) {
			continue;
		}
		// Rows in a collapsed folder aren't drawn, and get_item_area_rect doesn't say so.
		bool shown = true;
		for (TreeItem *parent = item->get_parent(); parent; parent = parent->get_parent()) {
			shown = shown && !parent->is_collapsed();
		}
		if (!shown) {
			continue;
		}
		const Rect2 row = tree->get_item_area_rect(item, 0);
		const float x = right - slot + (slot - font->get_string_size(badge.text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x) / 2;
		const float y = row.position.y + (row.size.y - font->get_height(font_size)) / 2 + font->get_ascent(font_size);
		font->draw_string(canvas, Vector2(x, y), badge.text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, badge.color);
	}
}

// Expanding a folder can add rows without a new root.
void GitFileSystemColors::_on_tree_item_collapsed(TreeItem *p_item) {
	painted_root = 0;
}

void GitFileSystemColors::_on_list_draw() {
	ItemList *list = Object::cast_to<ItemList>(ObjectDB::get_instance(list_id));
	if (!list) {
		return;
	}
	// The list is refilled for every folder; its items' paths say whether it's the one painted.
	String items = itos(list->get_item_count());
	for (int i = 0; i < MIN(list->get_item_count(), 3); i++) {
		items += String(list->get_item_metadata(i));
	}
	if (items != painted_list || version != painted_list_version) {
		painted_list = items;
		painted_list_version = version;
		_paint_list(false);
	}
}

void GitFileSystemColors::_paint_tree(bool p_restore_only) {
	Tree *tree = Object::cast_to<Tree>(ObjectDB::get_instance(tree_id));
	if (!tree || !tree->get_root()) {
		return;
	}
	painted_root = tree->get_root()->get_instance_id();
	painted_version = version;
	painted_badges.clear();

	LocalVector<TreeItem *> stack;
	stack.push_back(tree->get_root());
	while (!stack.is_empty()) {
		TreeItem *item = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		for (TreeItem *child = item->get_first_child(); child; child = child->get_next()) {
			stack.push_back(child);
		}
		const Variant path = item->get_metadata(0);
		const bool changed = !p_restore_only && path.get_type() == Variant::STRING && colors.has(path);
		if (changed) {
			if (!item->has_meta(META_PREVIOUS)) {
				item->set_meta(META_PREVIOUS, item->get_custom_color(0));
			}
			item->set_custom_color(0, colors[path]);
			if (badges.has(path)) {
				const Array badge = badges[path];
				painted_badges.push_back({ item->get_instance_id(), badge[0], badge[1] });
			}
		} else if (item->has_meta(META_PREVIOUS)) {
			const Color previous = item->get_meta(META_PREVIOUS);
			item->remove_meta(META_PREVIOUS);
			if (previous == Color()) {
				item->clear_custom_color(0);
			} else {
				item->set_custom_color(0, previous);
			}
		}
	}
}

void GitFileSystemColors::_paint_list(bool p_restore_only) {
	ItemList *list = Object::cast_to<ItemList>(ObjectDB::get_instance(list_id));
	if (!list) {
		return;
	}
	// The dock never sets text colors on this list itself, so "no color" is always the original.
	for (int i = 0; i < list->get_item_count(); i++) {
		const Variant path = list->get_item_metadata(i);
		const bool changed = !p_restore_only && path.get_type() == Variant::STRING && colors.has(path);
		const Color color = changed ? Color(colors[path]) : Color();
		if (list->get_item_custom_fg_color(i) != color) {
			list->set_item_custom_fg_color(i, color);
		}
	}
}
