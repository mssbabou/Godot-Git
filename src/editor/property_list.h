#pragma once

#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/tree.hpp>

using namespace godot;

// A list of changed properties drawn like Godot's Inspector, for everything the Diff panel shows
// setting by setting: `.import`, `.uid`, `project.godot` and `export_presets.cfg` files, the
// companions under a file, and a scene's nodes. One look for all of them:
// - a title row for what the properties belong to (a node or a file: icon, name, what it is, and
//   a status pill);
// - section rows in the Inspector's category style ("Compress", "Transform");
// - property rows: the name as the Inspector shows it, the value as it is now, and under it, dim,
//   what it was ("was 22.75, 0, 19.5"). Vectors get x, y, z labels in the Inspector's axis colors,
//   colors a swatch, booleans On / Off. A property that's only new or only gone is tagged so.
// Nothing in it can be selected or clicked.
class GitPropertyList : public Tree {
	GDCLASS(GitPropertyList, Tree)

	struct ThemeCache {
		Ref<Font> font;
		Ref<Font> bold;
		int font_size = 0;
		Color text;
		Color name;
		Color dim;
		Color added;
		Color removed;
		Color axis[4];
		Ref<StyleBox> section;
		float scale = 1;
		float line = 0; // A line's height.
	} theme_cache;

	float name_width = 0; // The widest property name, so the values line up.

	void _update_theme_cache();
	TreeItem *_add_row(const String &p_kind, const Dictionary &p_data, int p_lines);
	void _draw_row(TreeItem *p_item, const Rect2 &p_rect);
	float _draw_value(const String &p_value, float p_x, float p_baseline, float p_right, const Color &p_color, bool p_axes);

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	void clear_list();
	// p_status: "Changed", "Added", ...; drawn as a pill in p_status_color. Both may be empty.
	void add_title(const Ref<Texture2D> &p_icon, const String &p_name, const String &p_detail, const String &p_status, const Color &p_status_color);
	void add_section(const String &p_title);
	// A property changed from p_old to p_new (as displayed); p_has_old / p_has_new false when it
	// didn't exist on that side.
	void add_property(const String &p_name, const String &p_old, bool p_has_old, const String &p_new, bool p_has_new, const String &p_tooltip = String());
	void add_note(const String &p_text, const String &p_tooltip = String());

	GitPropertyList();
};
