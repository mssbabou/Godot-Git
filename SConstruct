#!/usr/bin/env python
import json
import os
import sys

sys.path.insert(0, os.path.join(Dir("#").abspath, "tools"))
import class_profile  # noqa: E402
import libgit2  # noqa: E402
import package  # noqa: E402

libname = "godot_git"
addondir = "project/addons/godot_git"

localEnv = Environment(tools=["default"], PLATFORM="")

customs = ["custom.py"]
customs = [os.path.abspath(path) for path in customs]

opts = Variables(customs, ARGUMENTS)
opts.Add(BoolVariable("rebuild_libgit2", "Wipe and rebuild the bundled libgit2", False))
opts.Update(localEnv)

Help(opts.GenerateHelpText(localEnv))

# Reuse compiled objects between CI runs (godot-cpp alone is ~1000 files).
if os.environ.get("SCONS_CACHE"):
    CacheDir(os.environ["SCONS_CACHE"])

env = localEnv.Clone()

# This is an editor-only plugin, so build the editor target unless told otherwise.
env["target"] = "editor"

# Lets the editor reload the extension after a rebuild instead of needing a restart.
# Can still be overridden with use_hot_reload=no.
env["use_hot_reload"] = ARGUMENTS.get("target", "editor") != "template_release"

if not (os.path.isdir("thirdparty/godot-cpp") and os.listdir("thirdparty/godot-cpp")):
    print("thirdparty/godot-cpp is empty. Run: git submodule update --init --recursive")
    sys.exit(1)

# Match Godot 4.7's own minimum macOS (10.13 on Intel; the compiler raises Apple Silicon to 11.0).
# Without this the library requires the macOS version it was built on. godot-cpp only reads this
# option from the command line, hence setting a default there.
ARGUMENTS.setdefault("macos_deployment_target", "10.13")

# Smaller libraries, at no speed you'd notice (2026-10-02, Windows: 3.45 MB -> 2.67 MB; a 14,000-file
# `git status` 75 vs 76 ms, a file's history through 86k commits 878 vs 892 ms): our code and
# godot-cpp are UI and glue, and git work waits on the disk and network (libgit2 is built for
# size too, in tools/libgit2.py). Every line of code ships six times (two Windows, two Linux, a
# universal macOS), so this is what keeps the zip from growing half a megabyte per release.
ARGUMENTS.setdefault("optimize", "size")
# Bindings for the engine classes we use, not all ~1000 (see tools/class_profile.py). Saves only
# ~50 KB (the linker dropped most unused ones already), but compiles godot-cpp several times faster.
ARGUMENTS.setdefault("build_profile", class_profile.write(Dir("#").abspath, "4.7", os.path.join(Dir("#").abspath, "build", "gen", "build_profile.json")))

env = SConscript("thirdparty/godot-cpp/SConstruct", {"env": env, "customs": customs, "api_version": "4.7"})

# Let the Linux linker drop functions nothing calls (plenty in godot-cpp's bindings and libgit2).
# macOS gets this from -dead_strip and Windows from /OPT:REF; without it the Linux library was
# twice as big (4.2 MB vs about 2 MB per architecture on macOS). No effect on speed. godot-cpp
# builds with this same env, so the flags reach it too; libgit2 gets them in tools/libgit2.py.
if env["platform"] == "linux":
    env.Append(CCFLAGS=["-ffunction-sections", "-fdata-sections"], LINKFLAGS=["-Wl,--gc-sections"])

libgit2.setup(env, Dir("#").abspath)

env.Append(CPPPATH=["src/"])
# Object files go to build/obj/ instead of next to the sources. Their names already carry the
# platform, target and arch (e.g. .windows.editor.x86_64.obj), so one folder serves all builds.
env.VariantDir("build/obj", "src", duplicate=False)
sources = Glob("build/obj/*.cpp", exclude=["build/obj/build_info.cpp"]) + Glob("build/obj/git/*.cpp") + Glob("build/obj/editor/*.cpp")


# Where this build comes from (see src/build_info.h). CI builds name their commit and run, which is
# what the release attestations point at; anything else is a local build.
def write_build_info():
    version = package.detect_version()
    commit, url = "", ""
    if os.environ.get("GITHUB_ACTIONS") == "true" and os.environ.get("GITHUB_REPOSITORY"):
        commit = os.environ.get("GITHUB_SHA", "")
        url = "{}/{}/actions/runs/{}".format(
            os.environ.get("GITHUB_SERVER_URL", "https://github.com"), os.environ["GITHUB_REPOSITORY"], os.environ.get("GITHUB_RUN_ID", "")
        )
    text = "".join(
        "#define {} {}\n".format(name, json.dumps(value))
        for name, value in [("GODOT_GIT_BUILD_VERSION", version), ("GODOT_GIT_BUILD_COMMIT", commit), ("GODOT_GIT_BUILD_URL", url)]
    )
    path = os.path.join(Dir("#").abspath, "build", "gen", "build_info.gen.h")
    old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
    if old != text:  # Only rewritten when it changes, so it doesn't trigger a rebuild every time.
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)


write_build_info()
# Its own include path only here: adding it to env would change every godot-cpp compile command too.
sources.append(env.SharedObject("build/obj/build_info.cpp", CPPPATH=env["CPPPATH"] + [Dir("#build/gen")]))

# Class reference for GitRepository from doc_classes/*.xml (none written yet).
sources.append(env.GodotCPPDocData("build/gen/doc_data.gen.cpp", source=Glob("doc_classes/*.xml")))

# Output: project/addons/godot_git/bin/<platform>/libgodot_git.<platform>.<target>.<arch>.<ext>
# (macOS gets a .framework bundle instead). Must match godot_git.gdextension next to it.
if env["platform"] == "macos":
    name = "lib{}.{}.{}".format(libname, env["platform"], env["target"])
    target_path = "{}/bin/macos/{}.framework/{}".format(addondir, name, name)
else:
    target_path = "{}/bin/{}/lib{}{}{}".format(addondir, env["platform"], libname, env["suffix"], env["SHLIBSUFFIX"])

library = env.SharedLibrary(target_path, source=sources)
env.NoCache(library)
Default(library)

# `scons package`: build, then zip the addon into dist/.
def package_action(target, source, env):
    package.make_zip()
    return 0


package_zip = env.Command("#dist/.package-stamp", library, package_action)
env.AlwaysBuild(package_zip)
env.Alias("package", package_zip)
