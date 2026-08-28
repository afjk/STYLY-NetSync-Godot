#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the vendored libzmq as a static library for one target.

Called automatically by SConstruct, and usable on its own:

    python3 scripts/build_libzmq.py --platform android --arch arm64 \
        --target template_release --prefix build/libzmq/android-arm64-release

Android requires ANDROID_NDK_ROOT (or ANDROID_HOME/ndk/<version>) to be set;
everything else builds with the host toolchain CMake finds. The build is
reproducible: the same submodule commit and the same flags produce the same
library, and nothing is downloaded.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
LIBZMQ_SOURCE = REPO_ROOT / "third_party" / "libzmq"

# Android API 29 is the floor for Quest 3 / PICO 4 era devices and is what the
# Godot Android export template targets.
ANDROID_API_LEVEL = "29"

ANDROID_ABI_BY_ARCH = {
    "arm64": "arm64-v8a",
    "arm32": "armeabi-v7a",
    "x86_64": "x86_64",
    "x86_32": "x86",
}

MACOS_ARCH_BY_ARCH = {
    "arm64": "arm64",
    "x86_64": "x86_64",
    "universal": "arm64;x86_64",
}


def find_android_ndk() -> Path:
    for variable in ("ANDROID_NDK_ROOT", "ANDROID_NDK_HOME", "ANDROID_NDK"):
        value = os.environ.get(variable)
        if value and Path(value).is_dir():
            return Path(value)

    android_home = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if android_home:
        ndk_root = Path(android_home) / "ndk"
        if ndk_root.is_dir():
            versions = sorted((p for p in ndk_root.iterdir() if p.is_dir()), reverse=True)
            if versions:
                return versions[0]

    raise SystemExit(
        "Android build requires the NDK. Set ANDROID_NDK_ROOT to your NDK "
        "installation (or ANDROID_HOME with an ndk/<version> subdirectory)."
    )


def cmake_arguments(platform: str, arch: str, target: str, prefix: Path) -> list[str]:
    build_type = "Debug" if target == "template_debug" else "Release"

    arguments = [
        "-S",
        str(LIBZMQ_SOURCE),
        "-DCMAKE_BUILD_TYPE=" + build_type,
        "-DCMAKE_INSTALL_PREFIX=" + str(prefix),
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        # Static only: the shipped addon must not need a system libzmq.
        "-DBUILD_SHARED=OFF",
        "-DBUILD_STATIC=ON",
        "-DBUILD_TESTS=OFF",
        "-DWITH_PERF_TOOL=OFF",
        "-DWITH_DOC=OFF",
        "-DENABLE_CPACK=OFF",
        # The client uses only the stable API and no CURVE security, so the
        # optional dependencies stay out.
        "-DENABLE_DRAFTS=OFF",
        "-DENABLE_CURVE=OFF",
        "-DWITH_TLS=OFF",
        "-DWITH_LIBSODIUM=OFF",
    ]

    if platform == "android":
        ndk = find_android_ndk()
        toolchain = ndk / "build" / "cmake" / "android.toolchain.cmake"
        if not toolchain.is_file():
            raise SystemExit(f"NDK toolchain file not found at {toolchain}")
        abi = ANDROID_ABI_BY_ARCH.get(arch)
        if abi is None:
            raise SystemExit(f"unsupported Android arch: {arch}")
        arguments += [
            "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain),
            "-DANDROID_ABI=" + abi,
            "-DANDROID_PLATFORM=android-" + ANDROID_API_LEVEL,
            "-DANDROID_STL=c++_shared",
            # Android has no getifaddrs before API 24 and libzmq probes for
            # several APIs that are absent in the NDK sysroot; leaving TIPC and
            # the pgm transports off keeps the probe set to what Android has.
            "-DWITH_TIPC=OFF",
        ]
    elif platform == "macos":
        osx_arch = MACOS_ARCH_BY_ARCH.get(arch, "arm64;x86_64")
        arguments += [
            "-DCMAKE_OSX_ARCHITECTURES=" + osx_arch,
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0",
        ]
    elif platform == "ios":
        arguments += ["-DCMAKE_SYSTEM_NAME=iOS", "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0"]
    elif platform == "windows":
        # Static CRT keeps the .dll self-contained.
        arguments += ["-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>"]

    return arguments


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--arch", default="x86_64")
    parser.add_argument("--target", default="template_release")
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--clean", action="store_true", help="Remove the build tree first")
    arguments = parser.parse_args()

    if not (LIBZMQ_SOURCE / "CMakeLists.txt").is_file():
        raise SystemExit(
            "third_party/libzmq is empty. Run: git submodule update --init --recursive"
        )
    if shutil.which("cmake") is None:
        raise SystemExit("cmake was not found on PATH; see docs/BUILD.md for prerequisites")

    prefix = Path(arguments.prefix).resolve()
    build_dir = prefix.parent / (prefix.name + "-build")
    if arguments.clean and build_dir.exists():
        shutil.rmtree(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)

    configure = ["cmake", "-B", str(build_dir)] + cmake_arguments(
        arguments.platform, arguments.arch, arguments.target, prefix
    )
    if shutil.which("ninja") is not None:
        configure += ["-G", "Ninja"]

    print("+ " + " ".join(configure))
    if subprocess.call(configure) != 0:
        return 1

    build = [
        "cmake",
        "--build",
        str(build_dir),
        "--target",
        "install",
        "--parallel",
        str(arguments.jobs),
    ]
    if arguments.platform == "windows":
        build += ["--config", "Debug" if arguments.target == "template_debug" else "Release"]

    print("+ " + " ".join(build))
    if subprocess.call(build) != 0:
        return 1

    print(f"libzmq installed into {prefix}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
