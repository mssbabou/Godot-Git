#pragma once

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace godot_git {

// Opens a file the way the Git dock promises, and never anything else:
// - in Godot when Godot can edit it (scenes, scripts, other resources; scripts follow Godot's
//   own "use external editor" setting);
// - otherwise in the external editor from Editor Settings > Text Editor > External, or VS Code
//   (in the window for p_workdir, the repository folder).
// Returns ERR_FILE_NOT_FOUND if the file doesn't exist (e.g. deleted), ERR_UNAVAILABLE if it
// needs a code editor and none was found.
Error open_file(const String &p_absolute_path, const String &p_workdir);

} // namespace godot_git
