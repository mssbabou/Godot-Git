#!/usr/bin/env python
import os
import sys

sys.path.insert(0, os.path.join(Dir("#").abspath, "tools"))
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

env = SConscript("thirdparty/godot-cpp/SConstruct", {"env": env, "customs": customs, "api_version": "4.7"})

libgit2.setup(env, Dir("#").abspath)

env.Append(CPPPATH=["src/"])
sources = Glob("src/*.cpp")

if env["target"] in ["editor", "template_debug"]:
    try:
        doc_data = env.GodotCPPDocData("src/gen/doc_data.gen.cpp", source=Glob("doc_classes/*.xml"))
        sources.append(doc_data)
    except AttributeError:
        print("Not including class reference as we're targeting a pre-4.3 baseline.")

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
