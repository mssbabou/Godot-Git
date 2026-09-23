#pragma once

#include <godot_cpp/classes/editor_plugin.hpp>

#include "git_dock.h"

using namespace godot;

// Loaded automatically by the extension in the editor; owns the Git dock.
class GitEditorPlugin : public EditorPlugin {
	GDCLASS(GitEditorPlugin, EditorPlugin)

	GitDock *dock = nullptr;

protected:
	static void _bind_methods() {}

public:
	void _enter_tree() override;
	void _exit_tree() override;
};
