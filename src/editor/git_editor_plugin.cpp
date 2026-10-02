#include "editor/git_editor_plugin.h"

#include <godot_cpp/classes/editor_export_platform_extension.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include "addon_files.h"

// The addon is listed under Project Settings > Plugins through its plugin.cfg, whose plugin.gd
// does nothing: it's the switch. This plugin, registered by the extension itself, does the work
// and follows the switch. The other way round (plugin.gd extending this class) crashed the
// editor when the addon's folder disappeared mid-session (switching to a branch without it):
// Godot unloads the library but leaves the Plugins list's plugin object, of a class that's gone.
void GitEditorPlugin::_enter_tree() {
	const String manifest = godot_git::addon_manifest_path();
	const String cfg = manifest.is_empty() ? String() : ProjectSettings::get_singleton()->localize_path(manifest.get_base_dir().path_join("plugin.cfg"));
	plugin_path = !cfg.is_empty() && FileAccess::file_exists(cfg) ? cfg : String();
	ProjectSettings::get_singleton()->connect("settings_changed", callable_mp(this, &GitEditorPlugin::_on_project_settings_changed));
	_on_project_settings_changed();
	callable_mp(this, &GitEditorPlugin::_enable_once).call_deferred(); // Once the editor is up.
}

void GitEditorPlugin::_exit_tree() {
	ProjectSettings::get_singleton()->disconnect("settings_changed", callable_mp(this, &GitEditorPlugin::_on_project_settings_changed));
	_remove_docks();
}

// Read from the project's settings, not EditorInterface::is_plugin_enabled: that only knows
// plugins already loaded, and the editor loads them after this runs.
bool GitEditorPlugin::_is_switched_on() const {
	if (plugin_path.is_empty()) {
		return true; // An addon without plugin.cfg (a build from before it had one): always on.
	}
	const PackedStringArray enabled = ProjectSettings::get_singleton()->get_setting("editor_plugins/enabled", PackedStringArray());
	return enabled.has(plugin_path);
}

// Ticking or unticking the plugin in Project Settings > Plugins changes editor_plugins/enabled.
void GitEditorPlugin::_on_project_settings_changed() {
	const bool on = _is_switched_on();
	if (on && !dock) {
		_add_docks();
	} else if (!on && dock) {
		_remove_docks();
	}
}

// The first time the library loads in a project, the plugin is switched on, so installing stays
// unzip-and-restart (and nobody loses the dock by updating from a build without plugin.cfg).
// Remembered per project in editor metadata, so a plugin switched off stays off.
void GitEditorPlugin::_enable_once() {
	if (plugin_path.is_empty() || !is_inside_tree()) {
		return;
	}
	Ref<EditorSettings> settings = EditorInterface::get_singleton()->get_editor_settings();
	if (bool(settings->get_project_metadata("godot_git", "plugin_enabled_once", false))) {
		return;
	}
	settings->set_project_metadata("godot_git", "plugin_enabled_once", true);
	if (!_is_switched_on()) {
		EditorInterface::get_singleton()->set_plugin_enabled(plugin_path, true);
	}
}

void GitEditorPlugin::_add_docks() {
	dock = memnew(GitDock);
	add_dock(dock);
	diff_dock = memnew(GitDiffDock);
	add_dock(diff_dock);
	dock->set_diff_dock(diff_dock);
	dock->set_reload_export_presets(callable_mp(this, &GitEditorPlugin::_reload_export_presets));
	filesystem_menu.instantiate();
	filesystem_menu->set_dock(dock);
	add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM, filesystem_menu);
	script_menu.instantiate();
	script_menu->set_marks(dock->get_script_marks());
	add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_SCRIPT_EDITOR_CODE, script_menu);
}

void GitEditorPlugin::_remove_docks() {
	if (filesystem_menu.is_valid()) {
		remove_context_menu_plugin(filesystem_menu);
		filesystem_menu.unref();
	}
	if (script_menu.is_valid()) {
		remove_context_menu_plugin(script_menu);
		script_menu.unref();
	}
	if (dock) {
		remove_dock(dock);
		// Not queue_free: when the addon's files disappear (switching to a branch without it),
		// Godot unloads the library right after removing this plugin, before the end of the
		// frame. A queued free would then run the dock's destructor in unmapped code.
		memdelete(dock);
		dock = nullptr;
	}
	if (diff_dock) {
		remove_dock(diff_dock);
		memdelete(diff_dock); // Deleted after the Git dock, which points at it.
		diff_dock = nullptr;
	}
}

// Makes the editor read export_presets.cfg again, after the dock edited it (see
// _leave_out_of_exports). There's no API for that, or for changing a preset's filters, but the
// editor reloads the file whenever an export platform comes or goes (EditorExport::
// should_reload_presets, 4.7.2): so one comes and goes, before the editor's next frame.
void GitEditorPlugin::_reload_export_presets() {
	Ref<EditorExportPlatformExtension> platform;
	platform.instantiate();
	add_export_platform(platform);
	remove_export_platform(platform);
}
