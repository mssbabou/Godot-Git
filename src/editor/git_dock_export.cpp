// GitDock: keeping the addon out of exports on Godot 4.7. The addon is editor-only; 4.8 knows that
// from the .gdextension (include_tags), but 4.7 exports it anyway: the export warns and the game
// logs an error. Only the preset's exclude filter avoids both (gotcha 13), so the dock offers to
// add it, once, while a preset lacks it.

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include "editor/ui_text.h"

using namespace godot_git;

namespace {

const char *PRESETS_FILE = "res://export_presets.cfg";
const char *EXPORT_FILTER = "addons/godot_git/*";

// Whether an exclude filter ("*.txt, addons/godot_git/*") already keeps the addon out.
bool filter_covers_addon(const String &p_filter) {
	for (const String &part : p_filter.split(",", false)) {
		const String pattern = part.strip_edges().trim_prefix("res://");
		if (pattern.begins_with("addons/godot_git") || pattern == "addons/*" || pattern == "addons/**") {
			return true;
		}
	}
	return false;
}

// export_presets.cfg with the filter added to every preset that lacks it, edited as text so
// nothing else in the file changes. r_names: those presets' names.
String add_filter_to_presets(const String &p_text, PackedStringArray &r_names) {
	PackedStringArray lines = p_text.split("\n");
	bool in_preset = false;
	String name;
	for (int i = 0; i < lines.size(); i++) {
		const String line = lines[i].strip_edges();
		if (line.begins_with("[")) {
			// [preset.0] holds the filters; [preset.0.options] the platform's options.
			in_preset = line.begins_with("[preset.") && line.count(".") == 1;
			continue;
		}
		if (in_preset && line.begins_with("name=")) {
			name = line.trim_prefix("name=").trim_prefix("\"").trim_suffix("\"");
		}
		if (in_preset && line.begins_with("exclude_filter=\"") && line.ends_with("\"")) {
			const String filter = line.trim_prefix("exclude_filter=\"").trim_suffix("\"");
			if (!filter_covers_addon(filter)) {
				const String added = filter.strip_edges().is_empty() ? String(EXPORT_FILTER) : vformat("%s, %s", filter, EXPORT_FILTER);
				lines.set(i, lines[i].replace(vformat("exclude_filter=\"%s\"", filter), vformat("exclude_filter=\"%s\"", added)));
				r_names.push_back(name);
			}
		}
	}
	return String("\n").join(lines);
}

} // namespace

void GitDock::_build_export_banner(Control *p_parent) {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	export_banner = memnew(PanelContainer);
	export_banner->hide();
	p_parent->add_child(export_banner);
	VBoxContainer *vb = memnew(VBoxContainer);
	export_banner->add_child(vb);
	HBoxContainer *text_hb = memnew(HBoxContainer);
	vb->add_child(text_hb);
	TextureRect *icon = memnew(TextureRect);
	icon->set_name("Icon");
	icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	icon->set_v_size_flags(SIZE_SHRINK_BEGIN);
	text_hb->add_child(icon);
	export_label = memnew(Label);
	export_label->set_h_size_flags(SIZE_EXPAND_FILL);
	export_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	export_label->set_custom_minimum_size(Vector2(160 * scale, 0)); // Gotcha 45.
	text_hb->add_child(export_label);
	HBoxContainer *buttons = memnew(HBoxContainer);
	buttons->set_alignment(BoxContainer::ALIGNMENT_END);
	vb->add_child(buttons);
	Button *dismiss = memnew(Button);
	dismiss->set_text("Not Now");
	dismiss->set_tooltip_text("Don't offer this again in this project. The filter can always be added by hand: Project > Export, Resources, Filters to exclude files.");
	dismiss->connect("pressed", callable_mp(this, &GitDock::_dismiss_export_offer));
	buttons->add_child(dismiss);
	Button *leave_out = memnew(Button);
	leave_out->set_text("Leave It Out");
	leave_out->set_tooltip_text(vformat("Add %s to every export preset's \"Filters to exclude files\".", EXPORT_FILTER));
	leave_out->connect("pressed", callable_mp(this, &GitDock::_leave_out_of_exports));
	buttons->add_child(leave_out);
}

// Called on every refresh; reads the presets only when their file changed.
void GitDock::_update_export_banner() {
	const Dictionary version = Engine::get_singleton()->get_version_info();
	const bool knows_editor_only = (int)version["major"] > 4 || ((int)version["major"] == 4 && (int)version["minor"] >= 8);
	const bool dismissed = EditorInterface::get_singleton()->get_editor_settings()->get_project_metadata("godot_git", "export_offer_dismissed", false);
	if (knows_editor_only || dismissed || !FileAccess::file_exists(PRESETS_FILE)) {
		export_banner->hide();
		return;
	}
	const uint64_t time = FileAccess::get_modified_time(PRESETS_FILE);
	if (time != export_presets_time) {
		export_presets_time = time;
		export_presets_missing.clear();
		add_filter_to_presets(FileAccess::get_file_as_string(PRESETS_FILE), export_presets_missing);
	}
	export_banner->set_visible(!export_presets_missing.is_empty());
	if (export_presets_missing.is_empty()) {
		return;
	}
	export_label->set_text(vformat("Exports from %s will include Godot Git, which only works in the editor: the export warns and the game logs an error at start.", join_list(export_presets_missing, 2)));
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	const Ref<StyleBoxFlat> panel = _tinted_panel(get_theme_color("accent_color", "Editor"));
	for (const Side side : { SIDE_TOP, SIDE_RIGHT, SIDE_BOTTOM }) {
		panel->set_content_margin(side, Math::round(4 * scale));
	}
	export_banner->add_theme_stylebox_override("panel", panel);
	Object::cast_to<TextureRect>(export_banner->find_child("Icon", true, false))->set_texture(get_theme_icon("NodeInfo", "EditorIcons"));
}

void GitDock::_leave_out_of_exports() {
	PackedStringArray names;
	const String text = add_filter_to_presets(FileAccess::get_file_as_string(PRESETS_FILE), names);
	Ref<FileAccess> file = FileAccess::open(PRESETS_FILE, FileAccess::WRITE);
	if (file.is_null()) {
		_set_status(STATUS_ERROR, "Couldn't write export_presets.cfg, so nothing was changed.");
		return;
	}
	file->store_string(text);
	file.unref();
	// The editor keeps the presets in memory and would save its old copy over the file the next
	// time a preset is edited; this makes it read the file again.
	if (reload_export_presets.is_valid()) {
		reload_export_presets.call();
	}
	export_presets_time = 0;
	_update_export_banner();
	refresh();
	_set_status(STATUS_SUCCESS, vformat("Godot Git is left out of exports from %s (%s in Filters to exclude files)", join_list(names, 3), EXPORT_FILTER));
}

void GitDock::_dismiss_export_offer() {
	EditorInterface::get_singleton()->get_editor_settings()->set_project_metadata("godot_git", "export_offer_dismissed", true);
	export_banner->hide();
}

void GitDock::set_reload_export_presets(const Callable &p_reload) {
	reload_export_presets = p_reload;
}
