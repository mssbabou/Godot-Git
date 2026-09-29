// The branch picker: listing branches, switching (in the background when LFS files may need
// downloading), and New Branch.

#include "editor/git_dock.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>

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
	branch_select->add_separator();
	branch_select->add_icon_item(get_theme_icon("Add", "EditorIcons"), "New Branch...");
	branch_select->set_item_metadata(branch_select->get_item_count() - 1, Variant());
}

void GitDock::_on_branch_selected(int p_index) {
	const Variant target = branch_select->get_item_metadata(p_index);
	if (target.get_type() != Variant::STRING) {
		// "New Branch..." item: put the picker back and ask for a name.
		refresh();
		_on_more_menu_id(MORE_NEW_BRANCH);
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
