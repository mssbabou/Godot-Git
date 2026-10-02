@tool
extends EditorPlugin
## The switch for Godot Git under Project Settings > Plugins. The extension itself loads the Git
## dock and the Git Diff panel, and shows them while this plugin is on. Plain GDScript on purpose:
## a plugin made of the extension's own class crashed the editor when the addon's folder went away
## (switching to a branch without it).
