extends "res://tests/test_case.gd"
## Scene diffs (get_diff(...)["scene"], scene_changes): `.tscn` and `.tres` changes as nodes and
## properties. What Godot rewrites on its own (ids renumbered, load_steps) isn't a change.

const BASE := """[gd_scene load_steps=4 format=3 uid="uid://abc"]

[ext_resource type="Script" uid="uid://s1" path="res://player.gd" id="1_aaaaa"]
[ext_resource type="Texture2D" uid="uid://t1" path="res://hero.png" id="2_bbbbb"]

[sub_resource type="RectangleShape2D" id="RectangleShape2D_ccccc"]
size = Vector2(32, 32)

[node name="Player" type="CharacterBody2D" unique_id=100]
script = ExtResource("1_aaaaa")
speed = 200.0

[node name="Sprite" type="Sprite2D" parent="." unique_id=200]
texture = ExtResource("2_bbbbb")

[node name="Shape" type="CollisionShape2D" parent="." unique_id=300]
shape = SubResource("RectangleShape2D_ccccc")

[node name="Old" type="Node2D" parent="." unique_id=400]

[node name="Area" type="Area2D" parent="." unique_id=500]

[connection signal="body_entered" from="Area" to="." method="_on_body_entered"]
"""


func run() -> void:
	_property_changes()
	_nodes_added_removed_renamed_moved()
	_renumbered_ids_arent_changes()
	_sub_resource_contents()
	_connections()
	_resource_file()


## The scene diff of BASE changed to p_new (unstaged).
func _scene_diff(name: String, new_text: String, file := "player.tscn", old_text := BASE) -> Dictionary:
	var repo := make_repo(name)
	write(repo.path_join(file), old_text)
	commit_all(repo, "First")
	write(repo.path_join(file), new_text)
	return open(repo).get_diff(file, false).get("scene", {})


func _node(scene: Dictionary, path: String) -> Dictionary:
	for node: Dictionary in scene.get("nodes", []):
		if node.path == path:
			return node
	return {}


func _property(node: Dictionary, name: String) -> Dictionary:
	for property: Dictionary in node.get("properties", []):
		if property.name == name:
			return property
	return {}


func _property_changes() -> void:
	var scene := _scene_diff("props", BASE.replace("speed = 200.0", "speed = 250.0").replace("res://hero.png", "res://hero_red.png"))
	var player := _node(scene, "Player")
	check("props: Player changed, speed 200 -> 250", player.get("status") == "changed" and _property(player, "speed").get("old") == "200.0" and _property(player, "speed").get("new") == "250.0", scene)
	var texture := _property(_node(scene, "Player/Sprite"), "texture")
	check("props: the texture shows as its path", texture.get("old") == "res://hero.png" and texture.get("new") == "res://hero_red.png", texture)
	check("props: nothing else", scene.nodes.size() == 2, scene.nodes)


func _nodes_added_removed_renamed_moved() -> void:
	var text := BASE.replace('[node name="Old" type="Node2D" parent="." unique_id=400]\n', "")
	text = text.replace('[node name="Sprite" type="Sprite2D" parent="." unique_id=200]', '[node name="Body" type="Sprite2D" parent="." unique_id=200]')
	text = text.replace('[node name="Shape" type="CollisionShape2D" parent="." unique_id=300]', '[node name="Shape" type="CollisionShape2D" parent="Area" unique_id=300]')
	text += '\n[node name="Camera" type="Camera2D" parent="." unique_id=600]\nzoom = Vector2(2, 2)\n'
	var scene := _scene_diff("nodes", text)
	var body := _node(scene, "Player/Body")
	check("nodes: Sprite renamed to Body", body.get("status") == "renamed" and body.get("old_path") == "Player/Sprite", scene)
	var shape := _node(scene, "Player/Area/Shape")
	check("nodes: Shape moved under Area", shape.get("status") == "moved" and shape.get("old_path") == "Player/Shape", shape)
	var camera := _node(scene, "Player/Camera")
	check("nodes: Camera added, with its zoom", camera.get("status") == "added" and camera.get("type") == "Camera2D" and _property(camera, "zoom").get("new") == "Vector2(2, 2)", camera)
	check("nodes: Old removed", _node(scene, "Player/Old").get("status") == "removed", scene)


func _renumbered_ids_arent_changes() -> void:
	# What a re-save can do: new ExtResource and SubResource ids, load_steps and the order of
	# ext_resources changed, nothing that means anything.
	var text := BASE.replace("1_aaaaa", "3_zzzzz").replace("2_bbbbb", "1_yyyyy").replace("RectangleShape2D_ccccc", "RectangleShape2D_xxxxx").replace("load_steps=4 ", "")
	var scene := _scene_diff("renumbered", text)
	check("renumbered: no node changes, generated only", (scene.nodes as Array).is_empty() and (scene.connections as Array).is_empty() and scene.generated_only, scene)


func _sub_resource_contents() -> void:
	var text := BASE.replace("size = Vector2(32, 32)", "size = Vector2(40, 32)").replace("RectangleShape2D_ccccc", "RectangleShape2D_qqqqq")
	var scene := _scene_diff("sub", text)
	var size := _property(_node(scene, "Player/Shape"), "shape › size")
	check("sub: the shape's own property changed, whatever its id", size.get("old") == "Vector2(32, 32)" and size.get("new") == "Vector2(40, 32)", scene)


func _connections() -> void:
	var text := BASE + '[connection signal="area_entered" from="Area" to="." method="_on_area_entered"]\n'
	text = text.replace('[connection signal="body_entered" from="Area" to="." method="_on_body_entered"]\n', "")
	var scene := _scene_diff("connections", text)
	var statuses := {}
	for connection: Dictionary in scene.connections:
		statuses[connection.text] = connection.status
	check("connections: one added, one removed", statuses.get("area_entered: Area → Player._on_area_entered()") == "added" and statuses.get("body_entered: Area → Player._on_body_entered()") == "removed", scene.connections)


func _resource_file() -> void:
	var old_text := '[gd_resource type="Resource" script_class="Weapon" load_steps=2 format=3]\n\n[ext_resource type="Script" path="res://weapon.gd" id="1_a"]\n\n[resource]\nscript = ExtResource("1_a")\ndamage = 10\nname = "Sword"\n'
	var scene := _scene_diff("tres", old_text.replace("damage = 10", "damage = 12"), "sword.tres", old_text)
	var resource := _node(scene, "")
	check("tres: its own properties, typed as the resource", resource.get("type") == "Weapon" and _property(resource, "damage").get("old") == "10" and _property(resource, "damage").get("new") == "12", scene)
