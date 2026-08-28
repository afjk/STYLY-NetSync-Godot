# STYLY NetSync for Godot

A Godot Engine client for [STYLY NetSync](https://github.com/styly-dev/STYLY-NetSync),
wire-compatible with the existing NetSync server and Unity client.

Protocol **v8**, spoken byte for byte. Godot clients and Unity clients join the
same rooms, see each other's avatars, and share RPCs, network variables and
synchronised objects.

```gdscript
@onready var net_sync: NetSyncManager = $NetSyncManager

func _ready() -> void:
    net_sync.room_id = "my_room"
    net_sync.ready_to_sync.connect(_on_ready)
    net_sync.rpc_received.connect(_on_rpc)
    net_sync.connect_to_server()          # empty server_address discovers one on the LAN

func _on_ready() -> void:
    net_sync.set_global_variable("phase", "lobby")
    net_sync.send_rpc("Wave", PackedStringArray(["hello"]))

func _on_rpc(sender: int, name: String, args: PackedStringArray) -> void:
    print("client %d called %s(%s)" % [sender, name, ", ".join(args)])
```

## What it does

| | |
|---|---|
| **Avatar sync** | Head, both hands, arbitrary extra transforms, and the XR locomotion delta that lets peers reconstruct your real-world pose |
| **RPC** | Broadcast, single-target and multi-target, with string arguments |
| **Network variables** | Room-wide globals and per-client values, with change notifications |
| **Object sync** | Shared transforms with server-arbitrated ownership |
| **Discovery** | Finds a server on the LAN, or connect to a known address |
| **Stealth clients** | Join a room without an avatar — for tools, dashboards and headless services |

Built on ZeroMQ through a GDExtension, so it works from **GDScript** with no C#
dependency, on desktop and on Android (Quest, PICO).

## Requirements

* Godot **4.3+** (4.4+ recommended)
* A STYLY NetSync server — see
  [upstream](https://github.com/styly-dev/STYLY-NetSync/tree/develop/STYLY-NetSync-Server)

## Install

Copy `addons/styly_netsync/` — including `bin/` — into your project, then build
the native library once per platform:

```bash
git submodule update --init --recursive
scons target=template_debug          # editor + debug export
scons target=template_release        # before shipping
```

Full instructions, including Android and Quest/PICO, are in
[docs/BUILD.md](docs/BUILD.md).

> On a fresh clone Godot needs **one restart** before it picks up the extension;
> the first launch will report `Could not find type "NetSyncBridge"`. That is
> Godot's `.gdextension` discovery order, not a fault in the addon.

## Try it

```bash
python3 -m styly_netsync              # start a server
```

Open the project in Godot and run `samples/basic/basic_demo.tscn` — no XR
hardware needed. Enter the server address (or leave it empty to discover one),
press **Connect**, and drive the avatar with the arrow keys. Run a second copy,
or a Unity client, in the same room to see them meet.

`samples/xr/xr_demo.tscn` shows the same thing driven by an OpenXR rig.

## API

### NetSyncManager

The one node you add to your scene.

**Properties** — `server_address`, `room_id`, `device_id`, `stealth_mode`,
`transform_send_rate`, `auto_connect`, `control_port`, `transform_port`,
`sub_port`, `discovery_port`, and the read-only `client_no` and
`resolved_server_address`.

**Methods**

```gdscript
connect_to_server() -> bool
disconnect_from_server() -> void
is_connected_to_server() -> bool          # sockets up
is_ready() -> bool                        # connected + handshaken + variables synced
get_connection_state() -> int             # NetSyncManager.State

send_rpc(function_name, args := PackedStringArray()) -> void
send_rpc_to(client_no, function_name, args := PackedStringArray()) -> void
send_rpc_to_many(client_nos, function_name, args := PackedStringArray()) -> void

set_global_variable(name, value) -> bool
get_global_variable(name, default_value := "") -> String
set_client_variable(name, value) -> bool
set_client_variable_for(client_no, name, value) -> bool
get_client_variable(client_no, name, default_value := "") -> String
clear_client_variables() -> bool

get_remote_client_numbers() -> PackedInt32Array
get_known_client_numbers() -> PackedInt32Array   # includes stealth clients
get_remote_pose(client_no) -> Dictionary         # Godot space
is_client_stealth(client_no) -> bool

set_local_pose(pose: Dictionary) -> void         # or attach a NetSyncAvatar
register_object_id(object_id) -> bool            # or add a NetSyncObject node
submit_object_transform(object_id, transform) -> void
request_object_ownership(object_id) -> bool
release_object_ownership(object_id) -> bool
```

**Signals**

```gdscript
ready_to_sync
connection_state_changed(state: int, state_name: String)
connection_error(message: String)
server_discovered(address: String, server_name: String)
client_no_assigned(client_no: int)
avatar_connected(client_no: int, device_id: String)
avatar_disconnected(client_no: int)
rpc_received(sender_client_no: int, function_name: String, args: PackedStringArray)
global_variable_changed(name: String, old_value: String, new_value: String)
client_variable_changed(client_no: int, name: String, old_value: String, new_value: String)
object_ownership_changed(object_id: int, new_owner: int, previous_owner: int)
object_ownership_rejected(object_id: int, current_owner: int, reason: int)
server_version_received(major: int, minor: int, patch: int)
```

### NetSyncAvatar

A `Node3D` that publishes this client's pose, or applies a remote client's.
Assign `head`, `right_hand`, `left_hand` and any `virtual_transforms`; set
`is_local_avatar`, and `client_no` for remote ones. For an XR rig, add a
`NetSyncXRAdapter` child and it wires the camera and controllers for you.

### NetSyncObject

A `Node3D` whose world transform is shared. Set `object_id` (or `object_name`,
which hashes to a stable id for Godot-only scenes), then
`request_ownership()` / `release_ownership()`. The owner's transform is
authoritative; everyone else has theirs applied from the network.

**Object ids must match across clients**, including Unity — see
[docs/INTEROP_TEST.md](docs/INTEROP_TEST.md#6-object-sync-and-ownership).

## Readiness, not just connection

`is_connected_to_server()` means the sockets are up. `is_ready()` means the
client is actually usable: connected, assigned a client number by the server,
and holding the initial network-variable sync. Reading a network variable before
`ready_to_sync` returns the fallback. RPCs sent before then are queued and
flushed once ready, as the Unity client does.

## Coordinates

Godot is right-handed with −Z forward; the NetSync wire format is Unity's
left-handed +Z forward. The conversion happens in exactly one place —
`src/godot/coordinate_converter.cpp` — and is derived from the basis conjugation
`M · B · M` with `M = diag(1, 1, −1)`, not from hard-coded sign flips. The test
suite proves the closed form agrees with that conjugation over 20 000 random
rotations, and an end-to-end test confirms a pose set in Godot arrives at a peer
at the same Godot coordinates through a real server, with 0.0 m error.

You write Godot coordinates. A Unity peer sees you where you are.

## Compatibility

Verified against upstream commit
[`72fc884`](https://github.com/styly-dev/STYLY-NetSync/commit/72fc88409d36e146b03f2d90bd11ebd9998408a3)
(`develop`), server version 0.17.4, protocol v8 — on Linux x86_64 with Godot
4.3 and 4.4.1.

* **47 message cases** byte-identical to the upstream Python serializer, plus
  **2016** quaternion and **104** quantisation vectors.
* **40 checks** against a real STYLY NetSync server, LAN discovery included.
* **14 checks** end-to-end through the Godot addon.

Known differences, what has *not* been tested (Unity in the loop, Android,
macOS, Windows), and the exact pinned revisions are recorded in
[docs/UPSTREAM_COMPATIBILITY.md](docs/UPSTREAM_COMPATIBILITY.md). The wire
format itself is written up in [docs/PROTOCOL_V8.md](docs/PROTOCOL_V8.md).

## Testing

```bash
cmake -S . -B build && cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

pip install -e /path/to/STYLY-NetSync/STYLY-NetSync-Server
python3 tests/integration/test_against_server.py --probe build/netsync_probe
GODOT=/path/to/godot python3 tests/integration/test_godot_client.py
```

## Licence

Apache-2.0, matching upstream STYLY NetSync. See [LICENSE](LICENSE).

Third-party components — godot-cpp (MIT) and libzmq (MPL-2.0) — are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

This project does not modify upstream STYLY NetSync in any way. Compatibility is
achieved entirely on the Godot side.
