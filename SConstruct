# SPDX-License-Identifier: Apache-2.0
#
# SCons build for the STYLY NetSync GDExtension.
#
#   scons target=template_debug                 # host platform, auto-detected
#   scons platform=linux   target=template_release
#   scons platform=windows target=template_release
#   scons platform=macos   target=template_release arch=arm64
#   scons platform=android target=template_release arch=arm64
#
# libzmq is built once per (platform, arch, target) by scripts/build_libzmq.py
# and linked statically, so the shipped addon has no runtime ZeroMQ dependency.
# See docs/BUILD.md.

import os
import subprocess
import sys

EnsureSConsVersion(4, 0)

if ARGUMENTS.get("platform") == "android":
    # godot-cpp resolves the NDK as ANDROID_HOME/ndk/<the version it pins> and
    # only falls back to ANDROID_NDK_ROOT when ANDROID_HOME is unset. On a
    # machine whose SDK does not carry that exact version — CI images included —
    # that points the build at an NDK which is not installed, while a perfectly
    # good one is named by ANDROID_NDK_ROOT. Prefer the NDK that was named
    # explicitly, which is the one scripts/build_libzmq.py picks too, so both
    # halves of the build use one toolchain. An explicit ANDROID_HOME= on the
    # command line still wins.
    if "ANDROID_HOME" not in ARGUMENTS and os.environ.get("ANDROID_NDK_ROOT"):
        os.environ["ANDROID_HOME"] = ""

    # The same goes for the API level: libzmq and this extension are linked into
    # one .so, so building them against different bionic headers is incoherent.
    # godot-cpp defaults to 21, and below 24 bionic hides getifaddrs(), which
    # server discovery needs. Take libzmq's level as the single answer; an
    # explicit android_api_level= on the command line still wins.
    sys.path.insert(0, "scripts")
    from build_libzmq import ANDROID_API_LEVEL

    ARGUMENTS.setdefault("android_api_level", ANDROID_API_LEVEL)

env = SConscript("third_party/godot-cpp/SConstruct")

# --- Our sources -------------------------------------------------------------

env.Append(CPPPATH=["src"])

sources = (
    Glob("src/protocol/*.cpp")
    + Glob("src/transport/*.cpp")
    + Glob("src/core/*.cpp")
    + Glob("src/godot/*.cpp")
)

# --- libzmq ------------------------------------------------------------------

platform = env["platform"]
arch = env["arch"]
target = env["target"]

libzmq_root = os.path.join("third_party", "libzmq")
if not os.path.isfile(os.path.join(libzmq_root, "CMakeLists.txt")):
    print("ERROR: third_party/libzmq is empty. Run: git submodule update --init --recursive")
    Exit(1)

libzmq_prefix = os.path.abspath(
    os.path.join("build", "libzmq", "{}-{}-{}".format(platform, arch, target))
)


def libzmq_static_path(prefix):
    """Locate the static libzmq inside an install prefix."""
    for lib_dir in ("lib", "lib64"):
        for name in ("libzmq.a", "libzmq-v143-mt-s-4_3_5.lib", "libzmq-mt-s-4_3_5.lib", "zmq.lib"):
            candidate = os.path.join(prefix, lib_dir, name)
            if os.path.isfile(candidate):
                return candidate
    return None


static_libzmq = libzmq_static_path(libzmq_prefix)
if static_libzmq is None:
    print("Building libzmq for {}-{}-{} ...".format(platform, arch, target))
    command = [
        sys.executable,
        os.path.join("scripts", "build_libzmq.py"),
        "--platform",
        platform,
        "--arch",
        arch,
        "--target",
        target,
        "--prefix",
        libzmq_prefix,
    ]
    if subprocess.call(command) != 0:
        print("ERROR: libzmq build failed. See docs/BUILD.md for prerequisites.")
        Exit(1)
    static_libzmq = libzmq_static_path(libzmq_prefix)
    if static_libzmq is None:
        print("ERROR: libzmq built but no static library was found under " + libzmq_prefix)
        Exit(1)

env.Append(CPPPATH=[os.path.join(libzmq_root, "include")])
env.Append(CPPDEFINES=["ZMQ_STATIC"])
env.Append(LIBS=[File(static_libzmq)])

# Platform link requirements of libzmq itself.
if platform == "windows":
    env.Append(LIBS=["ws2_32", "iphlpapi", "rpcrt4"])
elif platform in ("linux", "android"):
    env.Append(LIBS=["pthread"])
    if platform == "linux":
        env.Append(LIBS=["rt"])

# --- Output ------------------------------------------------------------------

addon_bin = os.path.join("addons", "styly_netsync", "bin")

if platform == "macos":
    library = env.SharedLibrary(
        "{}/libstyly_netsync.{}.{}.framework/libstyly_netsync.{}.{}".format(
            addon_bin, platform, target, platform, target
        ),
        source=sources,
    )
else:
    library = env.SharedLibrary(
        "{}/libstyly_netsync{}{}".format(addon_bin, env["suffix"], env["SHLIBSUFFIX"]),
        source=sources,
    )

Default(library)
