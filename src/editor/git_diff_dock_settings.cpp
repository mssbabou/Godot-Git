// The Diff panel's settings view: a `.import` or `.uid` file's changes setting by setting
// ("Compress › Mode   Lossless → VRAM Compressed"), shown on its own or under the file it belongs
// to. The settings come from GitRepository::get_diff ("settings", "importer").

#include "editor/git_diff_dock.h"

#include <godot_cpp/classes/editor_interface.hpp>

#include "editor/ui_text.h"

using namespace godot_git;

namespace {

enum SettingColumn {
	COLUMN_SETTING,
	COLUMN_OLD,
	COLUMN_ARROW,
	COLUMN_NEW,
	COLUMN_REST, // Takes the remaining width, so the others stay as wide as their text.
	SETTING_COLUMN_COUNT,
};

// Values Godot fills in itself: where the imported copy goes (named after the file), what the
// importer reported about it, and the file's dependencies. They change with the file's name or its
// real settings, so listing them repeats the change.
bool is_generated(const String &p_section, const String &p_key) {
	if (p_section == "deps") {
		return true;
	}
	return p_section == "remap" && (p_key == "path" || p_key.begins_with("path.") || p_key == "metadata" || p_key == "type");
}

// A settings key as the Import dock names it: "compress/hdr_compression" -> "Compress › HDR Compression".
String setting_name(const String &p_section, const String &p_key) {
	if (p_key == "uid") {
		return "UID";
	}
	if (p_section == "remap" && p_key == "importer") {
		return "Import As";
	}
	static const char *const words[][2] = {
		{ "2d", "2D" },
		{ "3d", "3D" },
		{ "astc", "ASTC" },
		{ "bpm", "BPM" },
		{ "bptc", "BPTC" },
		{ "etc2", "ETC2" },
		{ "fbx", "FBX" },
		{ "fps", "FPS" },
		{ "gltf", "glTF" },
		{ "hdr", "HDR" },
		{ "id", "ID" },
		{ "lod", "LOD" },
		{ "mp3", "MP3" },
		{ "msdf", "MSDF" },
		{ "rdo", "RDO" },
		{ "rgb", "RGB" },
		{ "rgba", "RGBA" },
		{ "s3tc", "S3TC" },
		{ "sdf", "SDF" },
		{ "srgb", "sRGB" },
		{ "uastc", "UASTC" },
		{ "uv", "UV" },
		{ "uv2", "UV2" },
		{ "vram", "VRAM" },
		{ "wav", "WAV" },
	};
	PackedStringArray parts;
	if (!p_section.is_empty() && p_section != "params" && p_section != "remap") {
		parts.push_back(p_section.capitalize());
	}
	for (const String &part : p_key.split("/")) {
		PackedStringArray shown;
		for (const String &word : part.split("_", false)) {
			String text = word.capitalize();
			for (const auto &pair : words) {
				if (word == pair[0]) {
					text = pair[1];
				}
			}
			shown.push_back(text);
		}
		parts.push_back(String(" ").join(shown));
	}
	return String::utf8(" › ").join(parts);
}

// The names of settings that are a choice, stored as a number. Plugins can't ask an importer for
// its options, so these are copied from Godot 4.7.2 (editor/import/resource_importer_texture.cpp
// and resource_importer_wav.cpp): the importers people change most. Others show the number.
const char *choice_names(const String &p_importer, const String &p_key) {
	static const char *const channels = "Red,Green,Blue,Alpha,Inverted Red,Inverted Green,Inverted Blue,Inverted Alpha,Unused,Zero,One";
	static const char *const choices[][3] = {
		{ "texture", "compress/mode", "Lossless,Lossy,VRAM Compressed,VRAM Uncompressed,Basis Universal" },
		{ "texture", "compress/uastc_level", "Fastest,Faster,Medium,Slower,Slowest" },
		{ "texture", "compress/hdr_compression", "Disabled,Opaque Only,Always" },
		{ "texture", "compress/normal_map", "Detect,Enable,Disabled" },
		{ "texture", "compress/channel_pack", "sRGB Friendly,Optimized" },
		{ "texture", "roughness/mode", "Detect,Disabled,Red,Green,Blue,Alpha,Gray" },
		{ "texture", "process/channel_remap/red", channels },
		{ "texture", "process/channel_remap/green", channels },
		{ "texture", "process/channel_remap/blue", channels },
		{ "texture", "process/channel_remap/alpha", channels },
		{ "texture", "detect_3d/compress_to", "Disabled,VRAM Compressed,Basis Universal" },
		{ "wav", "edit/loop_mode", "Detect From WAV,Disabled,Forward,Ping-Pong,Backward" },
		{ "wav", "compress/mode", "PCM (Uncompressed),IMA ADPCM,Quite OK Audio" },
	};
	for (const auto &choice : choices) {
		if (p_importer == choice[0] && p_key == choice[1]) {
			return choice[2];
		}
	}
	return nullptr;
}

// A value as the Import dock would show it: On / Off, a choice's name, a string without quotes.
// Long values (a scene's per-node options) are shortened; r_full is the whole value for the tooltip.
String setting_value(const String &p_importer, const String &p_section, const String &p_key, const String &p_raw, String &r_full) {
	r_full = String();
	if (p_raw == "true") {
		return "On";
	}
	if (p_raw == "false") {
		return "Off";
	}
	if (p_raw.length() >= 2 && p_raw.begins_with("\"") && p_raw.ends_with("\"")) {
		const String text = p_raw.substr(1, p_raw.length() - 2).c_unescape();
		if (p_section == "remap" && p_key == "importer") {
			// The Import dock's names for the common importers.
			static const char *const names[][2] = { { "texture", "Texture2D" }, { "image", "Image" }, { "texture_atlas", "TextureAtlas" }, { "bitmap", "BitMap" }, { "wav", "AudioStreamWAV" }, { "mp3", "AudioStreamMP3" }, { "oggvorbisstr", "AudioStreamOggVorbis" }, { "scene", "Scene" } };
			for (const auto &name : names) {
				if (text == name[0]) {
					return name[1];
				}
			}
		}
		return text.is_empty() ? String("None") : text;
	}
	if (p_section == "params" && p_raw.is_valid_int()) {
		if (const char *names = choice_names(p_importer, p_key)) {
			const PackedStringArray list = String(names).split(",");
			const int index = p_raw.to_int();
			if (index >= 0 && index < list.size()) {
				return list[index];
			}
		}
	}
	const String line = p_raw.replace("\n", " ").replace("\t", " ");
	if (line.length() <= 60) {
		return line;
	}
	r_full = p_raw;
	return line.left(59) + String::utf8("…");
}

void set_cell(TreeItem *p_item, int p_column, const String &p_text, const Color &p_color) {
	p_item->set_text(p_column, p_text);
	p_item->set_custom_color(p_column, p_color);
	// Otherwise the column doesn't grow with its text (gotcha 4).
	p_item->set_text_overrun_behavior(p_column, TextServer::OVERRUN_NO_TRIMMING);
}

} // namespace

Tree *GitDiffDock::_make_settings_tree(Control *p_parent) {
	Tree *tree = memnew(Tree);
	tree->set_columns(SETTING_COLUMN_COUNT);
	tree->set_hide_root(true);
	tree->set_hide_folding(true);
	tree->set_focus_mode(FOCUS_NONE);
	tree->set_v_size_flags(SIZE_EXPAND_FILL);
	for (int i = 0; i < SETTING_COLUMN_COUNT; i++) {
		tree->set_column_expand(i, i == COLUMN_REST);
	}
	p_parent->add_child(tree);
	return tree;
}

void GitDiffDock::_show_settings() {
	settings_tree->clear();
	if (_current_view() == VIEW_SETTINGS) {
		_fill_settings(settings_tree, settings_tree->create_item(), diff);
	}

	// The companions: each one's name, then its settings under it.
	companion_tree->clear();
	const Array companions = diff.get("companions", Array());
	companion_view->set_visible(!companions.is_empty());
	if (companions.is_empty()) {
		return;
	}
	TreeItem *root = companion_tree->create_item();
	for (int i = 0; i < companions.size(); i++) {
		const Dictionary companion = companions[i];
		const String status = companion.get("status", String());
		TreeItem *file = companion_tree->create_item(root);
		file->set_selectable(COLUMN_SETTING, false);
		set_cell(file, COLUMN_SETTING, String(companion.get("path", String())).get_file(), theme.dim);
		// Its settings, also when it's new or deleted: a new script's .uid is always new, and its
		// uid is the point (it said only "New file", maintainer 2026-09-30). New values alone, or
		// old ones alone, say it's new or deleted, so there's no label for that (it didn't line up).
		_fill_settings(companion_tree, file, companion);
		for (int c = 0; c < SETTING_COLUMN_COUNT; c++) {
			file->set_selectable(c, false);
		}
	}
	callable_mp(this, &GitDiffDock::_fit_companions).call_deferred();
}

// The changed settings of p_diff as rows under p_parent. Settings that only one side has, in a
// file both sides have, are folded into one line each: the importer gained or dropped options
// (a newer Godot, or another importer), and dozens of rows would bury the real change.
void GitDiffDock::_fill_settings(Tree *p_tree, TreeItem *p_parent, const Dictionary &p_diff) {
	const Array changes = p_diff.get("settings", Array());
	const String importer = p_diff.get("importer", String());
	const String status = p_diff.get("status", String());
	const bool whole_file = status == "new" || status == "untracked" || status == "deleted";
	const Color font_color = get_theme_color("font_color", "Tree");

	PackedStringArray added, removed;
	int rows = 0;
	for (int i = 0; i < changes.size(); i++) {
		const Dictionary change = changes[i];
		const String section = change["section"];
		const String key = change["key"];
		if (is_generated(section, key)) {
			continue;
		}
		const String name = setting_name(section, key);
		const bool has_old = change.has("old");
		const bool has_new = change.has("new");
		if (!whole_file && has_old != has_new) {
			(has_new ? added : removed).push_back(name);
			continue;
		}
		TreeItem *row = p_tree->create_item(p_parent);
		set_cell(row, COLUMN_SETTING, name, font_color);
		for (const bool is_new : { false, true }) {
			if (!change.has(is_new ? "new" : "old")) {
				continue;
			}
			String full;
			const String value = setting_value(importer, section, key, change[is_new ? "new" : "old"], full);
			set_cell(row, is_new ? COLUMN_NEW : COLUMN_OLD, value, is_new ? theme.added : theme.removed);
			if (!full.is_empty()) {
				row->set_tooltip_text(is_new ? COLUMN_NEW : COLUMN_OLD, full);
			}
		}
		if (has_old && has_new) {
			set_cell(row, COLUMN_ARROW, String::utf8("→"), theme.dim);
		}
		row->set_tooltip_text(COLUMN_SETTING, vformat("%s (in the file: %s)", name, key));
		rows++;
	}

	auto note = [&](const String &p_text, const String &p_tooltip) {
		TreeItem *row = p_tree->create_item(p_parent);
		set_cell(row, COLUMN_SETTING, p_text, theme.dim);
		row->set_tooltip_text(COLUMN_SETTING, p_tooltip);
		rows++;
	};
	if (!added.is_empty()) {
		note(plural(added.size(), "new setting", "new settings"), vformat("Settings the file didn't have before, usually because a newer version of Godot added them:\n%s", String("\n").join(added)));
	}
	if (!removed.is_empty()) {
		note(vformat("%s removed", plural(removed.size(), "setting", "settings")), vformat("Settings the file no longer has, usually because a newer version of Godot dropped them:\n%s", String("\n").join(removed)));
	}
	if (rows == 0) {
		note("Only values Godot fills in itself changed.", "Such as where the imported copy is kept in .godot/imported, which follows from the file's name.");
	}
	for (TreeItem *row = p_parent->get_first_child(); row; row = row->get_next()) {
		for (int c = 0; c < SETTING_COLUMN_COUNT; c++) {
			row->set_selectable(c, false);
		}
	}
}

// The companions' list as tall as its rows, up to a third of the default panel height; it scrolls
// beyond that.
void GitDiffDock::_fit_companions() {
	const float limit = 120 * EditorInterface::get_singleton()->get_editor_scale();
	companion_scroll->set_custom_minimum_size(Vector2(0, MIN(companion_tree->get_combined_minimum_size().y, limit)));
}
