#include "register_types.h"

#include <gdextension_interface.h>
#include <git2.h>

#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "editor/git_dock.h"
#include "editor/git_editor_plugin.h"
#include "git/git_repository.h"

using namespace godot;

void initialize_godot_git_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		git_libgit2_init();
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
		git_libgit2_shutdown();
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT godot_git_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_godot_git_module);
	init_obj.register_terminator(uninitialize_godot_git_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
