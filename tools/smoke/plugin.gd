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
## emitted signals, not real mouse input, which skips the Tree's own click handling (CLAUDE.md,
## gotcha 38). A crash still ends the run without "SMOKE: OK", which is what CI checks for.

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
		if f.title == title:
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

	# Refreshing twice: the second takes the "nothing changed" paths (History kept as is).
	dock.refresh()
	dock.refresh()
	await _frames()
	_check(_file_row("Changes", "player.gd") != null, "an unstaged file is listed")
	_check(_file_row("Staged Changes", "enemy.gd") != null, "a staged file is listed")

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
	player.select(0)
	changes.multi_selected.emit(player, 0, true)
	await _frames()
	changes.button_clicked.emit(_file_row("Changes", "player.gd"), 0, 0, MOUSE_BUTTON_LEFT) # BUTTON_STAGE
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

	if failures == 0:
		print("SMOKE: OK")
	else:
		print("SMOKE: %d FAILED" % failures)
	get_tree().quit(1 if failures else 0)
