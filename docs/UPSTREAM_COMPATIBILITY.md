# Upstream compatibility

What this client was built against, what was actually verified, and where it
knowingly differs.

## Pinned upstream revision

| | |
|---|---|
| Repository | `https://github.com/styly-dev/STYLY-NetSync` |
| Branch | `develop` |
| Commit | **`72fc88409d36e146b03f2d90bd11ebd9998408a3`** |
| Commit subject | `chore: update STYLY XR Rig to 0.4.22 (#516)` |
| Protocol version | **8** |
| Server package version | `styly-netsync` **0.17.4** |
| Unity package version | `com.styly.styly-netsync` 0.17.4 |

Upstream is used strictly read-only. Nothing in this repository modifies,
vendors or forks it; the golden-vector generator clones it at the commit above
and imports one module by path.

To re-pin after an upstream change, update `UPSTREAM_COMMIT` in
`tests/tools/generate_golden_vectors.py`, regenerate
`tests/golden/vectors.json`, and re-run the suites below. A protocol change will
show up as a golden mismatch rather than as silent drift.

## Vendored dependencies

| Component | Version | Commit |
|---|---|---|
| `godot-cpp` | branch `4.3` | `d5cc777a89d899665fb61f1650ef0dc0cf6488c4` |
| `libzmq` | `v4.3.5` | `622fc6dde99ee172ebaa9c8628d85a7a1995a21d` |

Both are git submodules; neither is patched. Licences are recorded in
[`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

## What was verified

### Byte-level protocol conformance — automated

`tests/protocol/test_golden_vectors.cpp` feeds the **upstream Python serializer**
a fixed set of inputs and asserts this implementation produces the identical
bytes. Nothing is compared against a hand-written expectation.

| | Count | Result |
|---|---|---|
| Message cases encoded and byte-compared | 47 | exact match |
| Message cases decoded and field-checked | 49 | pass |
| Quaternion codec vectors (random + edge cases) | 2016 | exact match |
| Quantisation vectors (4 scales × 26 values) | 104 | exact match |

Covered message types: `CLIENT_HELLO`, `CLIENT_POSE` (stealth, head-only,
head+physical, full body with hands and virtuals, moving-floor-local, clamping
extremes, rounding boundaries, degenerate quaternions, over-cap virtuals, and
six random poses), `ROOM_POSE`, `RPC` (broadcast, single target, 20 targets,
escaped and non-ASCII arguments), `DEVICE_ID_MAPPING`, `GLOBAL_VAR_SET`,
`GLOBAL_VAR_SYNC`, `CLIENT_VAR_SET`, `CLIENT_VAR_SYNC`, `CLIENT_VAR_CLEAR`,
`OBJECT_POSE`, `ROOM_OBJECTS`, `OBJECT_OWNERSHIP_REQUEST`,
`OBJECT_OWNERSHIP_CHANGED`, `OBJECT_OWNERSHIP_REJECTED`.

`OBJECT_OWNERSHIP_REQUEST` has no serializer upstream in Python (only a parser),
so it is verified the other way round: this implementation's bytes are fed back
through its own parser and the fields checked, and separately accepted by the
real server in the integration run below.

### Live server interoperability — automated

`tests/integration/test_against_server.py` starts a real
`styly_netsync.NetSyncServer` **0.17.4** and drives the native client against it.
**40 checks, all passing:**

* client hello accepted, client number assigned, `IsReady`-equivalent reached;
* server version reported through `MSG_DEVICE_ID_MAPPING`;
* two clients see each other join (`avatar_connected` both ways);
* pose upload and relay, verified to 0.01 m and 0.5° after quantisation;
* RPC broadcast (including the server's echo to the sender), targeted RPC, and
  confirmation that a targeted RPC does **not** reach a non-target;
* global network variables set and observed by a peer;
* client network variables set, observed, and removed by `CLIENT_VAR_CLEAR`
  (verified as a `removed` change, i.e. authoritative-snapshot semantics);
* object ownership: request granted, pose relayed through `ROOM_OBJECTS`,
  ownership transferred to a second client, a release by a non-owner rejected
  with reason code 1, and a legitimate release;
* stealth client: joins, appears in the ID mapping flagged stealth, drives
  network variables, and publishes no avatar pose;
* reconnect with the same `deviceId` is reassigned the same client number;
* **LAN discovery**: a client started with no configured address finds the
  server, is told all three ports, and completes the handshake on them.

### Godot end-to-end — automated

`tests/integration/test_godot_client.py` runs two headless Godot clients through
the addon against the real server. **14 checks, all passing.** The one that
matters most for interoperability:

> A pose set at Godot `(1.25, 1.6, −3.5)` with yaw 37° arrives at the peer as
> Godot `(1.25, 1.6, −3.5)` with yaw 36.995° — **position error 0.0 m**, yaw
> error 0.005°.

The test pose is deliberately asymmetric on every axis and not axis-aligned, so
a swapped axis or an inverted rotation sense cannot pass by cancelling out. Also
verified: head-relative hand position, shared-object pose and ownership, global
network variable, and an RPC carrying a non-ASCII, quote-containing argument.

### Environments tested

| | |
|---|---|
| OS | Linux x86_64 (Ubuntu 24.04, glibc 2.39) |
| Compiler | GCC 13.3.0, C++17 |
| Godot | **4.3.stable** and **4.4.1.stable** (official Linux builds) |
| Python | 3.11.15 |
| libzmq | 4.3.5, built from the vendored submodule, linked statically |

**Not yet run on real hardware.** macOS, Windows, Android/Quest/PICO and XR
device paths are implemented and the build is parameterised for them, but this
work was verified only on Linux desktop. See *Known gaps* below.

## Known differences from upstream

These are places where upstream's own implementations disagree, or where this
port makes a deliberate choice. None of them affect the verified cases above.

### 1. Degenerate-quaternion epsilon

Python treats a quaternion as degenerate when `magnitude² <= 1e-12`; Unity uses
`1e-10`. A quaternion whose squared magnitude falls between the two collapses to
identity on one side and not the other.

**This client follows Python (`1e-12`)**, since the Python serializer is the
golden-test baseline. The affected inputs are ~1e-5-scale quaternions, which no
tracking system produces.

### 2. Float versus double arithmetic

Unity quantises in `float`; Python quantises in `double`. At an exact `.5`
rounding boundary the two can emit different bytes for the same nominal input.

**This client computes in `double`**, matching Python exactly. Against Unity the
residual is at most one quantisation step (0.01 m, 0.005 m, or one 10-bit
quaternion step) on inputs that land precisely on a boundary — below the
protocol's own resolution.

### 3. Variable name and value truncation

Python slices by Unicode code point (`name[:64]`); Unity slices by UTF-16 code
unit (`name.Substring(0, 64)`). These agree for all BMP text and differ only for
astral characters (emoji, rare CJK extensions), where one UTF-16 slice can cut a
surrogate pair.

**This client slices by code point**, matching Python, and additionally never
splits a UTF-8 sequence.

### 4. Stealth flag sanitisation

Python force-reduces `flags` to `IsStealth` alone when the stealth bit is set;
Unity relies on its pose builder never setting the other bits. The results agree
for any conforming sender.

**This client applies Python's rule**, so a caller that sets stealth together
with body-part flags still produces a conforming frame.

### 5. Non-finite pose values

Python raises on `NaN`/`Inf` in a pose (the message is then never sent). Unity's
`Mathf.RoundToInt` produces an unspecified value.

**This client saturates infinities to the quantisation limits and maps `NaN` to
zero**, so one bad float from a misbehaving tracker cannot take the connection
down or emit an unspecified byte.

### 6. Decode location

Upstream Unity decodes incoming payloads on the network thread and queues typed
objects. **This client queues the raw payload and decodes inside `poll()`**, on
the caller's thread. The observable semantics are identical — including the
depth-1 latest-wins room-pose queue — and it keeps all state single-threaded.

### 7. Device ID provenance

Unity resolves `deviceId` through STYLY's Device-ID-Provider (with an Android
permission flow), falling back to `SystemInfo.deviceUniqueIdentifier`.

**This client generates a UUID v4 on first run and persists it** under `user://`
(app-private storage on Android, so it survives restarts), or takes an explicit
id from the application. The protocol semantics are fully preserved — the id is
stable, opaque and used identically — but a device running both the Unity and
the Godot client will present **two different identities**, and therefore two
client numbers, to the same room. Set `NetSyncManager.device_id` explicitly if
you need them to coincide.

### 8. Not implemented

Deliberately out of scope, with the reasoning:

* **Moving floor (`NetSyncMovingFloor`).** The `MovingFloorLocal` pose flag and
  its encoding *are* implemented, encoded, decoded and golden-tested, so a Godot
  client correctly receives and displays a Unity client standing on a moving
  floor. What is missing is the Godot-side authoring component for *declaring*
  one. A Godot client can send moving-floor-local poses through
  `NetSyncManager.set_local_pose({"moving_floor_local": true, "physical": …})`.
* **Human presence prefabs** and the avatar-smoothing/interpolation machinery
  (`NetSyncSmoothing`, `NetSyncTransformApplier`). These are Unity presentation
  concerns, not protocol. `NetSyncAvatar` and `NetSyncObject` implement
  half-life smoothing of their own.
* **REST bridge.** The port discovered through `MSG_DEVICE_ID_MAPPING`/discovery
  is surfaced as `get_discovered_rest_api_port()`, but no HTTP client is
  provided — Godot's `HTTPRequest` covers it.
* **Offline mode.** Upstream's loopback-without-a-server mode is a convenience,
  not protocol.

## Known gaps

* **Unity ↔ Godot has not been run.** No Unity installation was available. The
  protocol is byte-verified against the shared Python serializer and interop is
  verified against the real server that Unity talks to, which together pin the
  wire format — but the final Unity-in-the-loop check is a manual procedure,
  written up in [`INTEROP_TEST.md`](INTEROP_TEST.md).
* **Android has not been built or run.** No NDK was available.
  `scripts/build_libzmq.py` implements the arm64-v8a path (API 29,
  `c++_shared`) and no API used by the client is unavailable on Android, but
  this is untested. See [`BUILD.md`](BUILD.md#android).
* **macOS and Windows have not been built.** The SCons and CMake paths handle
  them and the code has no Linux-only dependency (socket code is behind
  `_WIN32` branches), but neither has been compiled.
* **Godot 4.3 headless editor shutdown.** `godot --headless --editor --quit`
  crashes intermittently (2 of 5 runs) at shutdown with this addon enabled.
  Running the game is unaffected (0 of 3), and **Godot 4.4.1 does not exhibit it
  at all** (0 of 5). It is a Godot-side shutdown race; prefer 4.4+ for CI that
  imports headlessly.
