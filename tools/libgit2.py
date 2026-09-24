"""Builds libgit2 as a static library with CMake and wires it into a SCons environment.

libgit2 is configured once per platform/arch and installed into
build/libgit2/<platform>.<arch>/. Delete that folder (or pass
`rebuild_libgit2=yes`) to force a fresh build.
"""

import os
import shutil
import subprocess

LIBGIT2_OPTIONS = {
    "BUILD_SHARED_LIBS": "OFF",
    "BUILD_TESTS": "OFF",
    "BUILD_CLI": "OFF",
    "BUILD_EXAMPLES": "OFF",
    "BUILD_FUZZERS": "OFF",
    "USE_BUNDLED_ZLIB": "ON",
    "REGEX_BACKEND": "builtin",
    "USE_HTTP_PARSER": "builtin",
    # SSH remotes run the system's `ssh` client, so the user's keys, agent and ~/.ssh/config just work.
    "USE_SSH": "exec",
    "USE_GSSAPI": "OFF",
}


def _run(cmd):
    print("libgit2: " + " ".join(cmd))
    subprocess.check_call(cmd)


def _cmake_args(env):
    platform = env["platform"]
    arch = env["arch"]
    options = dict(LIBGIT2_OPTIONS)
    args = []

    if platform == "windows":
        if env.get("use_mingw", False):
            args += ["-G", "MinGW Makefiles"]
        else:
            msvc_arch = {"x86_64": "x64", "x86_32": "Win32", "arm64": "ARM64"}.get(arch)
            if msvc_arch:
                args += ["-A", msvc_arch]
            options["STATIC_CRT"] = "ON" if env.get("use_static_cpp", True) else "OFF"
        # WinHTTP ships with Windows, so HTTPS needs no extra dependency.
        options["USE_HTTPS"] = "WinHTTP"
    elif platform == "macos":
        options["USE_HTTPS"] = "SecureTransport"
        options["USE_ICONV"] = "ON"
        options["CMAKE_OSX_ARCHITECTURES"] = "arm64;x86_64" if arch == "universal" else arch
        # Same minimum as the extension itself (set in SConstruct); must never be "default",
        # which would mean "the macOS version of the build machine".
        target = env.get("macos_deployment_target", "10.13")
        options["CMAKE_OSX_DEPLOYMENT_TARGET"] = "10.13" if target == "default" else target
    elif platform == "linux":
        # Loads OpenSSL at runtime, so the extension doesn't hard-depend on a libssl version.
        options["USE_HTTPS"] = "OpenSSL-Dynamic"
        options["CMAKE_POSITION_INDEPENDENT_CODE"] = "ON"
        # One section per function, so the extension's --gc-sections link can drop unused ones.
        options["CMAKE_C_FLAGS"] = "-ffunction-sections -fdata-sections"
    else:
        raise RuntimeError("libgit2 build is not set up for platform '%s' yet." % platform)

    args += ["-D%s=%s" % (k, v) for k, v in options.items()]
    return args


def _system_libs(env):
    platform = env["platform"]
    if platform == "windows":
        return ["winhttp", "rpcrt4", "crypt32", "ole32", "ws2_32", "secur32", "advapi32"]
    if platform == "linux":
        return ["pthread", "dl", "rt"]
    return ["iconv"]


def setup(env, root_dir):
    source_dir = os.path.join(root_dir, "thirdparty", "libgit2")
    if not os.path.isfile(os.path.join(source_dir, "CMakeLists.txt")):
        raise RuntimeError("thirdparty/libgit2 is empty. Run: git submodule update --init --recursive")

    triple = "%s.%s" % (env["platform"], env["arch"])
    build_dir = os.path.join(root_dir, "build", "libgit2", triple, "cmake")
    install_dir = os.path.join(root_dir, "build", "libgit2", triple, "install")
    lib_dir = os.path.join(install_dir, "lib")
    lib_file = "git2.lib" if env["platform"] == "windows" and not env.get("use_mingw", False) else "libgit2.a"

    if env.get("rebuild_libgit2", False):
        shutil.rmtree(os.path.dirname(build_dir), ignore_errors=True)

    if not env.GetOption("clean") and not os.path.isfile(os.path.join(lib_dir, lib_file)):
        if shutil.which("cmake") is None:
            raise RuntimeError("CMake is required to build libgit2 but was not found on PATH.")
        config = "RelWithDebInfo"
        _run(
            ["cmake", "-S", source_dir, "-B", build_dir, "-DCMAKE_BUILD_TYPE=" + config, "-DCMAKE_INSTALL_PREFIX=" + install_dir]
            + _cmake_args(env)
        )
        _run(["cmake", "--build", build_dir, "--config", config, "--parallel"])
        _run(["cmake", "--install", build_dir, "--config", config])

    env.Append(CPPPATH=[os.path.join(install_dir, "include")])
    env.Append(LIBPATH=[lib_dir])
    env.Append(LIBS=["git2"] + _system_libs(env))
    if env["platform"] == "macos":
        env.Append(LINKFLAGS=["-framework", "CoreFoundation", "-framework", "Security"])
