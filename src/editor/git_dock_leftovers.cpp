// GitDock: copies of your edits a pull set aside and never put back, because the editor went
// down in the middle of it (see GitRepository::pull). They're your work: the banner says where
// they are and puts them back, or lets them go.

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include "editor/ui_text.h"

using namespace godot_git;

void GitDock::_build_leftovers_banner(Control *p_parent) {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	leftovers_banner = memnew(PanelContainer);
	leftovers_banner->hide();
	p_parent->add_child(leftovers_banner);
	VBoxContainer *vb = memnew(VBoxContainer);
	leftovers_banner->add_child(vb);
	HBoxContainer *text_hb = memnew(HBoxContainer);
	vb->add_child(text_hb);
	TextureRect *icon = memnew(TextureRect);
	icon->set_name("Icon");
	icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	icon->set_v_size_flags(SIZE_SHRINK_BEGIN);
	text_hb->add_child(icon);
	leftovers_label = memnew(Label);
	leftovers_label->set_h_size_flags(SIZE_EXPAND_FILL);
	leftovers_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	leftovers_label->set_custom_minimum_size(Vector2(160 * scale, 0)); // Gotcha 45.
	text_hb->add_child(leftovers_label);
	HBoxContainer *buttons = memnew(HBoxContainer);
	buttons->set_alignment(BoxContainer::ALIGNMENT_END);
	vb->add_child(buttons);
	leftovers_delete = memnew(Button);
	leftovers_delete->set_text("Delete Copies");
	leftovers_delete->connect("pressed", callable_mp(this, &GitDock::_resolve_leftovers).bind(false));
	buttons->add_child(leftovers_delete);
	leftovers_put_back = memnew(Button);
	leftovers_put_back->set_text("Put Them Back");
	leftovers_put_back->connect("pressed", callable_mp(this, &GitDock::_resolve_leftovers).bind(true));
	buttons->add_child(leftovers_put_back);
	leftovers_confirm = _make_confirm("Delete Copies", "Delete", callable_mp(this, &GitDock::_resolve_leftovers_confirmed).bind(false));
}

// Called on every refresh: a folder listing in .git, nothing more.
void GitDock::_update_leftovers_banner() {
	leftovers = repo->get_pull_leftovers();
	leftovers_banner->set_visible(!leftovers.is_empty());
	if (leftovers.is_empty()) {
		return;
	}
	const Dictionary first = leftovers[0];
	const Array files = first["files"];
	PackedStringArray names, waiting;
	for (int i = 0; i < files.size(); i++) {
		const Dictionary file = files[i];
		names.push_back(String(file["path"]).get_file());
		if (!bool(file["back"])) {
			waiting.push_back(String(file["path"]).get_file());
		}
	}
	if (waiting.is_empty()) {
		leftovers_label->set_text(vformat("A pull that didn't finish left copies of your edits to %s in the .git folder. The files already match them.", join_list(names, 3)));
	} else {
		leftovers_label->set_text(vformat("A pull didn't finish (the editor closed during it). Your uncommitted edits to %s are saved in the .git folder, not in the files.", join_list(waiting, 3)));
	}
	leftovers_label->set_tooltip_text(vformat("%s\n\n%s", first["folder"], String("\n").join(names)));
	leftovers_put_back->set_visible(!waiting.is_empty());
	leftovers_put_back->set_tooltip_text(vformat("Write your edits back into %s, byte for byte, replacing what's in them now. Then the copies are deleted.", join_list(waiting, 3)));
	leftovers_delete->set_tooltip_text(waiting.is_empty() ? String("The files already have these edits: delete the copies.") : String("Delete the copies; your edits in them are lost."));

	const Color tint = get_theme_color("warning_color", "Editor");
	leftovers_banner->add_theme_stylebox_override("panel", _tinted_panel(tint));
	Object::cast_to<TextureRect>(leftovers_banner->find_child("Icon", true, false))->set_texture(get_theme_icon("StatusWarning", "EditorIcons"));
}

void GitDock::_resolve_leftovers(bool p_put_back) {
	if (leftovers.is_empty()) {
		return;
	}
	const Dictionary first = leftovers[0];
	bool any_waiting = false;
	const Array files = first["files"];
	for (int i = 0; i < files.size(); i++) {
		any_waiting = any_waiting || !bool(Dictionary(files[i])["back"]);
	}
	// Deleting copies the files don't have yet loses those edits: ask.
	if (!p_put_back && any_waiting) {
		leftovers_confirm->set_text("Delete the saved copies of your edits? The files don't have them, so those edits are lost.");
		leftovers_confirm->popup_centered();
		return;
	}
	_resolve_leftovers_confirmed(p_put_back);
}

void GitDock::_resolve_leftovers_confirmed(bool p_put_back) {
	if (leftovers.is_empty()) {
		return;
	}
	const String folder = Dictionary(leftovers[0])["folder"];
	_remember_open_scenes();
	const Error err = repo->resolve_pull_leftovers(folder, p_put_back);
	_report(err, p_put_back ? "Put back" : "Delete copies");
	if (err == OK) {
		EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	}
	refresh();
	if (err == OK) {
		_set_status(STATUS_SUCCESS, p_put_back ? String("Put your edits back from the unfinished pull") : String("Deleted the copies from the unfinished pull"));
		_reload_changed_scenes();
	}
}
