extends "res://tests/test_case.gd"
## Scene merges (GitApi.merge_scene): three versions of a .tscn / .tres as text, merged section by
## section, so two sides adding resources or nodes in the same place don't clash the way git's line
## merge does. Invented scenes only, except the canary at the end, which uses Godot's own writer.

const HEADER := '[gd_scene format=4 uid="uid://abc"]'
const E_SCRIPT := '[ext_resource type="Script" uid="uid://s1" path="res://player.gd" id="1_aaaaa"]'
const ROOT := '[node name="Root" type="Node2D" unique_id=100]\nscript = ExtResource("1_aaaaa")'
const NODE_A := '[node name="A" type="Node2D" parent="." unique_id=200]\nposition = Vector2(10, 10)'
const NODE_B := '[node name="B" type="Node2D" parent="." unique_id=300]\nposition = Vector2(20, 20)'


func run() -> void:
	_both_add_at_end()
	_same_ext_id_renamed()
	_same_path_different_ids()
	_different_properties()
	_same_property_conflicts()
	_removed_but_edited()
	_removed_parent_with_new_child()
	_removed_resource_still_used()
	_round_trip()
	_newer_format_refused()
	_load_steps_updated()
	_both_add_file_differently()
	_godot_writer_canary()


## Joins a file from its sections (each may hold several lines, joined with \n escapes).
func _file(sections: Array) -> String:
	return "\n\n".join(PackedStringArray(sections)) + "\n"


## The merged file with one side picked for every conflict: String segments as they are.
func _joined(result: Dictionary, side: String) -> String:
	var out := ""
	for segment: Variant in result.get("segments", []):
		if segment is String:
			out += segment
		else:
			out += str(segment.get(side, ""))
	return out


## The id Godot's ext_resource line for p_path has in p_text ("" when there is none).
func _ext_id(text: String, path: String) -> String:
	var regex := RegEx.new()
	regex.compile('path="%s" id="([^"]+)"' % path)
	var found := regex.search(text)
	return found.get_string(1) if found else ""


func _count(text: String, needle: String) -> int:
	return text.split(needle).size() - 1


func _conflicts_of(result: Dictionary) -> Array:
	var out: Array = []
	for segment: Variant in result.get("segments", []):
		if segment is Dictionary:
			out.append(segment)
	return out


func _base() -> String:
	return _file([HEADER, E_SCRIPT, ROOT, NODE_A, NODE_B])


func _both_add_at_end() -> void:
	var hero := '[ext_resource type="Texture2D" uid="uid://t1" path="res://hero.png" id="2_bbbbb"]'
	var step := '[ext_resource type="AudioStream" uid="uid://a1" path="res://step.wav" id="3_ccccc"]'
	var node_c := '[node name="C" type="Sprite2D" parent="." unique_id=400]\ntexture = ExtResource("2_bbbbb")'
	var node_d := '[node name="D" type="AudioStreamPlayer" parent="." unique_id=500]\nstream = ExtResource("3_ccccc")'
	var mine := _file([HEADER, E_SCRIPT, hero, ROOT, NODE_A, NODE_B, node_c])
	var theirs := _file([HEADER, E_SCRIPT, step, ROOT, NODE_A, NODE_B, node_d])
	var result := GitApi.merge_scene(_base(), mine, theirs)
	var text: String = result.get("text", "")
	check("add at end: ok", result.get("ok") == true, result)
	check("add at end: no conflicts", result.get("conflicts") == 0, result.get("conflicts"))
	check("add at end: both nodes", text.contains('name="C"') and text.contains('name="D"'), text)
	check("add at end: mine's node before theirs'", text.find('name="C"') < text.find('name="D"'), text)
	check("add at end: both resources", text.contains("res://hero.png") and text.contains("res://step.wav"), text)
	var ext_end := text.find('[node name="Root"')
	check("add at end: resources before the nodes", text.find("res://hero.png") < ext_end and text.find("res://step.wav") < ext_end, text)
	check("add at end: mine's resource before theirs'", text.find("res://hero.png") < text.find("res://step.wav"), text)


func _same_ext_id_renamed() -> void:
	var hero := '[ext_resource type="Texture2D" uid="uid://t1" path="res://hero.png" id="5_aaaaa"]'
	var step := '[ext_resource type="AudioStream" uid="uid://a1" path="res://step.wav" id="5_aaaaa"]'
	var node_c := '[node name="C" type="Sprite2D" parent="." unique_id=400]\ntexture = ExtResource("5_aaaaa")'
	var node_d := '[node name="D" type="AudioStreamPlayer" parent="." unique_id=500]\nstream = ExtResource("5_aaaaa")'
	var mine := _file([HEADER, E_SCRIPT, hero, ROOT, NODE_A, NODE_B, node_c])
	var theirs := _file([HEADER, E_SCRIPT, step, ROOT, NODE_A, NODE_B, node_d])
	var result := GitApi.merge_scene(_base(), mine, theirs)
	var text: String = result.get("text", "")
	var step_id := _ext_id(text, "res://step.wav")
	check("same id: ok", result.get("ok") == true, result)
	check("same id: theirs' resource got a new id", step_id != "" and step_id != "5_aaaaa", step_id)
	check("same id: theirs' node points at the new id", text.contains('stream = ExtResource("%s")' % step_id), text)
	check("same id: mine's resource keeps its id", _ext_id(text, "res://hero.png") == "5_aaaaa", text)
	check("same id: mine's node unchanged", text.contains('texture = ExtResource("5_aaaaa")'), text)
	check("same id: only one resource has 5_aaaaa", _count(text, 'id="5_aaaaa"') == 1, text)


func _same_path_different_ids() -> void:
	var hero_mine := '[ext_resource type="Texture2D" uid="uid://t1" path="res://hero.png" id="2_mine1"]'
	var hero_theirs := '[ext_resource type="Texture2D" uid="uid://t1" path="res://hero.png" id="2_theirs"]'
	var node_c := '[node name="C" type="Sprite2D" parent="." unique_id=400]\ntexture = ExtResource("2_mine1")'
	var node_d := '[node name="D" type="Sprite2D" parent="." unique_id=500]\ntexture = ExtResource("2_theirs")'
	var mine := _file([HEADER, E_SCRIPT, hero_mine, ROOT, NODE_A, NODE_B, node_c])
	var theirs := _file([HEADER, E_SCRIPT, hero_theirs, ROOT, NODE_A, NODE_B, node_d])
	var result := GitApi.merge_scene(_base(), mine, theirs)
	var text: String = result.get("text", "")
	check("same path: ok", result.get("ok") == true, result)
	check("same path: one line for the path", _count(text, 'path="res://hero.png"') == 1, text)
	check("same path: that line keeps mine's id", _ext_id(text, "res://hero.png") == "2_mine1", text)
	check("same path: theirs' node refers to mine's id", _count(text, 'ExtResource("2_mine1")') == 2 and not text.contains("2_theirs"), text)


func _different_properties() -> void:
	var mine := _file([HEADER, E_SCRIPT, ROOT, "[node name=\"A\" type=\"Node2D\" parent=\".\" unique_id=200]\nposition = Vector2(11, 11)", NODE_B])
	var theirs := _file([HEADER, E_SCRIPT, ROOT, NODE_A, "[node name=\"B\" type=\"Node2D\" parent=\".\" unique_id=300]\nposition = Vector2(21, 21)"])
	var result := GitApi.merge_scene(_base(), mine, theirs)
	var text: String = result.get("text", "")
	check("other properties: ok", result.get("ok") == true, result)
	check("other properties: no conflicts", result.get("conflicts") == 0, result.get("conflicts"))
	check("other properties: both changes", text.contains("position = Vector2(11, 11)") and text.contains("position = Vector2(21, 21)"), text)
	check("other properties: from_mine 1, from_theirs 1", result.get("from_mine") == 1 and result.get("from_theirs") == 1, result)
	check("other properties: the exact merged file", text == _file([HEADER, E_SCRIPT, ROOT, "[node name=\"A\" type=\"Node2D\" parent=\".\" unique_id=200]\nposition = Vector2(11, 11)", "[node name=\"B\" type=\"Node2D\" parent=\".\" unique_id=300]\nposition = Vector2(21, 21)"]), text)


func _same_property_conflicts() -> void:
	var mine_text := _file([HEADER, E_SCRIPT, ROOT, "[node name=\"A\" type=\"Node2D\" parent=\".\" unique_id=200]\nposition = Vector2(1, 1)", NODE_B])
	var theirs_text := _file([HEADER, E_SCRIPT, ROOT, "[node name=\"A\" type=\"Node2D\" parent=\".\" unique_id=200]\nposition = Vector2(2, 2)", NODE_B])
	var result := GitApi.merge_scene(_base(), mine_text, theirs_text)
	check("same property: ok", result.get("ok") == true, result)
	check("same property: one conflict", result.get("conflicts") == 1, result.get("conflicts"))
	var conflicts := _conflicts_of(result)
	if conflicts.size() == 1:
		var c: Dictionary = conflicts[0]
		check("same property: base has the old value", str(c.get("base", "")).contains("Vector2(10, 10)"), c)
		check("same property: mine has its value", str(c.get("mine", "")).contains("Vector2(1, 1)"), c)
		check("same property: theirs has its value", str(c.get("theirs", "")).contains("Vector2(2, 2)"), c)
	else:
		check("same property: a conflict segment", false, result.get("segments"))
	check("same property: mine's side is mine's file", _joined(result, "mine") == mine_text, _joined(result, "mine"))


func _removed_but_edited() -> void:
	var mine := _file([HEADER, E_SCRIPT, ROOT, NODE_A])
	var theirs := _file([HEADER, E_SCRIPT, ROOT, NODE_A, "[node name=\"B\" type=\"Node2D\" parent=\".\" unique_id=300]\nposition = Vector2(25, 25)"])
	var result := GitApi.merge_scene(_base(), mine, theirs)
	check("removed and edited: one conflict", result.get("conflicts") == 1, result)
	var conflicts := _conflicts_of(result)
	if conflicts.size() == 1:
		var c: Dictionary = conflicts[0]
		check("removed and edited: mine's side is empty", c.get("mine") == "", c)
		check("removed and edited: theirs keeps the edit", str(c.get("theirs", "")).contains("Vector2(25, 25)"), c)
	else:
		check("removed and edited: a conflict segment", false, result.get("segments"))


func _removed_parent_with_new_child() -> void:
	var child := "[node name=\"E\" type=\"Node2D\" parent=\"A\" unique_id=600]"
	var mine := _file([HEADER, E_SCRIPT, ROOT, NODE_B])
	var theirs := _file([HEADER, E_SCRIPT, ROOT, NODE_A, NODE_B, child])
	var result := GitApi.merge_scene(_base(), mine, theirs)
	var reason: String = result.get("reason", "")
	check("removed parent: refused", result.get("ok") == false, result)
	check("removed parent: reason mentions the parent", reason.to_lower().contains("parent") or reason.contains("A"), reason)


func _removed_resource_still_used() -> void:
	var hero := '[ext_resource type="Texture2D" uid="uid://t1" path="res://hero.png" id="2_bbbbb"]'
	var node_c := '[node name="C" type="Sprite2D" parent="." unique_id=400]\ntexture = ExtResource("2_bbbbb")'
	var base := _file([HEADER, E_SCRIPT, hero, ROOT, NODE_A, NODE_B])
	var mine := _file([HEADER, E_SCRIPT, hero, ROOT, NODE_A, NODE_B, node_c])
	var theirs := _file([HEADER, E_SCRIPT, ROOT, NODE_A, NODE_B])
	var result := GitApi.merge_scene(base, mine, theirs)
	var reason: String = result.get("reason", "")
	check("removed resource: refused", result.get("ok") == false, result)
	check("removed resource: reason mentions a resource", reason.to_lower().contains("resource"), reason)


func _round_trip() -> void:
	var base := _base()
	var same := GitApi.merge_scene(base, base, base)
	check("round trip: ok", same.get("ok") == true, same)
	check("round trip: text is the file", same.get("text") == base, same.get("text"))
	var crlf := base.replace("\n", "\r\n")
	var crlf_result := GitApi.merge_scene(crlf, crlf, crlf)
	check("round trip: CRLF kept", crlf_result.get("ok") == true and crlf_result.get("text") == crlf, crlf_result.get("text"))


func _newer_format_refused() -> void:
	var newer := _base().replace("format=4", "format=5")
	var result := GitApi.merge_scene(newer, newer, newer)
	var reason: String = result.get("reason", "")
	check("format 5: refused", result.get("ok") == false, result)
	check("format 5: reason mentions a newer Godot", reason.to_lower().contains("newer"), reason)
	var mine_only := GitApi.merge_scene(_base(), newer, _base())
	check("format 5 in mine only: refused", mine_only.get("ok") == false, mine_only)


## Format 3 counts load_steps as its ext_resources and sub_resources plus one, so the base has
## three of those (load_steps=4) and theirs adds a fourth (load_steps=5).
func _load_steps_updated() -> void:
	var hero := '[ext_resource type="Texture2D" uid="uid://t1" path="res://hero.png" id="2_bbbbb"]'
	var sub := '[sub_resource type="RectangleShape2D" id="RectangleShape2D_ccccc"]\nsize = Vector2(32, 32)'
	var base := _file(['[gd_scene load_steps=4 format=3 uid="uid://abc"]', E_SCRIPT, hero, sub, ROOT, NODE_A, NODE_B])
	var mine := _file(['[gd_scene load_steps=4 format=3 uid="uid://abc"]', E_SCRIPT, hero, sub, ROOT, "[node name=\"A\" type=\"Node2D\" parent=\".\" unique_id=200]\nposition = Vector2(11, 11)", NODE_B])
	var theirs := _file(['[gd_scene load_steps=5 format=3 uid="uid://abc"]', E_SCRIPT, hero, '[ext_resource type="AudioStream" uid="uid://a1" path="res://step.wav" id="3_ccccc"]', sub, ROOT, NODE_A, NODE_B])
	var result := GitApi.merge_scene(base, mine, theirs)
	var text: String = result.get("text", "")
	check("load_steps: ok", result.get("ok") == true, result)
	check("load_steps: header says 5", text.begins_with('[gd_scene load_steps=5 format=3 uid="uid://abc"]\n'), text.left(80))
	check("load_steps: theirs' resource and mine's edit", text.contains("res://step.wav") and text.contains("Vector2(11, 11)"), text)


func _both_add_file_differently() -> void:
	var mine := _file([HEADER, '[node name="Root" type="Node2D" unique_id=100]\nposition = Vector2(1, 1)'])
	var theirs := _file([HEADER, '[node name="Root" type="Node2D" unique_id=100]\nposition = Vector2(2, 2)'])
	var result := GitApi.merge_scene("", mine, theirs)
	check("new file on both sides: not refused", result.get("ok") == true, result)
	check("new file on both sides: conflicts", int(result.get("conflicts", 0)) > 0, result)


## Godot's own writer decides what the files look like, so the merge is checked against a real scene.
func _godot_writer_canary() -> void:
	var folder := dir.path_join("canary")
	DirAccess.make_dir_recursive_absolute(folder)
	var original := folder.path_join("original.tscn")
	var root := Node2D.new()
	root.name = "Root"
	var sprite := Sprite2D.new()
	sprite.name = "Sprite"
	root.add_child(sprite)
	sprite.owner = root
	var mover := Node2D.new()
	mover.name = "Mover"
	root.add_child(mover)
	mover.owner = root
	mover.position = Vector2(5, 5)
	var saved := _save_scene(root, original)
	root.free()
	check("canary: Godot saved the original", saved == OK, saved)
	var base_text := FileAccess.get_file_as_string(original)

	var theirs_root := _instance(original)
	var added := Node2D.new()
	added.name = "Added"
	theirs_root.add_child(added)
	added.owner = theirs_root
	var theirs_path := folder.path_join("theirs.tscn")
	_save_scene(theirs_root, theirs_path)
	theirs_root.free()

	var mine_root := _instance(original)
	var moved := mine_root.get_node_or_null("Mover") as Node2D
	if moved:
		moved.position = Vector2(100, 50)
	var mine_path := folder.path_join("mine.tscn")
	_save_scene(mine_root, mine_path)
	mine_root.free()

	var mine_text := FileAccess.get_file_as_string(mine_path)
	var theirs_text := FileAccess.get_file_as_string(theirs_path)
	check("canary: the original merges to itself", GitApi.merge_scene(base_text, base_text, base_text).get("text") == base_text, base_text)

	var result := GitApi.merge_scene(base_text, mine_text, theirs_text)
	check("canary: merge ok", result.get("ok") == true, result)
	check("canary: no conflicts", result.get("conflicts") == 0, result)
	if result.get("ok") != true:
		return
	var merged_path := folder.path_join("merged.tscn")
	write(merged_path, result.get("text", ""))
	var merged := _instance(merged_path)
	check("canary: merged scene loads", merged != null, result.get("text", ""))
	if merged == null:
		return
	check("canary: Root has three children", merged.get_child_count() == 3, merged.get_child_count())
	check("canary: Sprite is there", merged.get_node_or_null("Sprite") != null, result.get("text", ""))
	check("canary: Added is there", merged.get_node_or_null("Added") != null, result.get("text", ""))
	var merged_mover := merged.get_node_or_null("Mover") as Node2D
	check("canary: Mover has mine's position", merged_mover != null and merged_mover.position == Vector2(100, 50), merged_mover.position if merged_mover else "missing")
	merged.free()


func _save_scene(root: Node, path: String) -> Error:
	var packed := PackedScene.new()
	var err := packed.pack(root)
	if err != OK:
		return err
	return ResourceSaver.save(packed, path)


func _instance(path: String) -> Node:
	var scene := ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE) as PackedScene
	return scene.instantiate() if scene else null
