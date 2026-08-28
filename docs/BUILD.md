# Building

The addon has two halves: a GDScript layer under `addons/styly_netsync/` that
ships as source, and a native GDExtension you build once per platform. libzmq is
compiled from the vendored submodule and linked **statically**, so the finished
addon has no runtime ZeroMQ dependency.

## Prerequisites

| | |
|---|---|
| Git | for the submodules |
| Python 3.8+ | SCons and the build scripts |
| SCons 4.0+ | `pip install scons` |
| CMake 3.16+ | builds libzmq |
| A C++17 compiler | GCC 9+, Clang 10+, or MSVC 2019+ |
| Godot | 4.3+ (4.4+ recommended — see [UPSTREAM_COMPATIBILITY.md](UPSTREAM_COMPATIBILITY.md#known-gaps)) |

Ninja is used for the libzmq build when present, and is worth installing.

## Get the sources

```bash
git clone https://github.com/afjk/STYLY-NetSync-Godot.git
cd STYLY-NetSync-Godot
git submodule update --init --recursive
```

The submodules are `third_party/godot-cpp` (Godot's C++ bindings) and
`third_party/libzmq` (ZeroMQ). Both are pinned; see
[UPSTREAM_COMPATIBILITY.md](UPSTREAM_COMPATIBILITY.md#vendored-dependencies).

## Desktop

```bash
scons target=template_debug          # host platform and architecture
```

That is enough to work in the editor and to run a debug export: the
`.gdextension` maps the editor's feature tag to the `template_debug` binary
deliberately, so you do not need a separate `target=editor` build. Before
shipping, add:

```bash
scons target=template_release
```

Explicit platform and architecture:

```bash
scons platform=linux   target=template_release arch=x86_64
scons platform=windows target=template_release arch=x86_64
scons platform=macos   target=template_release arch=arm64      # or arch=universal
```

The first build compiles godot-cpp and libzmq and takes a few minutes; later
builds reuse both. Output lands in `addons/styly_netsync/bin/`.

`scons -j$(nproc)` parallelises. `scons -c` cleans the extension objects;
`rm -rf build/libzmq` forces a libzmq rebuild.

### First time you open the project

Godot only discovers a `.gdextension` during the project scan that first sees
it — so on a fresh clone the **first** editor launch reports
`Could not find type "NetSyncBridge"` and the NetSync scripts fail to parse.
**Close and reopen the editor once** and it resolves. This is Godot's
bootstrapping order, not a fault in the addon; the enabled plugin prints a
warning explaining it if it happens.

## Android

Targets `arm64-v8a`, which is what Quest 2/3/Pro and PICO 4 run.

```bash
export ANDROID_NDK_ROOT=/path/to/android-ndk-r23c   # r23+ recommended
scons platform=android target=template_release arch=arm64
```

`ANDROID_NDK_ROOT` is what both halves of the build follow: the GDExtension
itself and the libzmq that gets linked into it. Left unset, godot-cpp falls back
to `ANDROID_HOME/ndk/<the version it pins>` (23.2.8568313 for godot-cpp 4.3),
which fails unless the SDK carries that exact version — so point
`ANDROID_NDK_ROOT` at the NDK you want and both agree. Both are built against
**API level 29** with `c++_shared`, matching Godot's Android export template —
one level for the whole `.so`, since the two halves are linked together. 29 is
also the floor imposed by the code: `getifaddrs`, which LAN discovery needs,
only appears in bionic at API 24. `android_api_level=` overrides it.

Both the debug and release variants are needed for the two Godot Android export
presets:

```bash
scons platform=android target=template_debug   arch=arm64
scons platform=android target=template_release arch=arm64
```

Notes for Quest and PICO:

* Add `INTERNET` permission to the export preset. NetSync is LAN networking:
  without it, connect and discovery both fail silently.
* Android 13+ (API 33) restricts UDP broadcast on some devices. If LAN discovery
  finds nothing, set `NetSyncManager.server_address` explicitly — discovery is
  then skipped entirely.
* The device id is persisted under `user://`, which is app-private storage and
  survives restarts and reboots. Uninstalling the app resets it, which
  re-assigns a client number on the next join.

**Built, but never run on a device.** CI compiles and links the arm64-v8a
library on every push (NDK r26d, API 29) and uploads it as an artifact, so the
toolchain path is real. Nothing in the client uses an API unavailable on
Android, but no build has been loaded onto a headset — treat the first run on
Quest or PICO as the unverified step.

## Building the tests

The protocol, transport and core layers build without Godot, through CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Nine suites: golden vectors, binary reader/writer, pose codec, JSON, coordinate
conversion, SUB topic routing, transport queues, discovery parsing, and client
lifecycle. The last three bring up real ZeroMQ sockets on loopback.

To link a system libzmq instead of the submodule (useful on a CI image that
already ships one):

```bash
cmake -S . -B build -DSTYLY_NETSYNC_SYSTEM_LIBZMQ=ON
```

### Regenerating the golden vectors

`tests/golden/vectors.json` is generated from the upstream Python serializer and
is committed, so the tests run offline. To regenerate or to check for drift:

```bash
python3 tests/tools/generate_golden_vectors.py            # clones upstream at the pinned commit
python3 tests/tools/generate_golden_vectors.py --check    # fails if the committed file is stale
python3 tests/tools/generate_golden_vectors.py --upstream /path/to/STYLY-NetSync
```

The generator imports only `binary_serializer.py`, by path, so it needs no
third-party Python packages and never modifies upstream.

### Integration tests

These need a real server:

```bash
pip install -e /path/to/STYLY-NetSync/STYLY-NetSync-Server

python3 tests/integration/test_against_server.py --probe build/netsync_probe
GODOT=/path/to/godot python3 tests/integration/test_godot_client.py
```

Each starts its own `NetSyncServer` on ephemeral ports, so they do not collide
with a server you already have running.

## Installing into your own project

Copy `addons/styly_netsync/` — including `bin/` — into your project's `addons/`
directory. Nothing else in this repository is needed at runtime.

You do not have to enable the plugin: `NetSyncManager`, `NetSyncObject`,
`NetSyncAvatar` and `NetSyncXRAdapter` all declare `class_name`, so Godot
registers them either way. Enabling it only adds a diagnostic that tells you when
the native library is missing.

## Layout

```
addons/styly_netsync/     the addon: GDScript API + built binaries
src/protocol/             wire format — no Godot, no ZeroMQ, no threads
src/transport/            ZeroMQ sockets, network thread, LAN discovery
src/core/                 client lifecycle, RPC, network variables, objects
src/godot/                the GDExtension binding and the coordinate boundary
tests/                    native suites, golden vectors, integration harnesses
samples/basic             keyboard-driven demo, no XR hardware needed
samples/xr                OpenXR rig driving a synchronised avatar
third_party/              godot-cpp and libzmq submodules
```

`src/protocol`, `src/transport` and `src/core` never include a Godot header;
`src/godot` is the only place the two meet. That boundary is what lets the
protocol be tested — and reused — without an engine.
