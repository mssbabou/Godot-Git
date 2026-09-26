#!/usr/bin/env python
"""Builds a playground: a small Godot project in its own git repository, with a history and
uncommitted changes of every kind, and a copy of the addon. For trying the Git panel by hand, and
for the smoke test (--smoke), which CI runs in a headless editor.

    python tools/make_playground.py <folder> [--smoke]

The folder is deleted first if it exists. The addon (including its built library) is copied from
project/addons/godot_git, so build first. Open it with: godot -e --path <folder>

What's in it:
- history: a first commit, a commit with a message body, a feature branch merged back in
  (--smoke adds 60 more small commits, for "Load More Commits");
- staged: an edited script, a rename, and an image saved again with the same pixels; unstaged: an
  edited script with several hunks, an edited scene, a recolored sprite, a changed SVG, a changed
  binary file, a deleted file; new: an untracked script and image.
"""

import os
import shutil
import struct
import subprocess
import sys
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDON = os.path.join(REPO, "project", "addons", "godot_git")
SMOKE = os.path.join(REPO, "tools", "smoke")

PLAYER = '''extends CharacterBody2D
## The player: moves, jumps and takes damage.

signal died
signal health_changed(value: int)

const SPEED := 220.0
const JUMP_VELOCITY := -420.0

@export var max_health := 5
@export var invincible_time := 0.8

var health := max_health
var _hurt_timer := 0.0


func _ready() -> void:
	health = max_health
	add_to_group("player")


func _physics_process(delta: float) -> void:
	if not is_on_floor():
		velocity += get_gravity() * delta

	if Input.is_action_just_pressed("jump") and is_on_floor():
		velocity.y = JUMP_VELOCITY

	var direction := Input.get_axis("move_left", "move_right")
	if direction:
		velocity.x = direction * SPEED
	else:
		velocity.x = move_toward(velocity.x, 0, SPEED)

	move_and_slide()
	_hurt_timer = max(0.0, _hurt_timer - delta)


func take_damage(amount: int) -> void:
	if _hurt_timer > 0.0:
		return
	health -= amount
	health_changed.emit(health)
	_hurt_timer = invincible_time
	if health <= 0:
		died.emit()
		queue_free()


func heal(amount: int) -> void:
	health = min(max_health, health + amount)
	health_changed.emit(health)
'''

ENEMY = '''extends Area2D
## Walks back and forth and hurts the player on contact.

@export var speed := 60.0
@export var damage := 1

var _direction := 1.0


func _process(delta: float) -> void:
	position.x += speed * _direction * delta


func _on_body_entered(body: Node2D) -> void:
	if body.is_in_group("player"):
		body.take_damage(damage)


func _on_turn_timer_timeout() -> void:
	_direction = -_direction
'''

UTIL = '''extends RefCounted
## Old helpers nobody uses anymore.

static func clamp01(value: float) -> float:
	return clamp(value, 0.0, 1.0)
'''

HUD = '''extends CanvasLayer
## Shows the player's health.

@onready var label: Label = $Health


func show_health(value: int) -> void:
	label.text = "HP %d" % value
'''

COIN = '''extends Area2D
## A coin: +1 score when the player touches it.

signal collected


func _on_body_entered(body: Node2D) -> void:
	if body.is_in_group("player"):
		collected.emit()
		queue_free()
'''

SCENE = '''[gd_scene load_steps=2 format=3 uid="uid://b6x3playgrnd1"]

[ext_resource type="Script" path="res://player.gd" id="1_abcde"]

[node name="Main" type="Node2D"]

[node name="Player" type="CharacterBody2D" parent="."]
position = Vector2(120, 300)
script = ExtResource("1_abcde")

[node name="Camera2D" type="Camera2D" parent="Player"]
zoom = Vector2(2, 2)
'''


def png(width, height, pixel, level=9):
    """A PNG image (RGBA) whose pixel(x, y) returns (r, g, b, a); level is the zlib compression."""
    rows = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, level)) + chunk(b"IEND", b""))


def sprite(body):
    """A 32×32 pixel-art character: body color, darker outline, transparent around it."""
    outline = tuple(c // 2 for c in body[:3]) + (255,)

    def pixel(x, y):
        inside = 8 <= x < 24 and 4 <= y < 28
        edge = inside and (x in (8, 23) or y in (4, 27))
        eye = y == 10 and x in (12, 19)
        if eye:
            return (255, 255, 255, 255)
        if edge:
            return outline
        return body if inside else (0, 0, 0, 0)
    return pixel


SVG = '''<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
  <rect x="4" y="4" width="56" height="56" rx="12" fill="#478cbf"/>
  <circle cx="32" cy="32" r="14" fill="#ffffff"/>
</svg>
'''


def _writable(func, path, _exc):
    os.chmod(path, 0o666)  # git's object files are read-only, which Windows won't delete.
    func(path)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        sys.exit(2)
    smoke = "--smoke" in sys.argv
    root = os.path.abspath(args[0])
    if not os.path.isdir(os.path.join(ADDON, "bin")):
        sys.exit("No built library in %s; run scons first." % ADDON)

    if os.path.exists(root):
        shutil.rmtree(root, onexc=_writable)
    os.makedirs(root)

    def git(*git_args):
        subprocess.run(["git", "-C", root, *git_args], check=True, capture_output=True)

    def write(rel, text):
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    plugins = '"res://addons/godot_git_smoke/plugin.cfg"' if smoke else ""
    write("project.godot", '''; Engine configuration file.
config_version=5

[application]

config/name="Git Playground"
config/features=PackedStringArray("4.7")

[editor_plugins]

enabled=PackedStringArray(%s)
''' % plugins)
    write(".gitignore", ".godot/\naddons/godot_git/bin/\naddons/godot_git_smoke/\n")
    write("player.gd", PLAYER)
    write("enemy.gd", ENEMY)
    write("util/old_util.gd", UTIL)
    write("main.tscn", SCENE)
    write("README.md", "# Git Playground\n\nA tiny platformer to try the Git panel on.\n")
    with open(os.path.join(root, "save.dat"), "wb") as f:
        f.write(bytes(range(256)) * 4)

    def write_bytes(rel, data):
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    write_bytes("art/player.png", png(32, 32, sprite((70, 130, 220, 255))))
    background = lambda x, y: (40 + x // 2, 60 + y // 3, 90, 255)
    write_bytes("art/background.png", png(128, 72, background, level=9))
    write("icon.svg", SVG)

    subprocess.run(["git", "init", "-q", "-b", "main", root], check=True)
    git("config", "user.name", "Playground")
    git("config", "user.email", "playground@example.com")
    git("config", "core.autocrlf", "false")
    shutil.copytree(ADDON, os.path.join(root, "addons", "godot_git"), ignore=shutil.ignore_patterns("~*"))
    if smoke:
        shutil.copytree(SMOKE, os.path.join(root, "addons", "godot_git_smoke"))
    git("add", "-A")
    git("commit", "-q", "-m", "Initial platformer")

    # History: a commit with a message body, and a feature branch merged back in.
    write("enemy.gd", ENEMY.replace("@export var damage := 1", "@export var damage := 2"))
    git("commit", "-q", "-am", "Make enemies hit harder", "-m", "Playtesters walked straight through them.", "-m", "Two damage makes them worth avoiding.")
    git("checkout", "-q", "-b", "hud")
    write("hud.gd", HUD)
    git("add", "hud.gd")
    git("commit", "-q", "-m", "Add a health HUD")
    git("checkout", "-q", "main")
    write("util/old_util.gd", UTIL.replace("clamp01", "clamp_01"))
    git("commit", "-q", "-am", "Rename clamp01")
    git("merge", "-q", "--no-ff", "--no-edit", "hud")
    if smoke:
        for i in range(60):
            write("counter.txt", "%d\n" % i)
            git("add", "counter.txt")
            git("commit", "-q", "-m", "Counter %d" % i)

    # Uncommitted changes of every kind.
    player = PLAYER.replace("const SPEED := 220.0", "const SPEED := 260.0\nconst ACCELERATION := 1800.0")
    player = player.replace('''	if direction:
		velocity.x = direction * SPEED
	else:
		velocity.x = move_toward(velocity.x, 0, SPEED)
''', '''	velocity.x = move_toward(velocity.x, direction * SPEED, ACCELERATION * delta)
''')
    player = player.replace("	health = min(max_health, health + amount)", "	health = mini(max_health, health + amount)")
    write("player.gd", player + "\n\nfunc is_alive() -> bool:\n\treturn health > 0\n")
    write("enemy.gd", ENEMY.replace("@export var speed := 60.0", "@export var speed := 75.0\n@export var turn_on_edges := true"))
    git("add", "enemy.gd")
    git("mv", "README.md", "NOTES.md")
    write("main.tscn", SCENE.replace("position = Vector2(120, 300)", "position = Vector2(160, 280)"))
    write("coin.gd", COIN)
    os.remove(os.path.join(root, "util", "old_util.gd"))
    with open(os.path.join(root, "save.dat"), "wb") as f:
        f.write(bytes(range(255, -1, -1)) * 4)
    write_bytes("art/player.png", png(32, 32, sprite((220, 80, 70, 255))))  # Recolored: blue to red.
    write_bytes("art/coin.png", png(16, 16, lambda x, y: (240, 200, 40, 255) if (x - 7.5) ** 2 + (y - 7.5) ** 2 < 49 else (0, 0, 0, 0)))
    write_bytes("art/background.png", png(128, 72, background, level=1))  # Same pixels, other file.
    git("add", "art/background.png")
    write("icon.svg", SVG.replace("#478cbf", "#e0703c"))
    print(root)


if __name__ == "__main__":
    main()
