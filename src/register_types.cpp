#include "register_types.h"

#include <gdextension_interface.h>
#include <git2.h>

#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "addon_files.h"
#include "editor/git_dock.h"
#include "editor/git_editor_plugin.h"
#include "git/git_lfs.h"
#include "git/git_repository.h"

using namespace godot;

namespace {

// Set once this copy of the library is kept loaded after its addon was removed.
bool retired = false;

// Godot unloads an extension whose .gdextension file disappears mid-session, which is what
// switching to a branch without the addon does. Engine objects we touched (EditorFileSystem,
// theme icons, ...) still hold godot-cpp binding callbacks into this library, and the engine
// calls them when it frees those objects later, so an unmapped library crashes the editor.
// Keeping it mapped leaves those callbacks harmless. Only then, so hot reload keeps working.
void keep_loaded_if_addon_removed() {
	if (godot_git::addon_manifest_path().is_empty()) {
		godot_git::keep_library_loaded();
		retired = true;
	}
}

// When the addon comes back in the same session (switching back to a branch that has it), the
// OS hands Godot this same, already shut down copy instead of loading the file again. godot-cpp
// can't initialize twice, so load as an empty extension and ask for a restart.
GDExtensionBool init_retired(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionInitialization *r_initialization) {
	const GDExtensionInterfacePrintWarning print_warning = reinterpret_cast<GDExtensionInterfacePrintWarning>(p_get_proc_address("print_warning"));
	if (print_warning) {
		print_warning("Godot Git: the addon was removed and restored while the editor was open (for example by switching branches). Restart the editor to use the Git panel again.", __FUNCTION__, __FILE__, __LINE__, true);
	}
	r_initialization->minimum_initialization_level = GDEXTENSION_INITIALIZATION_SCENE;
	r_initialization->userdata = nullptr;
	r_initialization->initialize = [](void *, GDExtensionInitializationLevel) {};
	r_initialization->deinitialize = [](void *, GDExtensionInitializationLevel) {};
	return true;
}

} // namespace

void initialize_godot_git_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		git_libgit2_init();
		godot_git::register_lfs_filter();
		GDREGISTER_CLASS(GitRepository);
	}

	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_INTERNAL_CLASS(GitDock);
		GDREGISTER_INTERNAL_CLASS(GitEditorPlugin);
		EditorPlugins::add_by_type<GitEditorPlugin>();
	}
}

void uninitialize_godot_git_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::remove_by_type<GitEditorPlugin>();
	}

	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		godot_git::shutdown_lfs();
		git_libgit2_shutdown();
		// godot-cpp frees its bindings on engine singletons (Time, OS, ...) only in its CORE step,
		// which it skips after a hot reload (Godot then initializes only SCENE and up). Godot frees
		// those singletons after unloading us, so a binding left behind crashed the editor on quit.
		// Harmless when the CORE step does run: a freed binding leaves the list it works from.
		ClassDB::deinitialize(GDEXTENSION_INITIALIZATION_CORE);
		keep_loaded_if_addon_removed();
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT godot_git_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	if (retired) {
		return init_retired(p_get_proc_address, r_initialization);
	}

	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_godot_git_module);
	init_obj.register_terminator(uninitialize_godot_git_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
