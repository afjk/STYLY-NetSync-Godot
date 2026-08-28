# Unity ↔ Godot interoperability test

A manual procedure for confirming that a Godot client and a Unity client see
each other correctly in the same room.

**This has not been run** — no Unity installation was available during
development. What *has* been verified automatically is the layer underneath it:
every message this client emits is byte-identical to the upstream Python
serializer's output for the same input (47 cases, 2016 quaternion vectors), and
the client interoperates with the real STYLY NetSync server that Unity talks to
(40 checks). See [UPSTREAM_COMPATIBILITY.md](UPSTREAM_COMPATIBILITY.md). This
document is the remaining human-in-the-loop step.

## What you need

* A STYLY NetSync server, from upstream `STYLY-NetSync-Server`.
* A Unity project with `com.styly.styly-netsync` **0.17.4** and a
  `NetSyncManager` in the scene.
* This repository, built (`scons target=template_debug`), opened in Godot 4.4+.
* Both machines on the same LAN, or everything on one machine.

## Setup

Start the server, noting the address it prints:

```bash
python3 -m styly_netsync                    # 5555 control, 5556 pub, 5557 transform
```

**Unity.** On the `NetSyncManager` component set *Server Address* to the
server's IP (or leave it empty for LAN discovery) and *Room Id* to
`interop_test`. Assign the local and remote avatar prefabs so avatars are drawn.

**Godot.** Open `samples/basic/basic_demo.tscn`, run it, type the same server
address and `interop_test` as the room, and press **Connect**.

Both clients must use the **same room id**. They are separate identities: the
Godot client generates its own `deviceId`, so it gets its own client number.

---

## 1. Both clients join

**Expected**

* The Godot log shows `state -> ready` and a client number.
* The Godot *in room* line lists both client numbers.
* Unity's `NetSyncManager.OnAvatarConnected` fires with the Godot client's
  number, and a remote avatar appears.

**If it fails**

* A Godot client stuck at `synchronizing` has sockets up but no handshake —
  check that the server sees the hello, and that both clients use the same room.
* Nothing at all: check the firewall on ports 5555–5557, and that the Godot
  client's *Server address* has no scheme typo (`tcp://host` and `host` both
  work; `http://` does not).

---

## 2. Avatar pose, both directions — the important one

This is the check that the coordinate boundary is right in both directions.

**Godot → Unity.** In the Godot sample, drive the local avatar with the arrow
keys to a known spot — say four metres along Godot **−Z** (the direction the
avatar faces at rest), then turn 90° left.

**Expected in Unity:** the remote avatar is four metres along Unity **+Z**
(Unity's forward), turned 90° left as seen from above. Godot's −Z forward and
Unity's +Z forward are the same physical direction; a client that appeared
*behind* you instead would mean the Z flip is missing.

**Unity → Godot.** Walk (or drag the rig) to a known offset and turn.

**Expected in Godot:** the remote avatar cube mirrors the same physical motion.
Turning the Unity client to *its* left turns the Godot-side avatar to *its* left.

Check specifically:

| Motion | Both clients must agree |
|---|---|
| Move right | avatar moves to the same physical side |
| Move up | avatar moves up |
| Move forward | avatar moves the same way, not the opposite way |
| Yaw left | avatar turns the same way, not mirrored |
| Pitch head down | avatar looks down, not up |
| Roll head right | avatar rolls the same way |

A mirrored yaw with correct position means the rotation conversion is wrong but
the position conversion is right — the two are converted separately in
`src/godot/coordinate_converter.cpp`, so this narrows it precisely.

**Tolerance.** Head position is quantised at 0.01 m and rotation to a 10-bit
smallest-three quaternion (well under 0.25°), so a few millimetres and a
fraction of a degree of disagreement is the protocol working as designed.

---

## 3. Hands

With controllers or hand tracking in Unity, raise one hand.

**Expected in Godot:** the matching hand sphere moves to the same side and
height. Hand positions travel relative to the head, so an error here that does
*not* appear on the head points at the relative encoding.

Drop tracking (put a controller down, or take a hand out of view).

**Expected in Godot:** that hand hides rather than freezing in place — the pose
flags carry per-part validity.

---

## 4. RPC, both directions

**Godot → Unity.** In the Godot sample type `Hello` and an argument, press
**Send**.

**Expected in Unity:** `OnRPCReceived` fires with the Godot client's number, the
function name, and the argument array intact.

**Unity → Godot.** Call `NetSyncManager.Instance.Rpc("Hello", new[]{"from-unity"})`.

**Expected in Godot:** the log shows `RPC from client N: Hello(from-unity)`.

Also try:

* **A non-ASCII argument** (`日本語`) and one containing a quote and a backslash.
  RPC arguments are a JSON array, and this client reproduces Newtonsoft's
  escaping exactly — a garbled or truncated argument would mean it does not.
* **A targeted RPC** to a specific client number: it must reach only that client.
* Note that the server echoes a *broadcast* RPC back to its sender, on both
  clients. That is upstream behaviour, not a Godot quirk.

---

## 5. Network variables

**Global.** Set `phase` = `combat` from Godot.

**Expected in Unity:** `OnGlobalVariableChanged("phase", …, "combat")`, and
`GetGlobalVariable("phase")` returns `combat`. Then set it from Unity and
confirm the Godot log shows the change.

**Client.** Set `nickname` = `godot` from the Godot sample.

**Expected in Unity:** `GetClientVariable("nickname", <godot client number>)`
returns `godot`.

**Clear.** Press **Clear my client variables** in Godot.

**Expected in Unity:** `OnClientVariableChanged` fires with a null new value for
each cleared name — a client-variable sync is an authoritative snapshot, so
absence means removal.

---

## 6. Object sync and ownership

Object ids must match on both sides. Unity assigns them from its editor
pipeline (`GlobalObjectId`), which a Godot scene cannot reproduce, so **set the
same explicit id on both**:

* Unity: set the `NetSyncObject`'s serialised `_objectId` to a fixed value, for
  example `0x1234ABCD` (305441741).
* Godot: on the `SharedCube` node set `object_id` to `305441741` and clear
  `object_name`.

`NetSyncObject.object_name` hashes to a stable id for Godot-only scenes; it will
not match anything Unity assigns.

**Then:**

1. In Godot press **Grab / release cube**. Unity's `OnOwnershipChanged` must
   report the Godot client as the new owner.
2. Press **Nudge** a few times. The Unity object must follow, to within 0.01 m.
3. Grab it from Unity. The Godot log must show the ownership move, and the Godot
   cube must start following Unity's.
4. From Godot (now a non-owner) press **Grab / release cube** twice — the second
   press is a release by a non-owner and must come back as
   `object_ownership_rejected` with reason 1.

---

## 7. Disconnect

Stop the Godot client.

**Expected in Unity:** `OnAvatarDisconnected` fires within a couple of broadcast
intervals and the remote avatar is removed. Then stop Unity and confirm the
Godot log shows `client N left`.

---

## 8. Reconnect identity

Restart the Godot client without deleting `user://styly_netsync_device_id.txt`.

**Expected:** it rejoins with the **same client number**, because the server
keys room membership on the persisted `deviceId`. A new number means the id was
not persisted — check that `user://` is writable.

---

## Recording a result

When you run this, please record in
[UPSTREAM_COMPATIBILITY.md](UPSTREAM_COMPATIBILITY.md): the Unity package
version, the Godot version, the server version, the platforms on both ends, and
which sections passed. That turns "should interoperate" into "did, on these
versions".
