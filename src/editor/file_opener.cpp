#include "editor/file_opener.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>

namespace godot_git {

namespace {

// Splits a command line the way Godot splits "exec_flags": spaces separate arguments,
// double quotes group them.
PackedStringArray split_command_line(const String &p_command_line) {
	PackedStringArray args;
	String current;
	bool quoted = false;
	for (int i = 0; i < p_command_line.length(); i++) {
		const char32_t c = p_command_line[i];
		if (c == '"') {
			quoted = !quoted;
		} else if (c == ' ' && !quoted) {
			if (!current.is_empty()) {
				args.push_back(current);
				current = String();
			}
		} else {
			current += String::chr(c);
		}
	}
	if (!current.is_empty()) {
		args.push_back(current);
	}
	return args;
}

// Path to a VS Code executable, or "" if it isn't installed in a usual place.
String find_vscode() {
	OS *os = OS::get_singleton();
	PackedStringArray candidates;
	if (os->get_name() == "Windows") {
		candidates.push_back(os->get_environment("LOCALAPPDATA").replace("\\", "/").path_join("Programs/Microsoft VS Code/Code.exe"));
		candidates.push_back(os->get_environment("ProgramFiles").replace("\\", "/").path_join("Microsoft VS Code/Code.exe"));
		// The `code` command on PATH lives in <install>/bin/code.cmd.
		for (const String &dir : os->get_environment("PATH").replace("\\", "/").split(";", false)) {
			if (FileAccess::file_exists(dir.path_join("code.cmd"))) {
				candidates.push_back(dir.path_join("../Code.exe").simplify_path());
			}
		}
	} else if (os->get_name() == "macOS") {
		candidates.push_back("/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code");
		candidates.push_back(os->get_environment("HOME").path_join("Applications/Visual Studio Code.app/Contents/Resources/app/bin/code"));
	} else {
		for (const String &dir : os->get_environment("PATH").split(":", false)) {
			candidates.push_back(dir.path_join("code"));
		}
		candidates.push_back("/snap/bin/code");
	}
	for (const String &candidate : candidates) {
		if (FileAccess::file_exists(candidate)) {
			return candidate;
		}
	}
	return String();
}

// Opens a file in the external editor configured in Godot, or in VS Code if none is configured.
// Returns false if neither is available.
bool open_in_code_editor(const String &p_absolute_path, const String &p_workdir) {
	Ref<EditorSettings> settings = EditorInterface::get_singleton()->get_editor_settings();
	String program = settings->get_setting("text_editor/external/exec_path");
	PackedStringArray args;

	if (!program.is_empty()) {
		// Same placeholders Godot itself fills in when it opens scripts externally.
		bool has_file = false;
		for (String arg : split_command_line(settings->get_setting("text_editor/external/exec_flags"))) {
			has_file = has_file || arg.contains("{file}");
			arg = arg.replace("{project}", ProjectSettings::get_singleton()->globalize_path("res://"));
			arg = arg.replace("{file}", p_absolute_path).replace("{line}", "1").replace("{col}", "1");
			args.push_back(arg);
		}
		if (!has_file) {
			args.push_back(p_absolute_path);
		}
	} else {
		program = find_vscode();
		if (program.is_empty()) {
			return false;
		}
		// Passing the repository folder makes VS Code reuse (or open) the window for this repo.
		args.push_back(p_workdir);
		args.push_back(p_absolute_path);
	}
	return OS::get_singleton()->create_process(program, args) != -1;
}

} // namespace

Error open_file(const String &p_absolute_path, const String &p_workdir) {
	if (!FileAccess::file_exists(p_absolute_path)) {
		return ERR_FILE_NOT_FOUND;
	}

	const String local = ProjectSettings::get_singleton()->localize_path(p_absolute_path);
	if (local.begins_with("res://")) {
		EditorInterface *editor = EditorInterface::get_singleton();
		const String type = editor->get_resource_filesystem()->get_file_type(local);
		if (type == "PackedScene") {
			editor->open_scene_from_path(local);
			return OK;
		}
		if (!type.is_empty() && ResourceLoader::get_singleton()->exists(local)) {
			Ref<Resource> res = ResourceLoader::get_singleton()->load(local);
			Ref<Script> script = res;
			if (script.is_valid()) {
				editor->edit_script(script);
				return OK;
			}
			if (res.is_valid()) {
				editor->edit_resource(res);
				return OK;
			}
		}
	}

	// C++ sources, README, project.godot, .uid files, ...: Godot's script editor can't be asked
	// to open those (TextFile and ScriptEditor::open_file aren't exposed), so they go to the
	// code editor.
	return open_in_code_editor(p_absolute_path, p_workdir) ? OK : ERR_UNAVAILABLE;
}

} // namespace godot_git
