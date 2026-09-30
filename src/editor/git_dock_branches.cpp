// The branch picker: listing branches, switching (in the background when LFS files may need
// downloading), and New / Rename / Delete Branch.

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include "addon_files.h"
#include "build_info.h"
#include "editor/file_opener.h"
#include "editor/filesystem_colors.h"
#include "editor/ui_text.h"

using namespace godot_git;

void GitDock::_fill_branches() {
	const String current = sync_status.get("branch", String());
	PackedStringArray local = repo->get_branches();
	if (!current.is_empty() && !local.has(current)) {
		// Unborn branch (no commits yet) or detached HEAD.
		local.push_back(current);
	}
	local.sort();

	// Remote branches that don't already have a local branch of the same name.
	PackedStringArray remote;
	for (const String &name : repo->get_remote_branches()) {
		const int slash = name.find("/");
		if (!local.has(slash >= 0 ? name.substr(slash + 1) : name)) {
			remote.push_back(name);
		}
	}
	remote.sort();

	const Ref<Texture2D> branch_icon = get_theme_icon("VcsBranches", "EditorIcons");
	branch_select->clear();
	for (const String &name : local) {
		branch_select->add_icon_item(branch_icon, name);
		const int index = branch_select->get_item_count() - 1;
		branch_select->set_item_metadata(index, name);
		if (name == current) {
			branch_select->select(index);
		}
	}
	if (!remote.is_empty()) {
		branch_select->add_separator("Remote branches");
		const Ref<Texture2D> remote_icon = get_theme_icon("ArrowDown", "EditorIcons");
		for (const String &name : remote) {
			branch_select->add_icon_item(remote_icon, name);
			branch_select->set_item_metadata(branch_select->get_item_count() - 1, name);
			branch_select->set_item_tooltip(branch_select->get_item_count() - 1, "Check out as a local branch tracking " + name);
		}
	}
	// Actions on branches, after them (metadata: the MenuId).
	branch_select->add_separator();
	branch_select->add_icon_item(get_theme_icon("Add", "EditorIcons"), "New Branch...");
	branch_select->set_item_metadata(branch_select->get_item_count() - 1, MORE_NEW_BRANCH);

	const bool on_branch = repo->get_branches().has(current); // Not detached, and has commits.
	branch_select->add_icon_item(get_theme_icon("Edit", "EditorIcons"), "Rename Branch...");
	int index = branch_select->get_item_count() - 1;
	branch_select->set_item_metadata(index, BRANCH_RENAME);
	branch_select->set_item_disabled(index, !on_branch);
	branch_select->set_item_tooltip(index, on_branch ? vformat("Give %s a new name.", current) : String("There's no branch to rename: HEAD is detached, or there are no commits yet."));

	branch_select->add_icon_item(get_theme_icon("Remove", "EditorIcons"), "Delete Branch...");
	index = branch_select->get_item_count() - 1;
	branch_select->set_item_metadata(index, BRANCH_DELETE);
	const bool others = local.size() > (on_branch ? 1 : 0);
	branch_select->set_item_disabled(index, !others);
	branch_select->set_item_tooltip(index, others ? String("Delete a branch you're not on. It asks first, and says if commits would be lost.") : String("There's no other branch to delete (you can't delete the one you're on)."));
}

void GitDock::_on_branch_selected(int p_index) {
	const Variant target = branch_select->get_item_metadata(p_index);
	if (target.get_type() != Variant::STRING) {
		// An action: put the picker back on the current branch first.
		_fill_branches();
		switch (int(target)) {
			case BRANCH_RENAME:
				_show_rename_dialog();
				break;
			case BRANCH_DELETE:
				_show_delete_branch_dialog();
				break;
			default:
				_on_more_menu_id(MORE_NEW_BRANCH);
				break;
		}
		return;
	}
	if (String(target) == repo->get_current_branch()) {
		return;
	}
	const String addon = _addon_removed_by(target);
	if (!addon.is_empty()) {
		// Godot unloads an addon whose files disappear, so the panel would close mid-switch.
		pending_switch = target;
		_fill_branches(); // Shows the current branch unless the switch goes ahead.
		switch_confirm->set_text(vformat("\"%s\" doesn't include this Git panel (%s).\nSwitching removes it from the project, so the panel closes.\nSwitch back and restart the editor to get it back.", String(target), addon));
		switch_confirm->popup_centered();
		return;
	}
	_switch_branch(target);
}

// The addon's folder ("addons/godot_git") if switching to p_branch would delete it: it's
// committed on the current branch but missing on p_branch. Empty otherwise.
String GitDock::_addon_removed_by(const String &p_branch) const {
	const String manifest = godot_git::addon_manifest_path();
	if (manifest.is_empty()) {
		return String();
	}
	const String absolute = manifest.simplify_path();
	const String workdir = repo->get_workdir().simplify_path().trim_suffix("/") + "/";
	if (!absolute.begins_with(workdir)) {
		return String();
	}
	const String path = absolute.substr(workdir.length());
	if (!repo->has_file_at("HEAD", path) || repo->has_file_at(p_branch, path)) {
		return String();
	}
	return path.get_base_dir();
}

// p_branch is empty when it comes from the switch confirmation.
void GitDock::_switch_branch(const String &p_branch) {
	const String target = p_branch.is_empty() ? pending_switch : p_branch;
	pending_switch = String();
	if (target.is_empty() || !repo.is_valid()) {
		return;
	}
	if (_ask_to_save("Switch", callable_mp(this, &GitDock::_switch_branch).bind(target))) {
		_fill_branches(); // Shows the current branch until it's answered.
		return;
	}
	if (repo->uses_lfs()) {
		// May download LFS files: in the background, with progress and Cancel.
		network_branch = target;
		_fill_branches(); // Shows the current branch until the switch is done.
		_start_network(NETWORK_SWITCH);
		return;
	}
	_remember_open_scenes();
	const Error err = repo->checkout_branch(target);
	if (err == ERR_BUSY) {
		// Your changes are in the way: offer to set them aside (they wait under Stashes).
		pending_switch = target;
		stash_switch_confirm->set_text(vformat("%s\n\nStash them and switch to %s? They'll wait under Stashes, where you can restore them when you come back.", GitRepository::get_last_error(), target));
		stash_switch_confirm->popup_centered();
		return;
	}
	_report(err, "Switch branch");
	if (err == OK) {
		EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	}
	refresh();
	if (err == OK) {
		_set_status(STATUS_SUCCESS, vformat("Switched to %s", repo->get_current_branch()));
		_reload_changed_scenes(); // After the status: a scene it couldn't reload warns there.
	}
}

void GitDock::_on_branch_dialog_confirmed() {
	const String name = branch_name_edit->get_text().strip_edges();
	if (name.is_empty()) {
		return;
	}
	_report(repo->create_branch(name), "Create branch");
	refresh();
}

void GitDock::_build_branch_dialogs() {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();

	// Plain dialogs (not _make_confirm): their own text label would be an empty row above ours.
	rename_dialog = memnew(ConfirmationDialog);
	rename_dialog->set_title("Rename Branch");
	rename_dialog->set_ok_button_text("Rename");
	rename_dialog->connect("confirmed", callable_mp(this, &GitDock::_on_rename_confirmed));
	add_child(rename_dialog);
	VBoxContainer *rename_vb = memnew(VBoxContainer);
	rename_dialog->add_child(rename_vb);
	rename_label = memnew(Label);
	rename_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	rename_label->set_custom_minimum_size(Vector2(360 * scale, 0)); // Wraps at this width (gotcha 28).
	rename_vb->add_child(rename_label);
	rename_edit = memnew(LineEdit);
	rename_edit->set_placeholder("New name");
	rename_edit->connect("text_changed", callable_mp(this, &GitDock::_on_rename_text_changed));
	rename_vb->add_child(rename_edit);
	rename_dialog->register_text_enter(rename_edit);

	delete_branch_dialog = memnew(ConfirmationDialog);
	delete_branch_dialog->set_title("Delete Branch");
	delete_branch_dialog->connect("confirmed", callable_mp(this, &GitDock::_on_delete_branch_confirmed));
	add_child(delete_branch_dialog);
	VBoxContainer *delete_vb = memnew(VBoxContainer);
	delete_vb->add_theme_constant_override("separation", 8 * scale);
	delete_branch_dialog->add_child(delete_vb);
	delete_branch_select = memnew(OptionButton);
	delete_branch_select->connect("item_selected", callable_mp(this, &GitDock::_on_delete_branch_picked));
	delete_vb->add_child(delete_branch_select);
	delete_branch_label = memnew(Label);
	delete_branch_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	delete_branch_label->set_custom_minimum_size(Vector2(360 * scale, 0));
	delete_vb->add_child(delete_branch_label);
}

void GitDock::_show_rename_dialog() {
	const String current = repo->get_current_branch();
	const String upstream = repo->get_branch_details(current)["upstream"];
	rename_label->set_text(upstream.is_empty()
					? vformat("Rename %s to:", current)
					: vformat("Rename %s to:\n(It keeps following %s, which keeps its name.)", current, upstream));
	rename_edit->set_text(current);
	_on_rename_text_changed(current);
	rename_dialog->popup_centered();
	rename_edit->grab_focus();
	rename_edit->select_all();
}

void GitDock::_on_rename_text_changed(const String &p_text) {
	const String name = p_text.strip_edges();
	rename_dialog->get_ok_button()->set_disabled(name.is_empty() || name == repo->get_current_branch());
}

void GitDock::_on_rename_confirmed() {
	const String current = repo->get_current_branch();
	const String name = rename_edit->get_text().strip_edges();
	if (name.is_empty() || name == current) {
		return;
	}
	const Error err = repo->rename_branch(current, name);
	_report(err, "Rename branch");
	refresh();
	if (err == OK) {
		_set_status(STATUS_SUCCESS, vformat("Renamed %s to %s", current, name));
	}
}

void GitDock::_show_delete_branch_dialog() {
	const String current = repo->get_current_branch();
	PackedStringArray others = repo->get_branches();
	others.erase(current);
	if (others.is_empty()) {
		return;
	}
	others.sort();
	delete_branch_select->clear();
	for (const String &name : others) {
		delete_branch_select->add_icon_item(get_theme_icon("VcsBranches", "EditorIcons"), name);
	}
	delete_branch_select->select(0);
	_on_delete_branch_picked(0);
	delete_branch_dialog->popup_centered();
}

// Says what deleting the picked branch means: commits lost, or nothing lost.
void GitDock::_on_delete_branch_picked(int p_index) {
	const String name = delete_branch_select->get_item_text(p_index);
	const Dictionary details = repo->get_branch_details(name);
	const int unique = details["unique"];
	const String upstream = details["upstream"];
	String text;
	if (unique > 0) {
		text = vformat("%s has %s that no other branch has. Deleting it loses %s.", name, plural(unique, "commit", "commits"), unique == 1 ? String("it") : String("them"));
	} else if (unique == 0) {
		text = vformat("All of %s's commits are on other branches too, so nothing is lost.", name);
	} else {
		text = vformat("Couldn't check whether %s has commits no other branch has.", name);
	}
	if (!upstream.is_empty()) {
		text += vformat(" The branch it follows, %s, stays.", upstream);
	}
	delete_branch_label->set_text(text);
	delete_branch_label->add_theme_color_override("font_color", unique != 0 ? get_theme_color("warning_color", "Editor") : get_theme_color("font_color", "Label"));
	delete_branch_dialog->set_ok_button_text(unique != 0 ? "Delete Anyway" : "Delete");
}

void GitDock::_on_delete_branch_confirmed() {
	if (delete_branch_select->get_selected() < 0) {
		return;
	}
	const String name = delete_branch_select->get_item_text(delete_branch_select->get_selected());
	const Error err = repo->delete_branch(name);
	_report(err, "Delete branch");
	refresh();
	if (err == OK) {
		_set_status(STATUS_SUCCESS, vformat("Deleted branch %s", name));
	}
}
