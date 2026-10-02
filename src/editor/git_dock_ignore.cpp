// GitDock: Ignore..., for a new file nobody wants in git: this file, every file of its type, or a
// folder it's in. The dialog shows what the rule hides (git's own matching, before anything is
// written) and where it goes.

#include "editor/git_dock.h"

#include <godot_cpp/classes/button_group.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include "editor/git_dock_util.h"
#include "editor/ui_text.h"

using namespace godot_git;

namespace {

enum IgnoreChoice {
	IGNORE_FILE,
	IGNORE_TYPE,
	IGNORE_FOLDER,
};

} // namespace

void GitDock::_build_ignore_dialog() {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	ignore_dialog = memnew(ConfirmationDialog);
	ignore_dialog->set_title("Ignore");
	ignore_dialog->set_ok_button_text("Add");
	ignore_dialog->connect("confirmed", callable_mp(this, &GitDock::_on_ignore_confirmed));
	add_child(ignore_dialog);

	VBoxContainer *vb = memnew(VBoxContainer);
	vb->add_theme_constant_override("separation", 6 * scale);
	ignore_dialog->add_child(vb);
	Label *intro = memnew(Label);
	intro->set_text("Keep out of git, so it no longer shows in Changes:");
	vb->add_child(intro);

	Ref<ButtonGroup> group;
	group.instantiate();
	for (int i = 0; i < 3; i++) {
		HBoxContainer *row = memnew(HBoxContainer);
		vb->add_child(row);
		CheckBox *option = memnew(CheckBox);
		option->set_button_group(group);
		option->connect("pressed", callable_mp(this, &GitDock::_update_ignore_effect));
		row->add_child(option);
		ignore_options[i] = option;
		if (i == IGNORE_FOLDER) {
			ignore_folder_select = memnew(OptionButton);
			ignore_folder_select->connect("item_selected", callable_mp(this, &GitDock::_on_ignore_folder_selected));
			row->add_child(ignore_folder_select);
		}
	}

	ignore_effect_label = memnew(Label);
	ignore_effect_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	ignore_effect_label->set_custom_minimum_size(Vector2(420 * scale, 0)); // Gotcha 28.
	ignore_effect_label->set_modulate(Color(1, 1, 1, 0.65)); // Like the Git Settings dialog's notes.
	vb->add_child(ignore_effect_label);
}

// Ignore... for one new file (repository path), from the Changes list or the FileSystem dock.
void GitDock::show_ignore(const String &p_path) {
	if (!can_ignore(p_path)) {
		return;
	}
	ignore_path = p_path;
	ignore_file = _ignore_file_for(p_path);

	ignore_untracked.clear();
	for (const Variant &entry : repo->get_status()) {
		const Dictionary item = entry;
		if (String(item["worktree"]) == "untracked") {
			ignore_untracked.push_back(item["path"]);
		}
	}

	const String name = p_path.get_file();
	const String extension = name.get_extension();
	ignore_options[IGNORE_FILE]->set_text(vformat("This file: %s", name));
	ignore_options[IGNORE_TYPE]->set_text(vformat("Every .%s file", extension));
	ignore_options[IGNORE_TYPE]->set_visible(!extension.is_empty() && !name.begins_with("."));
	ignore_options[IGNORE_FOLDER]->set_text("Everything in");
	// The folders the file is in, nearest first, as far up as the .gitignore's own folder (a rule
	// there can't reach above it, and ignoring that folder itself would ignore the project).
	ignore_folder_select->clear();
	const String file_dir = ignore_file.contains("/") ? ignore_file.get_base_dir() : String();
	for (String folder = p_path.get_base_dir(); !folder.is_empty() && folder != file_dir; folder = folder.contains("/") ? folder.get_base_dir() : String()) {
		if (!file_dir.is_empty() && !folder.begins_with(vformat("%s/", file_dir))) {
			break;
		}
		ignore_folder_select->add_item(vformat("%s/", folder.trim_prefix(file_dir.is_empty() ? String() : vformat("%s/", file_dir))));
		ignore_folder_select->set_item_metadata(-1, folder);
	}
	ignore_options[IGNORE_FOLDER]->get_parent_control()->set_visible(ignore_folder_select->get_item_count() > 0);
	ignore_options[IGNORE_FILE]->set_pressed(true);
	_update_ignore_effect();
	ignore_dialog->popup_centered();
}

// The .gitignore a rule for p_path goes into: next to project.godot, or the nearest one above it;
// for a file outside the project folder, the repository root's.
String GitDock::_ignore_file_for(const String &p_path) const {
	const String project = get_repo_path("res://");
	const bool in_project = project.is_empty() || p_path.begins_with(vformat("%s/", project));
	return repo->get_ignore_file(in_project ? project : String());
}

// The lines that ignore just p_path (and its companions in p_untracked) from p_ignore_file.
PackedStringArray GitDock::_file_ignore_lines(const String &p_path, const String &p_ignore_file, const PackedStringArray &p_untracked) const {
	const String file_dir = p_ignore_file.contains("/") ? p_ignore_file.get_base_dir() : String();
	const String relative = file_dir.is_empty() ? p_path : p_path.trim_prefix(vformat("%s/", file_dir));
	PackedStringArray lines;
	lines.push_back(vformat("/%s", relative));
	for (const char *suffix : { ".import", ".uid" }) {
		if (p_untracked.has(vformat("%s%s", p_path, suffix))) {
			lines.push_back(vformat("/%s%s", relative, suffix));
		}
	}
	return lines;
}

// Only files git doesn't track yet: un-tracking a committed file deletes it from your teammates'
// disks when they pull, so that's left to the terminal (git rm --cached) for now.
bool GitDock::can_ignore(const String &p_path) const {
	if (repo.is_null() || !repo->is_open() || p_path.is_empty()) {
		return false;
	}
	for (const Variant &entry : repo->get_status()) {
		const Dictionary item = entry;
		if (String(item["path"]) == p_path) {
			return String(item["worktree"]) == "untracked";
		}
	}
	return false;
}

void GitDock::_on_ignore_folder_selected(int p_index) {
	ignore_options[IGNORE_FOLDER]->set_pressed(true);
	_update_ignore_effect();
}

// The lines the chosen option adds, relative to the .gitignore's folder, with Godot's companion
// files (.import, .uid) of what's ignored, so they don't stay behind as changes of their own. A
// folder rule takes them along anyway.
PackedStringArray GitDock::_ignore_lines() const {
	const String file_dir = ignore_file.contains("/") ? ignore_file.get_base_dir() : String();
	auto relative = [&](const String &p_path) -> String {
		return file_dir.is_empty() ? p_path : p_path.trim_prefix(vformat("%s/", file_dir));
	};
	PackedStringArray lines;
	if (ignore_options[IGNORE_FOLDER]->is_pressed() && ignore_folder_select->get_selected() >= 0) {
		lines.push_back(vformat("/%s/", relative(ignore_folder_select->get_selected_metadata())));
	} else if (ignore_options[IGNORE_TYPE]->is_pressed()) {
		const String extension = ignore_path.get_extension();
		lines.push_back(vformat("*.%s", extension));
		for (const char *suffix : { ".import", ".uid" }) {
			for (const String &path : ignore_untracked) {
				if (path.ends_with(vformat(".%s%s", extension, suffix))) {
					lines.push_back(vformat("*.%s%s", extension, suffix));
					break;
				}
			}
		}
	} else {
		lines = _file_ignore_lines(ignore_path, ignore_file, ignore_untracked);
	}
	return lines;
}

// Under the options: what the rule hides, and what's written where.
void GitDock::_update_ignore_effect() {
	const PackedStringArray lines = _ignore_lines();
	ignore_hidden = repo->get_paths_ignored_by(ignore_file, lines, ignore_untracked);
	PackedStringArray names;
	for (const String &path : ignore_hidden) {
		names.push_back(path.get_file());
	}
	String hides = ignore_hidden.is_empty() ? String("Hides nothing that's in Changes now.") : vformat("Hides %s from Changes: %s.", plural(ignore_hidden.size(), "file", "files"), join_list(names, 4));
	// A rule also covers files that don't exist yet, and never untracks committed ones.
	if (ignore_options[IGNORE_TYPE]->is_pressed()) {
		hides += vformat(" New .%s files won't show up either; ones already in git stay in git.", ignore_path.get_extension());
	} else if (ignore_options[IGNORE_FOLDER]->is_pressed()) {
		hides += vformat(" New files in %s won't show up either; ones already in git stay in git.", ignore_folder_select->get_text());
	}
	const bool exists = FileAccess::file_exists(repo->get_workdir().path_join(ignore_file));
	ignore_effect_label->set_text(vformat("%s\n\n%s %s:\n%s", hides, exists ? "Adds to" : "Creates", ignore_file, String("\n").join(lines)));
	ignore_dialog->get_ok_button()->set_disabled(ignore_hidden.is_empty());
	ignore_dialog->reset_size();
}

void GitDock::_on_ignore_confirmed() {
	const PackedStringArray lines = _ignore_lines();
	const int hidden = ignore_hidden.size();
	const Error err = repo->add_ignore_lines(ignore_file, lines);
	_report(err, "Ignore");
	refresh();
	if (err == OK) {
		_set_status(STATUS_SUCCESS, vformat("Ignored %s (%s), in %s", lines[0], plural(hidden, "file", "files"), ignore_file));
	}
}
