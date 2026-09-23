#include "editor/git_editor_plugin.h"

void GitEditorPlugin::_enter_tree() {
	dock = memnew(GitDock);
	add_dock(dock);
}

void GitEditorPlugin::_exit_tree() {
	if (dock) {
		remove_dock(dock);
		dock->queue_free();
		dock = nullptr;
	}
}
