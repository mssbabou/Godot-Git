@tool
extends EditorPlugin
## Smoke test of the Git dock and the Diff panel in a real (headless) editor. The backend suite
## (project/tests) never creates the dock, so it can't see the dock crash the editor; this does
## the things a person does, checks the results, and prints "SMOKE: OK" or what failed.
##
##   python tools/make_playground.py <folder> --smoke
##   godot --headless -e --path <folder> --quit-after 300     (first run registers the extension)
##   godot --headless -e --path <folder> --quit-after 20000   (the test; exit code 1 on failure)
##
## Limits: headless, so nothing is drawn (row and gutter drawing aren't exercised), and clicks are
## emitted signals, not real mouse input, which skips the Tree's own click handling (a Tree
## refuses to create rows while it handles a real click). A crash still ends the run without
## "SMOKE: OK", which is what CI checks for.

var failures := 0
var base: Control
var dock: Control
var diff: Control


func _enter_tree() -> void:
	_run.call_deferred()


func _check(ok: bool, what: String) -> void:
	print("SMOKE: ", "ok   " if ok else "FAIL ", what)
	if not ok:
		failures += 1


func _frames(count := 5) -> void:
	for i in count:
		await get_tree().process_frame


func _section(title: String) -> FoldableContainer:
	for f: FoldableContainer in dock.find_children("*", "FoldableContainer", true, false):
		if f.title == title or f.title.begins_with(title + " ("): # "Changes (9)"
			return f
	return null


func _tree(title: String) -> Tree:
	return _section(title).find_children("*", "Tree", true, false)[0]


func _row(tree: Tree, match: Callable) -> TreeItem:
	var stack: Array[TreeItem] = [tree.get_root()]
	while not stack.is_empty():
		var item: TreeItem = stack.pop_back()
		if item != tree.get_root() and match.call(item):
			return item
		for child in item.get_children():
			stack.push_back(child)
	return null


func _file_row(title: String, path: String) -> TreeItem:
	return _row(_tree(title), func(item: TreeItem) -> bool: return item.get_meta("git_path", "") == path)


func _commit_row(summary: String) -> TreeItem:
	return _row(_tree("History"), func(item: TreeItem) -> bool: return item.get_meta("git_row", "") == "commit" and item.get_text(0) == summary)


func _click(tree: Tree, item: TreeItem) -> void:
	tree.item_mouse_selected.emit(tree.get_item_area_rect(item).position + Vector2(4, 4), MOUSE_BUTTON_LEFT)


## Every label text in a control, internal children included (section headers are internal).
func _texts(root: Node) -> PackedStringArray:
	var texts := PackedStringArray()
	var stack: Array[Node] = [root]
	while not stack.is_empty():
		var node: Node = stack.pop_back()
		if node is Label and node.text != "":
			texts.append(node.text)
		for child in node.get_children(true):
			if not child is Tree:
				stack.push_back(child)
	return texts


## The index of the menu item called p_text, or -1.
func _menu_index(menu: PopupMenu, text: String) -> int:
	for i in menu.item_count:
		if menu.get_item_text(i) == text:
			return i
	return -1


## The popup menu under p_root that has an item called p_text (the right-click menu).
func _menu_with(root: Node, text: String) -> PopupMenu:
	for menu: PopupMenu in root.find_children("*", "PopupMenu", true, false):
		if _menu_index(menu, text) >= 0:
			return menu
	return null


## The text of the Diff panel's code views (all panes; only the visible view has lines).
func _diff_code() -> String:
	var text := ""
	for edit: CodeEdit in diff.find_children("*", "CodeEdit", true, false):
		text += edit.text + "\n"
	return text


func _run() -> void:
	var deadline := get_tree().create_timer(180.0)
	deadline.timeout.connect(func() -> void:
		print("SMOKE: FAIL gave up after 3 minutes")
		get_tree().quit(1))
	await get_tree().create_timer(3.0).timeout

	base = EditorInterface.get_base_control()
	var docks := base.find_children("*", "GitDock", true, false)
	var diffs := base.find_children("*", "GitDiffDock", true, false)
	_check(docks.size() == 1 and diffs.size() == 1, "the Git dock and the Diff panel exist")
	if docks.is_empty() or diffs.is_empty():
		get_tree().quit(1)
		return
	dock = docks[0]
	diff = diffs[0]
	dock.make_visible() # So trees have a size and rows a position.
	diff.make_visible()
	await _frames()

	# The ⋮ menu ends with where this library was built (a CI build names its commit).
	var more_button: MenuButton = dock.find_children("*", "MenuButton", true, false)[0]
	more_button.get_popup().about_to_popup.emit()
	var build_line := more_button.get_popup().get_item_text(more_button.get_popup().item_count - 1)
	print("SMOKE: ", build_line)
	_check(build_line.begins_with("Godot Git "), "the ⋮ menu says where this build comes from")

	# Refreshing twice: the second takes the "nothing changed" paths (History kept as is).
	dock.refresh()
	dock.refresh()
	await _frames()
	_check(_file_row("Changes", "player.gd") != null, "an unstaged file is listed")
	_check(_file_row("Staged Changes", "enemy.gd") != null, "a staged file is listed")

	# Changed files are colored in the FileSystem dock (its own Tree, recolored by the panel).
	var fs_tree: Tree = EditorInterface.get_file_system_dock().find_children("*", "Tree", true, false)[0]
	var fs_player := _row(fs_tree, func(item: TreeItem) -> bool: return item.get_metadata(0) == "res://player.gd")
	_check(fs_player != null and fs_player.has_meta("godot_git_previous_color"), "a changed file is colored in the FileSystem dock")

	# Folding and unfolding each section. Folding once sent the header alignment into an endless
	# layout loop that crashed the editor after a moment, so each stays folded for a while.
	for title in ["Staged Changes", "Changes", "History"]:
		_section(title).folded = true
		await get_tree().create_timer(2.0).timeout
		_section(title).folded = false
		await _frames()
	_check(true, "folding and unfolding every section")

	# Clicking a file shows its diff.
	var changes := _tree("Changes")
	var player := _file_row("Changes", "player.gd")
	player.select(0)
	changes.multi_selected.emit(player, 0, true)
	_click(changes, player)
	await _frames()
	_check(_texts(diff).has("player.gd") and _texts(diff).has("Unstaged"), "clicking a file shows it in the Diff panel")
	_check(_diff_code().contains("ACCELERATION"), "the diff has the changed lines")

	var views: OptionButton = diff.find_children("*", "OptionButton", true, false)[0]
	for view in [1, 0]:
		views.select(view)
		views.item_selected.emit(view)
		await _frames()
		_check(_diff_code().contains("ACCELERATION"), "the diff in the %s view" % ["unified", "side by side"][view])

	# Images: before | after instead of "binary file".
	var shows := func(title: String, path: String) -> PackedStringArray:
		var tree := _tree(title)
		var item := _file_row(title, path)
		item.select(0)
		tree.multi_selected.emit(item, 0, true)
		await _frames()
		return _texts(diff)
	var texts: PackedStringArray = await shows.call("Changes", "art/player.png")
	_check(Array(texts).any(func(t: String) -> bool: return t.begins_with("Before · 32×32")) and Array(texts).any(func(t: String) -> bool: return t.begins_with("After · 32×32")), "a changed image shows before and after")
	texts = await shows.call("Changes", "art/coin.png")
	_check(Array(texts).any(func(t: String) -> bool: return t.begins_with("Not in the old version")), "a new image says it's new")
	texts = await shows.call("Staged Changes", "art/background.png")
	_check(Array(texts).any(func(t: String) -> bool: return t.ends_with("same pixels")), "an image saved again with the same pixels says so")
	texts = await shows.call("Changes", "icon.svg")
	_check(Array(texts).any(func(t: String) -> bool: return t.begins_with("After · 64×64")) and views.get_item_count() == 3, "an SVG shows as an image, with its text diff one choice away")

	# Staging the shown file: it moves to Staged Changes, and the Diff panel follows it.
	player = _file_row("Changes", "player.gd")
	changes.deselect_all() # Only this file: the images above were selected too.
	player.select(0)
	changes.multi_selected.emit(player, 0, true)
	await _frames()
	# Through the right-click menu (the row's own buttons need a real mouse: see CLAUDE.md, gotcha 41).
	changes.item_mouse_selected.emit(Vector2(), MOUSE_BUTTON_RIGHT)
	await _frames()
	var menu := _menu_with(dock, "Stage")
	_check(menu != null, "right-clicking a changed file offers Stage")
	if menu:
		menu.id_pressed.emit(menu.get_item_id(_menu_index(menu, "Stage")))
		menu.hide()
	await _frames()
	_check(_file_row("Staged Changes", "player.gd") != null, "staging a file")
	_check(_texts(diff).has("Staged"), "the Diff panel follows the staged file")

	# History: 50 commits, then Load More, then a commit's files and a file's diff.
	var history := _tree("History")
	var more := _row(history, func(item: TreeItem) -> bool: return item.get_meta("git_row", "") == "more")
	_check(more != null, "History offers Load More Commits")
	if more:
		_click(history, more)
		await _frames()
	_check(_commit_row("Initial platformer") != null, "Load More reaches the first commit")
	var commit := _commit_row("Make enemies hit harder")
	if commit:
		_click(history, commit) # Expands it; the files are filled in right after.
		await _frames()
		var file := _row(history, func(item: TreeItem) -> bool: return item.get_meta("git_row", "") == "file" and item.get_parent() == commit)
		_check(file != null and file.get_meta("git_path") == "enemy.gd", "expanding a commit lists its files")
		if file:
			file.select(0)
			_click(history, file)
			await _frames()
			_check(_texts(diff).has("enemy.gd") and _diff_code().contains("damage := 2"), "a commit's file shows its change in the Diff panel")
		dock.refresh()
		await _frames()
		_check(not commit.collapsed and history.get_selected() != null, "an expanded commit and the selection survive a refresh")

	# A commit's .uid is shown on its script's row, not as a row of its own.
	var hud := _commit_row("Add a health HUD")
	_check(hud != null, "History lists the merged branch's commit")
	if hud:
		_click(history, hud)
		await _frames()
		var script := _row(history, func(item: TreeItem) -> bool: return item.get_parent() == hud and item.get_meta("git_path", "") == "hud.gd")
		var uid := _row(history, func(item: TreeItem) -> bool: return item.get_parent() == hud and item.get_meta("git_path", "") == "hud.gd.uid")
		_check(script != null and uid == null and PackedStringArray(script.get_meta("git_companions", PackedStringArray())).has("hud.gd.uid"), "a commit's .uid is shown on its file's row")

	# One file's history, and a search.
	dock.show_file_history("enemy.gd")
	await _frames()
	var summaries := PackedStringArray()
	for item in _tree("History").get_root().get_children():
		if item.get_meta("git_row", "") == "commit":
			summaries.append(item.get_text(0))
	_check(summaries == PackedStringArray(["Make enemies hit harder", "Initial platformer"]) and _texts(_section("History")).has("enemy.gd"), "a file's history lists only its commits (%s)" % ", ".join(summaries))
	var search: LineEdit = null
	for edit: LineEdit in _section("History").find_children("*", "LineEdit", true, false):
		if edit.placeholder_text == "Search commits":
			search = edit
	for button: Button in _section("History").find_children("*", "Button", true, false):
		if button.tooltip_text == "Show every commit again.":
			button.pressed.emit()
	if search:
		search.text = "clamp"
		search.text_submitted.emit("clamp")
		await _frames()
	var found := PackedStringArray()
	for item in _tree("History").get_root().get_children():
		if item.get_meta("git_row", "") == "commit":
			found.append(item.get_text(0))
	_check(search != null and found == PackedStringArray(["Rename clamp01"]), "searching History finds a commit (%s)" % ", ".join(found))
	if search:
		search.text = ""
		search.text_submitted.emit("")
		await _frames()

	# Line counts arrive from the background after an edit.
	var coin := FileAccess.open("res://coin.gd", FileAccess.READ_WRITE)
	coin.seek_end()
	coin.store_string("\n\nfunc value() -> int:\n\treturn 1\n")
	coin.close()
	dock.refresh()
	var counted := false
	for i in 200:
		await get_tree().create_timer(0.05).timeout
		if Array(_texts(_section("Changes"))).any(func(t: String) -> bool: return t.begins_with("+")):
			counted = true
			break
	_check(counted, "line counts arrive after an edit")

	# Stash last, and driven by timers, not await: stashing changes files on disk, scripts among
	# them, and Godot then reloads scripts, which cancels every await in progress (this script's
	# too; it then spun on "Object was deleted while awaiting a callback" until the editor crashed).
	get_tree().root.set_meta("smoke_failures", failures)
	_stash_press()


func _smoke_check(ok: bool, what: String) -> void:
	var root := get_tree().root
	if not ok:
		root.set_meta("smoke_failures", int(root.get_meta("smoke_failures", 0)) + 1)
	print("SMOKE: %s %s" % ["ok  " if ok else "FAIL", what])


func _smoke_dock() -> Control:
	return EditorInterface.get_base_control().find_children("*", "GitDock", true, false)[0]


func _smoke_section(title: String) -> FoldableContainer:
	for f: FoldableContainer in _smoke_dock().find_children("*", "FoldableContainer", true, false):
		if f.title == title or f.title.begins_with(title + " ("):
			return f
	return null


func _smoke_rows(title: String, kind: String) -> Array[TreeItem]:
	var rows: Array[TreeItem] = []
	var section := _smoke_section(title)
	if not section:
		return rows
	var tree: Tree = section.find_children("*", "Tree", true, false)[0]
	var stack: Array[TreeItem] = [tree.get_root()]
	while not stack.is_empty():
		var item: TreeItem = stack.pop_back()
		if item == null:
			continue
		if kind == "" or item.get_meta("git_row", "") == kind:
			rows.append(item)
		stack.append_array(item.get_children())
	return rows


# Stash: set what's staged aside, see it under Stashes, look inside, restore it.
func _stash_press() -> void:
	var stash_button: Button = null
	var nodes: Array[Node] = [_smoke_section("Staged Changes")] # Header buttons are internal children.
	while not nodes.is_empty():
		var node: Node = nodes.pop_back()
		if node is Button and node.tooltip_text.begins_with("Stash the"):
			stash_button = node
		nodes.append_array(node.get_children(true))
	_smoke_check(stash_button != null and not stash_button.disabled, "Staged Changes offers to stash what's staged")
	if stash_button:
		stash_button.pressed.emit()
	# It asks first, showing what goes; Stash (confirmed) goes ahead.
	var dialog: ConfirmationDialog = null
	for d: ConfirmationDialog in _smoke_dock().find_children("*", "ConfirmationDialog", true, false):
		if d.title == "Stash Changes":
			dialog = d
	var lists_file := false
	if dialog:
		for label: Label in dialog.find_children("*", "Label", true, false):
			lists_file = lists_file or label.text.contains("player.gd")
	_smoke_check(dialog != null and dialog.visible and lists_file, "stashing asks first, listing what goes")
	if dialog:
		dialog.confirmed.emit()
		dialog.hide()
	get_tree().create_timer(3.0).timeout.connect(_stash_look)


func _stash_look() -> void:
	var stashes := _smoke_rows("Stashes", "stash")
	var staged_left := _smoke_rows("Staged Changes", "").filter(func(item: TreeItem) -> bool: return item.get_meta("git_path", "") == "player.gd")
	_smoke_check(_smoke_section("Stashes").visible and stashes.size() == 1 and staged_left.is_empty(), "stashing moves what's staged into Stashes")
	if stashes.size() == 1:
		stashes[0].collapsed = false
	get_tree().create_timer(1.0).timeout.connect(_stash_restore)


func _stash_restore() -> void:
	var files := _smoke_rows("Stashes", "file").filter(func(item: TreeItem) -> bool: return item.get_meta("git_path", "") == "player.gd")
	_smoke_check(files.size() == 1, "an expanded stash lists its files")
	var stashes := _smoke_rows("Stashes", "stash")
	if stashes.size() == 1:
		stashes[0].get_tree().button_clicked.emit(stashes[0], 1, 0, MOUSE_BUTTON_LEFT) # Restore.
	get_tree().create_timer(3.0).timeout.connect(_stash_done)


func _stash_done() -> void:
	var staged := _smoke_rows("Staged Changes", "").filter(func(item: TreeItem) -> bool: return item.get_meta("git_path", "") == "player.gd")
	var strip := ""
	for label: RichTextLabel in _smoke_dock().find_children("*", "RichTextLabel", true, false):
		strip += label.get_parsed_text()
	_smoke_check(staged.size() == 1 and not _smoke_section("Stashes").visible, "restoring puts it back, staged, and the section goes away (strip: %s)" % strip)
	# Change marks: open a changed script in the script editor.
	EditorInterface.edit_script(load("res://player.gd"))
	get_tree().create_timer(1.5).timeout.connect(_marks_check)


func _marks_check() -> void:
	var marks: Node = null
	for node in _smoke_dock().get_children():
		if node.get_class() == "GitScriptMarks":
			marks = node
	var editor := EditorInterface.get_script_editor().get_current_editor()
	var code_edit := editor.get_base_editor() as CodeEdit if editor else null
	var has_column := false
	if code_edit:
		for i in code_edit.get_gutter_count():
			has_column = has_column or code_edit.get_gutter_name(i) == "godot_git_changes"
	var hunks: Array = marks.get_hunks(code_edit) if marks and code_edit else []
	_smoke_check(has_column and hunks.size() >= 3, "a changed script gets change marks (%d changes)" % hunks.size())
	if marks and code_edit and not hunks.is_empty():
		marks.show_preview(code_edit, 1)
		var preview_open := code_edit.find_children("*", "GitChangePreview", false, false).size() == 1
		var labels := PackedStringArray()
		for label: Label in code_edit.find_children("*", "Label", true, false):
			labels.append(label.text)
		_smoke_check(preview_open and " ".join(labels).contains("Change 2 of"), "clicking a mark shows what was there (%s)" % " ".join(labels))
		# Reverting every change, last first, gives back the committed text (unsaved, in the editor).
		for i in range(hunks.size() - 1, -1, -1):
			marks.revert(code_edit, i)
		var committed := ""
		var out := []
		OS.execute("git", ["-C", ProjectSettings.globalize_path("res://"), "show", "HEAD:player.gd"], out)
		committed = "".join(out)
		_smoke_check(marks.get_hunks(code_edit).is_empty() and code_edit.text.strip_edges() == committed.strip_edges(), "reverting every change gives back the committed text")
		code_edit.undo() # Leaves the file as it was, for anything after this.
	_smoke_finish()


func _smoke_finish() -> void:
	var failed := int(get_tree().root.get_meta("smoke_failures", 0))
	if failed == 0:
		print("SMOKE: OK")
	else:
		print("SMOKE: %d FAILED" % failed)
	get_tree().quit(1 if failed else 0)
