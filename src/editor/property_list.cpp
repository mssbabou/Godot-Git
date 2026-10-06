#include "editor/property_list.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/text_line.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace {

// "(22.6, 0, 9.9)" or "Vector3(22.6, 0, 9.9)" -> its numbers, for x/y/z labels; empty for
// anything else.
PackedStringArray vector_parts(const String &p_value) {
	const int open = p_value.find("(");
	if (open < 0 || !p_value.ends_with(")")) {
		return PackedStringArray();
	}
	const String prefix = p_value.left(open);
	if (!prefix.is_empty() && !prefix.begins_with("Vector")) {
		return PackedStringArray();
	}
	const PackedStringArray parts = p_value.substr(open + 1, p_value.length() - open - 2).split(",");
	if (parts.size() < 2 || parts.size() > 4) {
		return PackedStringArray();
	}
	PackedStringArray numbers;
	for (const String &part : parts) {
		const String number = part.strip_edges();
		if (!number.is_valid_float()) {
			return PackedStringArray();
		}
		numbers.push_back(number);
	}
	return numbers;
}

// A value as the Inspector words it: On / Off for booleans.
String shown(const String &p_value) {
	if (p_value == "true") {
		return "On";
	}
	if (p_value == "false") {
		return "Off";
	}
	return p_value;
}

// For "was ...": a vector's numbers without their brackets.
String plain(const String &p_value) {
	const PackedStringArray numbers = vector_parts(p_value);
	return numbers.is_empty() ? shown(p_value) : String(", ").join(numbers);
}

} // namespace

GitPropertyList::GitPropertyList() {
	set_columns(1);
	set_hide_root(true);
	set_hide_folding(true);
	set_focus_mode(FOCUS_NONE);
	set_select_mode(SELECT_ROW);
	create_item();
	// Read-only: no hover or selection boxes (gotcha 47). Set here, not on THEME_CHANGED: an
	// override sends THEME_CHANGED again, which recursed until the stack ran out.
	Ref<StyleBoxEmpty> empty;
	empty.instantiate();
	for (const char *name : { "panel", "focus", "hovered", "hovered_dimmed", "hovered_selected", "hovered_selected_focus", "selected", "selected_focus", "cursor", "cursor_unfocused" }) {
		add_theme_stylebox_override(name, empty);
	}
}

void GitPropertyList::_notification(int p_what) {
	if ((p_what == NOTIFICATION_THEME_CHANGED || p_what == NOTIFICATION_READY) && is_inside_tree()) {
		_update_theme_cache();
	}
}

void GitPropertyList::_update_theme_cache() {
	const Ref<Theme> editor = EditorInterface::get_singleton()->get_editor_theme();
	theme_cache.scale = EditorInterface::get_singleton()->get_editor_scale();
	theme_cache.font = get_theme_font("font");
	theme_cache.bold = editor->has_font("bold", "EditorFonts") ? editor->get_font("bold", "EditorFonts") : theme_cache.font;
	theme_cache.font_size = get_theme_font_size("font_size");
	theme_cache.text = get_theme_color("font_color");
	theme_cache.name = theme_cache.text * Color(1, 1, 1, 0.8);
	theme_cache.dim = theme_cache.text * Color(1, 1, 1, 0.5);
	theme_cache.added = editor->get_color("success_color", "Editor");
	theme_cache.removed = editor->get_color("error_color", "Editor");
	const char *axes[4] = { "property_color_x", "property_color_y", "property_color_z", "property_color_w" };
	for (int i = 0; i < 4; i++) {
		theme_cache.axis[i] = editor->has_color(axes[i], "Editor") ? editor->get_color(axes[i], "Editor") : theme_cache.text;
	}
	theme_cache.section = editor->has_stylebox("bg", "EditorInspectorCategory") ? editor->get_stylebox("bg", "EditorInspectorCategory") : Ref<StyleBox>();
	theme_cache.line = theme_cache.font.is_valid() ? theme_cache.font->get_height(theme_cache.font_size) : 16;
	queue_redraw();
}

void GitPropertyList::clear_list() {
	clear();
	create_item();
	name_width = 0;
}

TreeItem *GitPropertyList::_add_row(const String &p_kind, const Dictionary &p_data, int p_lines) {
	if (theme_cache.font.is_null()) {
		_update_theme_cache();
	}
	TreeItem *item = create_item(get_root());
	Dictionary data = p_data;
	data["kind"] = p_kind;
	item->set_metadata(0, data);
	item->set_cell_mode(0, TreeItem::CELL_MODE_CUSTOM);
	item->set_custom_draw_callback(0, callable_mp(this, &GitPropertyList::_draw_row));
	item->set_selectable(0, false);
	item->set_custom_minimum_height(Math::round(theme_cache.line * p_lines + (p_kind == "title" ? 12 : 6) * theme_cache.scale));
	return item;
}

void GitPropertyList::add_title(const Ref<Texture2D> &p_icon, const String &p_name, const String &p_detail, const String &p_status, const Color &p_status_color) {
	Dictionary data;
	data["icon"] = p_icon;
	data["name"] = p_name;
	data["detail"] = p_detail;
	data["status"] = p_status;
	data["status_color"] = p_status_color;
	_add_row("title", data, 1);
}

void GitPropertyList::add_section(const String &p_title) {
	Dictionary data;
	data["name"] = p_title;
	_add_row("section", data, 1);
}

void GitPropertyList::add_property(const String &p_name, const String &p_old, bool p_has_old, const String &p_new, bool p_has_new, const String &p_tooltip) {
	Dictionary data;
	data["name"] = p_name;
	data["old"] = p_old;
	data["new"] = p_new;
	data["has_old"] = p_has_old;
	data["has_new"] = p_has_new;
	TreeItem *item = _add_row("property", data, p_has_old && p_has_new ? 2 : 1);
	item->set_tooltip_text(0, p_tooltip.is_empty() ? vformat("%s\n%s", p_name, p_has_new ? p_new : p_old) : p_tooltip);
	if (theme_cache.font.is_valid()) {
		name_width = MAX(name_width, theme_cache.font->get_string_size(p_name, HORIZONTAL_ALIGNMENT_LEFT, -1, theme_cache.font_size).x);
	}
}

void GitPropertyList::add_note(const String &p_text, const String &p_tooltip) {
	Dictionary data;
	data["name"] = p_text;
	TreeItem *item = _add_row("note", data, 1);
	item->set_tooltip_text(0, p_tooltip);
}

// Draws p_value from p_x on p_baseline, trimmed at p_right; a vector as "x 1  y 2  z 3" in the
// axis colors (p_axes), a color with a swatch first. Returns where it ends.
float GitPropertyList::_draw_value(const String &p_value, float p_x, float p_baseline, float p_right, const Color &p_color, bool p_axes) {
	const RID canvas = get_custom_drawing_canvas_item();
	const Ref<Font> &font = theme_cache.font;
	const int size = theme_cache.font_size;
	float x = p_x;
	const PackedStringArray numbers = p_axes ? vector_parts(p_value) : PackedStringArray();
	if (!numbers.is_empty()) {
		static const char *labels[4] = { "x", "y", "z", "w" };
		for (int i = 0; i < numbers.size() && x < p_right; i++) {
			font->draw_string(canvas, Vector2(x, p_baseline), labels[i], HORIZONTAL_ALIGNMENT_LEFT, -1, size, theme_cache.axis[i]);
			x += font->get_string_size(labels[i], HORIZONTAL_ALIGNMENT_LEFT, -1, size).x + 4 * theme_cache.scale;
			font->draw_string(canvas, Vector2(x, p_baseline), numbers[i], HORIZONTAL_ALIGNMENT_LEFT, MAX(0.0f, p_right - x), size, p_color);
			x += font->get_string_size(numbers[i], HORIZONTAL_ALIGNMENT_LEFT, -1, size).x + 14 * theme_cache.scale;
		}
		return x;
	}
	if (p_axes && p_value.begins_with("Color(")) {
		const Variant color = UtilityFunctions::str_to_var(p_value);
		if (color.get_type() == Variant::COLOR) {
			const float side = theme_cache.line * 0.7f;
			RenderingServer::get_singleton()->canvas_item_add_rect(canvas, Rect2(x, p_baseline - side * 0.85f, side * 1.6f, side), (Color)color);
			x += side * 1.6f + 6 * theme_cache.scale;
		}
	}
	Ref<TextLine> line;
	line.instantiate();
	line->add_string(shown(p_value), font, size);
	line->set_width(MAX(0.0f, p_right - x));
	line->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	line->draw(canvas, Vector2(x, p_baseline - font->get_ascent(size)), p_color);
	return x + MIN(line->get_size().x, MAX(0.0f, p_right - x));
}

void GitPropertyList::_draw_row(TreeItem *p_item, const Rect2 &p_rect) {
	const Dictionary data = p_item->get_metadata(0);
	const String kind = data.get("kind", String());
	const RID canvas = get_custom_drawing_canvas_item();
	const Ref<Font> &font = theme_cache.font;
	const int size = theme_cache.font_size;
	const float scale = theme_cache.scale;
	const float pad = 8 * scale;
	const float right = p_rect.get_end().x - pad;
	const float first_baseline = p_rect.position.y + 3 * scale + (theme_cache.line - font->get_height(size)) / 2 + font->get_ascent(size);

	if (kind == "section") {
		const Rect2 band(p_rect.position.x, p_rect.position.y + 1 * scale, p_rect.size.x, p_rect.size.y - 2 * scale);
		if (theme_cache.section.is_valid()) {
			theme_cache.section->draw(canvas, band);
		}
		const String title = data.get("name", String());
		const float width = theme_cache.bold->get_string_size(title, HORIZONTAL_ALIGNMENT_LEFT, -1, size).x;
		const float baseline = band.position.y + (band.size.y - theme_cache.bold->get_height(size)) / 2 + theme_cache.bold->get_ascent(size);
		theme_cache.bold->draw_string(canvas, Vector2(band.position.x + (band.size.x - width) / 2, baseline), title, HORIZONTAL_ALIGNMENT_LEFT, -1, size, theme_cache.text);
		return;
	}
	if (kind == "title") {
		float x = p_rect.position.x + pad;
		const float middle = p_rect.position.y + p_rect.size.y / 2;
		const float baseline = middle - font->get_height(size) / 2 + font->get_ascent(size);
		const Ref<Texture2D> icon = data.get("icon", Variant());
		if (icon.is_valid()) {
			icon->draw(canvas, Vector2(x, middle - icon->get_height() / 2.0f));
			x += icon->get_width() + 6 * scale;
		}
		const String name = data.get("name", String());
		theme_cache.bold->draw_string(canvas, Vector2(x, baseline), name, HORIZONTAL_ALIGNMENT_LEFT, MAX(0.0f, right - x), size, theme_cache.text);
		x += theme_cache.bold->get_string_size(name, HORIZONTAL_ALIGNMENT_LEFT, -1, size).x + 10 * scale;
		const String detail = data.get("detail", String());
		if (!detail.is_empty() && x < right) {
			font->draw_string(canvas, Vector2(x, baseline), detail, HORIZONTAL_ALIGNMENT_LEFT, MAX(0.0f, right - x), size, theme_cache.dim);
			x += font->get_string_size(detail, HORIZONTAL_ALIGNMENT_LEFT, -1, size).x + 10 * scale;
		}
		const String status = data.get("status", String());
		if (!status.is_empty() && x < right) {
			const Color color = data.get("status_color", theme_cache.text);
			const int pill_size = Math::round(size * 0.85f);
			const float width = font->get_string_size(status, HORIZONTAL_ALIGNMENT_LEFT, -1, pill_size).x + 14 * scale;
			const float height = font->get_height(pill_size) + 2 * scale;
			const Rect2 pill(x, middle - height / 2, width, height);
			RenderingServer::get_singleton()->canvas_item_add_rect(canvas, pill, color * Color(1, 1, 1, 0.18f));
			font->draw_string(canvas, Vector2(x + 7 * scale, middle - font->get_height(pill_size) / 2 + font->get_ascent(pill_size)), status, HORIZONTAL_ALIGNMENT_LEFT, -1, pill_size, color);
		}
		return;
	}
	if (kind == "note") {
		font->draw_string(canvas, Vector2(p_rect.position.x + pad, first_baseline), data.get("name", String()), HORIZONTAL_ALIGNMENT_LEFT, MAX(0.0f, right - p_rect.position.x - pad), size, theme_cache.dim);
		return;
	}

	// A property: name, the value now, and what it was under it.
	const String name = data.get("name", String());
	const float name_x = p_rect.position.x + pad + 8 * scale;
	const float value_x = name_x + MIN(name_width, p_rect.size.x * 0.45f) + 18 * scale;
	font->draw_string(canvas, Vector2(name_x, first_baseline), name, HORIZONTAL_ALIGNMENT_LEFT, MAX(0.0f, value_x - name_x - 8 * scale), size, theme_cache.name);
	const bool has_old = data.get("has_old", false);
	const bool has_new = data.get("has_new", false);
	const String old_value = data.get("old", String());
	const String new_value = data.get("new", String());
	auto tag = [&](const String &p_text, float p_x, const Color &p_color) {
		const int tag_size = Math::round(size * 0.85f);
		font->draw_string(canvas, Vector2(p_x + 10 * scale, first_baseline), p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, tag_size, p_color);
	};
	if (has_new) {
		const float end = _draw_value(new_value, value_x, first_baseline, right, theme_cache.text, true);
		if (!has_old) {
			tag("new", end, theme_cache.added);
		}
	} else {
		const float end = _draw_value(old_value, value_x, first_baseline, right, theme_cache.dim, true);
		tag("removed", end, theme_cache.removed);
	}
	if (has_old && has_new) {
		const String was = vformat("was %s", plain(old_value));
		_draw_value(was, value_x, first_baseline + theme_cache.line, right, theme_cache.dim, false);
	}
}
