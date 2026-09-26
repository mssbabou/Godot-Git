#!/usr/bin/env python
"""Zips the addon (project/addons/godot_git) into dist/godot_git-<version>.zip.

The zip unpacks to addons/godot_git/, so users can extract it straight into their
Godot project. It contains whatever platform libraries are currently built in
project/addons/godot_git/bin/ (locally: just yours; in CI: all of them), plus the
license files.

Usage: python tools/package.py [--version X] [--out DIR] [--require-all]
Called by `scons package` too.
"""

import argparse
import configparser
import os
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDON_DIR = os.path.join(ROOT, "project", "addons", "godot_git")
ZIP_PREFIX = "addons/godot_git"

# Extra files placed in the zip: source path -> path inside the addon folder.
EXTRA_FILES = {
    "LICENSE": "LICENSE",
    "README.md": "README.md",  # The Asset Store recommends a copy in the plugin folder.
    "thirdparty/libgit2/COPYING": "thirdparty/libgit2-COPYING.txt",
    "thirdparty/godot-cpp/LICENSE.md": "thirdparty/godot-cpp-LICENSE.md",
}

# Build by-products that must not ship: MSVC import libs/symbols, and the "~" copies
# Godot makes of the library for hot reload.
SKIP_EXTENSIONS = (".lib", ".exp", ".pdb", ".ilk")


def detect_version():
    version = os.environ.get("GODOT_GIT_VERSION")
    if version:
        return version
    try:
        return subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"], cwd=ROOT, stderr=subprocess.DEVNULL, text=True
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "dev"


def should_skip(name):
    return name.startswith("~") or name == ".DS_Store" or name.endswith(SKIP_EXTENSIONS)


def library_status():
    """Returns (present, missing) lists of the library paths listed in the .gdextension."""
    config = configparser.ConfigParser()
    config.read(os.path.join(ADDON_DIR, "godot_git.gdextension"), encoding="utf-8")
    present, missing = [], []
    for key, value in config.items("libraries"):
        rel = value.strip('"').replace("res://addons/godot_git/", "")
        (present if os.path.exists(os.path.join(ADDON_DIR, rel)) else missing).append(key)
    return present, missing


def make_zip(version=None, out_dir=None, require_all=False):
    version = version or detect_version()
    out_dir = out_dir or os.path.join(ROOT, "dist")

    present, missing = library_status()
    if not present:
        raise RuntimeError("No libraries built in project/addons/godot_git/bin/. Run scons first.")
    if require_all and missing:
        raise RuntimeError("Missing libraries: {}".format(", ".join(missing)))

    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "godot_git-{}.zip".format(version))

    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for dirpath, dirnames, filenames in os.walk(ADDON_DIR):
            dirnames[:] = sorted(d for d in dirnames if not should_skip(d))
            for name in sorted(filenames):
                if should_skip(name):
                    continue
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, ADDON_DIR).replace(os.sep, "/")
                if name.endswith(".gdextension"):
                    # Hot reload is for developing the plugin. For users it only makes Godot drop
                    # "~" copies of the library into their project (and their git status).
                    with open(full, encoding="utf-8") as f:
                        text = f.read().replace("reloadable = true", "reloadable = false")
                    zf.writestr("{}/{}".format(ZIP_PREFIX, rel), text)
                else:
                    zf.write(full, "{}/{}".format(ZIP_PREFIX, rel))
        for src, dest in EXTRA_FILES.items():
            zf.write(os.path.join(ROOT, src), "{}/{}".format(ZIP_PREFIX, dest))

    print("Packaged {}".format(os.path.relpath(out_path, ROOT)))
    print("  included: {}".format(", ".join(present)))
    if missing:
        print("  not built (skipped): {}".format(", ".join(missing)))
    return out_path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", help="Version string for the zip name (default: git describe)")
    parser.add_argument("--out", help="Output directory (default: dist/)")
    parser.add_argument("--require-all", action="store_true", help="Fail unless every platform in the .gdextension is built")
    args = parser.parse_args()
    try:
        make_zip(args.version, args.out, args.require_all)
    except RuntimeError as e:
        print("error: {}".format(e), file=sys.stderr)
        sys.exit(1)
