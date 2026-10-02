#include "editor/git_editor_plugin.h"

#include <godot_cpp/classes/editor_export_platform_extension.hpp>

void GitEditorPlugin::_enter_tree() {
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

void GitEditorPlugin::_exit_tree() {
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
