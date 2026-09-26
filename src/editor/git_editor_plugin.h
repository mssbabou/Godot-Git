#pragma once

#include <godot_cpp/classes/editor_plugin.hpp>

#include "editor/git_diff_dock.h"
#include "editor/git_dock.h"

using namespace godot;

// Loaded automatically by the extension in the editor; owns the Git dock and the Diff panel.
class GitEditorPlugin : public EditorPlugin {
	GDCLASS(GitEditorPlugin, EditorPlugin)

	GitDock *dock = nullptr;
	GitDiffDock *diff_dock = nullptr;

protected:
	static void _bind_methods() {}

public:
	void _enter_tree() override;
	void _exit_tree() override;
};
