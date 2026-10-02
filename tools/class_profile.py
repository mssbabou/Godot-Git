"""Writes godot-cpp's build profile: the engine classes our sources include, so godot-cpp
generates and compiles bindings for those (and their parents) instead of all ~1000.

It's worked out from the `#include <godot_cpp/classes/...>` lines on every build, so it can't go
stale: a class that's included is in it. (A class used only through another's header would be
missing; the build then fails on it, and including its header fixes that.)
"""

import glob
import json
import os
import re
import sys


def write(root, api_version, out_path):
    sys.path.insert(0, os.path.join(root, "thirdparty", "godot-cpp"))
    from binding_generator import camel_to_snake  # godot-cpp's own name -> file name rule

    api_path = os.path.join(root, "thirdparty", "godot-cpp", "gdextension", "extension_api-%s.json" % api_version.replace(".", "-"))
    with open(api_path, encoding="utf-8") as f:
        api = json.load(f)
    by_file = {camel_to_snake(c["name"]): c["name"] for c in api["classes"]}

    used = set()
    include = re.compile(r"godot_cpp/classes/([a-z0-9_]+)\.hpp")
    for path in glob.glob(os.path.join(root, "src", "**", "*.*"), recursive=True):
        with open(path, encoding="utf-8") as f:
            for name in include.findall(f.read()):
                if name in by_file:
                    used.add(by_file[name])

    text = json.dumps({"enabled_classes": sorted(used)}, indent="\t") + "\n"
    # Only rewritten when it changes: godot-cpp regenerates every binding when it does.
    old = None
    if os.path.exists(out_path):
        with open(out_path, encoding="utf-8") as f:
            old = f.read()
    if old != text:
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    return out_path
