#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// Shows git status in the editor's FileSystem dock, the way the Git dock shows it: changed files'
// names in their status color with the status letter at the right edge, and folders holding
// changes in a softer color with a dot. There's no API for this: it recolors the dock's own Tree
// (and the file list of its split view, colors only) whenever they redraw with rows it hasn't
// colored yet, since the dock rebuilds its rows on every rescan, search and folder change, and
// draws the letters on the Tree's custom drawing layer. Rows it colored are put back as they
// were when their file is no longer changed, or when this is freed.
class GitFileSystemColors : public Node {
	GDCLASS(GitFileSystemColors, Node)

	struct Badge {
		uint64_t item = 0;
		String text;
		Color color;
	};

	Dictionary colors; // res:// path (folders end in "/") -> Color of the name.
	Dictionary badges; // res:// path -> [text, Color]: the letter (or a dot) at the right edge.
	int version = 0; // Bumped by set_colors.
	uint64_t tree_id = 0;
	uint64_t list_id = 0;
	uint64_t painted_root = 0; // The tree's root when it was last painted; new root = new rows.
	int painted_version = -1;
	LocalVector<Badge> painted_badges;
	String painted_list; // The file list's items when it was last painted.
	int painted_list_version = -1;

	void _connect_dock();
	void _on_tree_draw();
	void _on_tree_item_collapsed(TreeItem *p_item);
	void _on_list_draw();
	void _paint_tree(bool p_restore_only);
	void _paint_list(bool p_restore_only);
	void _draw_badges();

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	void set_colors(const Dictionary &p_colors, const Dictionary &p_badges);
};
