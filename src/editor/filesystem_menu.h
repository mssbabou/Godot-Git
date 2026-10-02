#pragma once

#include <godot_cpp/classes/editor_context_menu_plugin.hpp>

using namespace godot;

class GitDock;

// Git's items in the FileSystem dock's right-click menu: show a file's uncommitted changes in the
// Diff panel, and discard them. Only offered for files (or folders holding files) with changes.
class GitFileSystemMenu : public EditorContextMenuPlugin {
	GDCLASS(GitFileSystemMenu, EditorContextMenuPlugin)

	ObjectID dock;

	GitDock *_get_dock() const;
	void _show_change(const PackedStringArray &p_paths);
	void _show_history(const PackedStringArray &p_paths);
	void _discard(const PackedStringArray &p_paths);
	void _ignore(const PackedStringArray &p_paths);

protected:
	static void _bind_methods() {}

public:
	void set_dock(GitDock *p_dock);
	void _popup_menu(const PackedStringArray &p_paths) override;
};
