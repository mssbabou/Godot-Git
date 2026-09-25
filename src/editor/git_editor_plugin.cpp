#include "editor/git_editor_plugin.h"

void GitEditorPlugin::_enter_tree() {
	dock = memnew(GitDock);
	add_dock(dock);
}

void GitEditorPlugin::_exit_tree() {
	if (dock) {
		remove_dock(dock);
		// Not queue_free: when the addon's files disappear (switching to a branch without it),
		// Godot unloads the library right after removing this plugin, before the end of the
		// frame. A queued free would then run the dock's destructor in unmapped code.
		memdelete(dock);
		dock = nullptr;
	}
}
