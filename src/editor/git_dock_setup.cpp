// Setting a repository up from the panel: Initialize Repository (when the project isn't in one),
// Add Remote (when it has none), and the name and email git needs before the first commit.
// Signing in and creating the repository on GitHub or elsewhere stay outside the panel; see
// CLAUDE.md, "Design decisions".

#include "editor/git_dock.h"

#include <godot_cpp/classes/button_group.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/core/math.hpp>

namespace {

// A wrapping label. In a dialog, give it p_width: a wrapping label measures its height at its
// minimum width, and at 0 that's one word per line, which makes the dialog as tall as the screen.
Label *make_label(Control *p_parent, const String &p_text, float p_width = 0) {
	Label *label = memnew(Label);
	label->set_text(p_text);
	label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	label->set_custom_minimum_size(Vector2(p_width, 0));
	p_parent->add_child(label);
	return label;
}

// The project folder, without a trailing slash.
String project_folder() {
	return ProjectSettings::get_singleton()->globalize_path("res://").trim_suffix("/");
}

// Whether p_folder is a sensible place to start a repository that also holds other things:
// not a drive or file system root, not the home folder, and not one of the big shared folders
// in it (a repository there would sweep up everything else in them).
bool is_repo_worthy(const String &p_folder) {
	if (p_folder.is_empty() || p_folder.get_base_dir() == p_folder || p_folder.ends_with(":") || p_folder.ends_with(":/")) {
		return false;
	}
	OS *os = OS::get_singleton();
	const String home = os->get_environment(os->get_name() == "Windows" ? "USERPROFILE" : "HOME").replace("\\", "/").trim_suffix("/");
	if (home.is_empty()) {
		return true;
	}
	// The home folder, or a folder containing it.
	if (p_folder.nocasecmp_to(home) == 0 || home.to_lower().begins_with(p_folder.to_lower() + "/")) {
		return false;
	}
	for (const char *shared : { "Desktop", "Documents", "Downloads", "OneDrive" }) {
		if (p_folder.nocasecmp_to(home.path_join(shared)) == 0) {
			return false;
		}
	}
	return true;
}

// "Also holds: docs, art, README.md and 3 more", what else a repository in p_folder would include.
String other_contents(const String &p_folder, const String &p_except) {
	PackedStringArray names;
	Ref<DirAccess> dir = DirAccess::open(p_folder);
	if (dir.is_valid()) {
		dir->set_include_hidden(false);
		for (const String &name : dir->get_directories()) {
			if (name != p_except) {
				names.push_back(vformat("%s/", name));
			}
		}
		names.append_array(dir->get_files());
	}
	if (names.is_empty()) {
		return "Nothing else is in it yet.";
	}
	const int shown = MIN(4, (int)names.size());
	String text = vformat("Also holds %s", String(", ").join(names.slice(0, shown)));
	if (names.size() > shown) {
		text += vformat(" and %d more", names.size() - shown);
	}
	return text + ".";
}

} // namespace

void GitDock::_build_setup(Control *p_parent) {
	const float scale = EditorInterface::get_singleton()->get_editor_scale();

	// Empty state: the project isn't in a repository.
	MarginContainer *empty = memnew(MarginContainer);
	for (const char *side : { "margin_left", "margin_right", "margin_top" }) {
		empty->add_theme_constant_override(side, Math::round(8 * scale));
	}
	empty->hide();
	p_parent->add_child(empty);
	no_repo_ui = empty;

	VBoxContainer *empty_vb = memnew(VBoxContainer);
	empty_vb->add_theme_constant_override("separation", Math::round(8 * scale));
	empty->add_child(empty_vb);
	Label *title = make_label(empty_vb, "This project isn't in a git repository yet.");
	title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	no_repo_hint = make_label(empty_vb, "Start one to keep a history of your changes: commit them, go back to earlier versions and share them.");
	no_repo_hint->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	Button *init_button = memnew(Button);
	init_button->set_text("Initialize Repository...");
	init_button->set_h_size_flags(SIZE_SHRINK_CENTER);
	init_button->connect("pressed", callable_mp(this, &GitDock::_show_init_dialog));
	empty_vb->add_child(init_button);

	// Initialize: in the project folder, or the folder above it. Both are found by the panel
	// (it searches up from the project), so nothing needs configuring afterwards.
	init_dialog = memnew(ConfirmationDialog);
	init_dialog->set_title("Initialize Repository");
	init_dialog->set_ok_button_text("Initialize");
	init_dialog->connect("confirmed", callable_mp(this, &GitDock::_on_init_confirmed));
	add_child(init_dialog);

	VBoxContainer *init_vb = memnew(VBoxContainer);
	init_vb->set_custom_minimum_size(Vector2(440 * scale, 0));
	init_vb->add_theme_constant_override("separation", Math::round(6 * scale));
	init_dialog->add_child(init_vb);
	const float width = 440 * scale;
	init_question = make_label(init_vb, String(), width);

	Ref<ButtonGroup> where;
	where.instantiate();
	init_here = memnew(CheckBox);
	init_here->set_text("This project folder");
	init_here->set_button_group(where);
	init_vb->add_child(init_here);
	init_here_indent = memnew(MarginContainer);
	init_vb->add_child(init_here_indent);
	init_here_path = make_label(init_here_indent, String(), width);

	VBoxContainer *parent_vb = memnew(VBoxContainer);
	parent_vb->add_theme_constant_override("separation", Math::round(6 * scale));
	init_vb->add_child(parent_vb);
	init_parent_box = parent_vb;
	init_parent = memnew(CheckBox);
	init_parent->set_text("The folder above it, to keep other files next to the project");
	init_parent->set_button_group(where);
	parent_vb->add_child(init_parent);
	init_parent_indent = memnew(MarginContainer);
	parent_vb->add_child(init_parent_indent);
	init_parent_path = make_label(init_parent_indent, String(), width);

	make_label(init_vb, "Godot's .gitignore and .gitattributes are added if the project doesn't have them yet. Nothing is committed: your files show up as changes for your first commit.", 440 * scale);

	// Add Remote: only the URL. The repository itself is created on GitHub or elsewhere first.
	remote_dialog = memnew(ConfirmationDialog);
	remote_dialog->set_title("Add Remote");
	remote_dialog->set_ok_button_text("Add");
	remote_dialog->connect("confirmed", callable_mp(this, &GitDock::_on_remote_confirmed));
	add_child(remote_dialog);

	VBoxContainer *remote_vb = memnew(VBoxContainer);
	remote_vb->set_custom_minimum_size(Vector2(440 * scale, 0));
	remote_vb->add_theme_constant_override("separation", Math::round(6 * scale));
	remote_dialog->add_child(remote_vb);
	make_label(remote_vb, "Create an empty repository on GitHub, GitLab or another host, then paste its URL here:", 440 * scale);
	remote_url_edit = memnew(LineEdit);
	remote_url_edit->set_placeholder("https://github.com/you/your-game.git");
	remote_url_edit->connect("text_changed", callable_mp(this, &GitDock::_on_remote_url_changed));
	remote_vb->add_child(remote_url_edit);
	remote_dialog->register_text_enter(remote_url_edit);
	make_label(remote_vb, "It's added as \"origin\". Publish then sends your branch there, and asks you to sign in if needed.", 440 * scale);

	// Name and email: asked for right when they're needed, then the commit (or pull) goes ahead.
	identity_dialog = memnew(ConfirmationDialog);
	identity_dialog->set_title("Your Name and Email");
	identity_dialog->connect("confirmed", callable_mp(this, &GitDock::_on_identity_confirmed));
	add_child(identity_dialog);

	VBoxContainer *identity_vb = memnew(VBoxContainer);
	identity_vb->set_custom_minimum_size(Vector2(440 * scale, 0));
	identity_vb->add_theme_constant_override("separation", Math::round(6 * scale));
	identity_dialog->add_child(identity_vb);
	make_label(identity_vb, "Every commit records who made it, and git doesn't know your name and email yet.", 440 * scale);
	identity_name_edit = memnew(LineEdit);
	identity_name_edit->set_placeholder("Your name");
	identity_name_edit->connect("text_changed", callable_mp(this, &GitDock::_on_identity_changed));
	identity_vb->add_child(identity_name_edit);
	identity_email_edit = memnew(LineEdit);
	identity_email_edit->set_placeholder("you@example.com");
	identity_email_edit->connect("text_changed", callable_mp(this, &GitDock::_on_identity_changed));
	identity_vb->add_child(identity_email_edit);
	identity_dialog->register_text_enter(identity_email_edit);
	make_label(identity_vb, "Anyone who can see the repository can see these. GitHub can give you a private email address to use instead (Settings > Emails).", 440 * scale);
	identity_local_check = memnew(CheckBox);
	identity_local_check->set_text("Only for this repository");
	identity_local_check->set_tooltip_text("Otherwise they're saved in your git settings and used for all your repositories, in the terminal and other git tools too.");
	identity_vb->add_child(identity_local_check);
}

void GitDock::_show_init_dialog() {
	const String here = project_folder();
	const String parent = here.get_base_dir();
	init_here_path->set_text(here);
	init_parent_path->set_text(vformat("%s\n%s", parent, other_contents(parent, here.get_file())));
	for (Label *label : { init_here_path, init_parent_path }) {
		label->add_theme_color_override("font_color", _dim_color());
	}
	// Only one place to offer: no choice to make, just say where.
	const bool choice = is_repo_worthy(parent);
	init_parent_box->set_visible(choice);
	init_here->set_visible(choice);
	init_question->set_text(choice ? String("Where should the repository start?") : String("The repository starts in the project folder:"));
	init_here->set_pressed(true);
	// Paths line up with the choices' text: past the radio icon and its gap.
	const int indent = choice ? init_here->get_theme_icon("radio_unchecked", "CheckBox")->get_width() + init_here->get_theme_constant("h_separation", "CheckBox") + init_here->get_theme_stylebox("normal", "CheckBox")->get_margin(SIDE_LEFT) : 0;
	for (MarginContainer *margin : { init_here_indent, init_parent_indent }) {
		margin->add_theme_constant_override("margin_left", indent);
	}
	init_dialog->popup_centered(Vector2i(480, 0) * EditorInterface::get_singleton()->get_editor_scale());
}

void GitDock::_on_init_confirmed() {
	const String here = project_folder();
	const String folder = init_parent->is_pressed() && init_parent_box->is_visible() ? here.get_base_dir() : here;
	const Error err = GitRepository::init_repository(folder, here);
	if (err != OK) {
		// The dock has no strip without a repository; a toast is all there is.
		EditorInterface::get_singleton()->get_editor_toaster()->push_toast(vformat("Git: Couldn't initialize a repository. %s", GitRepository::get_last_error()), EditorToaster::SEVERITY_ERROR);
		return;
	}
	refresh();
	_set_status(STATUS_SUCCESS, vformat("Started a repository in %s. Stage your files and commit to begin its history.", folder == here ? String("the project folder") : vformat("%s/, the folder above the project", folder.get_file())));
}

void GitDock::_show_remote_dialog() {
	remote_url_edit->clear();
	remote_dialog->get_ok_button()->set_disabled(true);
	remote_dialog->popup_centered(Vector2i(480, 0) * EditorInterface::get_singleton()->get_editor_scale());
	remote_url_edit->grab_focus();
}

void GitDock::_on_remote_url_changed(const String &p_text) {
	remote_dialog->get_ok_button()->set_disabled(p_text.strip_edges().is_empty());
}

void GitDock::_on_remote_confirmed() {
	const String url = remote_url_edit->get_text().strip_edges();
	if (url.is_empty()) {
		return;
	}
	const Error err = repo->add_remote("origin", url);
	_report(err, "Adding the remote");
	refresh();
	if (err == OK) {
		_set_status(STATUS_SUCCESS, has_commits ? String("Added the remote origin. Publish sends your branch there.") : String("Added the remote origin. Make a first commit, then Publish sends it there."));
	}
}

// If git doesn't know the user's name and email, asks for them and returns true; p_then
// (NETWORK_COMMIT or NETWORK_PULL) runs once they're saved. False if they're known.
bool GitDock::_ask_identity(NetworkOp p_then) {
	const Dictionary identity = repo->get_identity();
	const String name = identity.get("name", String());
	const String email = identity.get("email", String());
	if (!name.is_empty() && !email.is_empty()) {
		return false;
	}
	identity_then = p_then;
	identity_name_edit->set_text(name);
	identity_email_edit->set_text(email);
	identity_local_check->set_pressed(false);
	identity_dialog->set_ok_button_text(p_then == NETWORK_PULL ? "Save and Pull" : "Save and Commit");
	_on_identity_changed(String());
	identity_dialog->popup_centered(Vector2i(480, 0) * EditorInterface::get_singleton()->get_editor_scale());
	(name.is_empty() ? identity_name_edit : identity_email_edit)->grab_focus();
	return true;
}

void GitDock::_on_identity_changed(const String &p_text) {
	identity_dialog->get_ok_button()->set_disabled(identity_name_edit->get_text().strip_edges().is_empty() || identity_email_edit->get_text().strip_edges().is_empty());
}

void GitDock::_on_identity_confirmed() {
	if (identity_dialog->get_ok_button()->is_disabled()) {
		return; // Enter in the email field with a field still empty.
	}
	const Error err = repo->set_identity(identity_name_edit->get_text(), identity_email_edit->get_text(), !identity_local_check->is_pressed());
	if (err != OK) {
		_report(err, "Saving your name and email");
		return;
	}
	const NetworkOp then = identity_then;
	identity_then = NETWORK_NONE;
	if (then == NETWORK_PULL) {
		_start_network(NETWORK_PULL);
	} else if (then == NETWORK_COMMIT) {
		_commit();
	}
}
