#pragma once

#include <godot_cpp/classes/editor_plugin.hpp>

#include "editor/filesystem_menu.h"
#include "editor/git_diff_dock.h"
#include "editor/git_dock.h"
#include "editor/script_marks.h"

using namespace godot;

// Loaded by the extension itself in the editor; owns the Git dock and the Diff panel. They're
// shown while the addon's plugin (addons/godot_git/plugin.cfg) is on in Project Settings > Plugins.
class GitEditorPlugin : public EditorPlugin {
	GDCLASS(GitEditorPlugin, EditorPlugin)

	GitDock *dock = nullptr;
	GitDiffDock *diff_dock = nullptr;
	Ref<GitFileSystemMenu> filesystem_menu;
	Ref<GitScriptMenu> script_menu;
	String plugin_path; // res:// path of the addon's plugin.cfg; "" if it has none.

	bool _is_switched_on() const;
	void _on_project_settings_changed();
	void _enable_once();
	void _add_docks();
	void _remove_docks();
	void _reload_export_presets();

protected:
	static void _bind_methods() {}

public:
	void _enter_tree() override;
	void _exit_tree() override;
};
