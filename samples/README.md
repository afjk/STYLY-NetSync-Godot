# Samples

Both scenes are part of this repository's Godot project, so you can open it and
press play. To use them in your own project, copy the scene and its script
alongside `addons/styly_netsync/`.

Start a server first:

```bash
python3 -m styly_netsync
```

## `basic/basic_demo.tscn`

Everything the API does, with no XR hardware. Enter a server address (or leave
it empty to discover one on the LAN) and a room, press **Connect**, and:

* drive the local avatar with the **arrow keys** — the camera stays put, so you
  can watch it move;
* watch the connection state, your client number and everyone in the room;
* send RPCs, set global and client network variables;
* grab and nudge a shared cube, to see ownership move.

Remote clients appear as blue heads with a nose (so you can tell which way they
face) and orange hands. Run a second copy of the project, a `netsync_probe`, or
a Unity client in the same room to see them meet.

This is also the quickest way to sanity-check a build: if the log reaches
`state -> ready` with a client number, the whole stack is working.

## `xr/xr_demo.tscn`

The same client driven by an OpenXR rig. `NetSyncXRAdapter` finds the
`XRCamera3D` and the two `XRController3D` nodes under `XROrigin3D` and wires
them into the local `NetSyncAvatar`; the rig's movement away from its starting
pose is sent as the locomotion delta, which is what lets peers reconstruct each
user's real-world position.

Untracked controllers are hidden rather than frozen, so peers see a hand
disappear when you put it down — the protocol carries per-part validity flags.

Without a headset the scene still runs as a flat window (OpenXR fails to
initialise and it says so), which is enough to watch remote avatars.

To deploy to Quest or PICO, build the Android library first — see
[docs/BUILD.md](../docs/BUILD.md#android) — and remember to grant the `INTERNET`
permission in the export preset.
