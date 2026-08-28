# STYLY NetSync — Protocol v8 (wire specification)

This document records the STYLY NetSync wire protocol **as implemented by upstream
`styly-dev/STYLY-NetSync` at commit `72fc88409d36e146b03f2d90bd11ebd9998408a3`
(branch `develop`)**. It is a description of observed upstream behaviour, derived by
reading the three authoritative implementations:

| Role | File |
|---|---|
| Unity client | `STYLY-NetSync-Unity/Packages/com.styly.styly-netsync/Runtime/Internal Scripts/BinarySerializer.cs` |
| Python server | `STYLY-NetSync-Server/src/styly_netsync/binary_serializer.py` |
| Python client | `STYLY-NetSync-Server/src/styly_netsync/client.py` |
| Simulator | `STYLY-NetSync-Server/src/styly_netsync/client_simulator.py` |
| Transport (Unity) | `.../Internal Scripts/ConnectionManager.cs` |
| Transport (server) | `STYLY-NetSync-Server/src/styly_netsync/server.py` |
| Discovery | `.../Internal Scripts/ServerDiscoveryManager.cs`, `src/styly_netsync/discovery.py`, `server.py` |

Nothing here is inferred or invented. Where the three implementations differ, the
difference is called out explicitly and recorded in
[`UPSTREAM_COMPATIBILITY.md`](UPSTREAM_COMPATIBILITY.md).

`PROTOCOL_VERSION = 8`.

---

## 1. Transport

### 1.1 Sockets

Three ZeroMQ sockets, all created by the client and all connecting outward:

| Lane | Socket type | Peer | Default port | Carries |
|---|---|---|---|---|
| Control | `DEALER` | server `ROUTER` | 5555 | `CLIENT_HELLO`, `RPC`, `GLOBAL_VAR_SET`, `CLIENT_VAR_SET`, `CLIENT_VAR_CLEAR`, `OBJECT_OWNERSHIP_REQUEST` (uplink); `DEVICE_ID_MAPPING`, `RPC`, `GLOBAL_VAR_SYNC`, `CLIENT_VAR_SYNC`, `OBJECT_OWNERSHIP_CHANGED`, `OBJECT_OWNERSHIP_REJECTED` (downlink) |
| Transform | `DEALER` | server `ROUTER` | 5557 | `CLIENT_POSE`, `OBJECT_POSE` (uplink only) |
| Broadcast | `SUB` | server `PUB` | 5556 | `ROOM_POSE`, `ROOM_OBJECTS` |

The server drops any message that arrives on the wrong lane
(`server.py::_drop_wrong_lane`), so lane assignment is normative, not advisory.

### 1.2 Framing

Every uplink message on both DEALER sockets is a **2-frame multipart message**:

```
frame 0 : roomId, UTF-8, no length prefix
frame 1 : binary payload (section 3)
```

The server's ROUTER prepends the client identity, so it sees 3 parts and requires
`len(parts) >= 3`.

Control downlink (server `ROUTER` → client `DEALER`) is the same 2-frame shape:
`[roomId, payload]`. The client must **drop any frame whose roomId does not equal
its own room** (`ConnectionManager.cs` compares `dealerRoomId == roomId`).

PUB downlink frames are `[topic, payload]`.

### 1.3 SUB topics — strict routing

The server publishes on two topics:

| Topic bytes | Message |
|---|---|
| `roomId` (UTF-8) | `MSG_ROOM_POSE` |
| `roomId` + `0x00 6F 62 6A` (`"\0obj"`) | `MSG_ROOM_OBJECTS` |

ZeroMQ `ZMQ_SUBSCRIBE` is a **prefix** filter, so subscribing to `roomId` also
delivers the object topic and — critically — the topics of any other room whose id
starts with `roomId`. Upstream therefore subscribes once with `roomId` and then
performs **byte-exact** classification in user space
(`ConnectionManager.IsAvatarTopic` / `IsObjectTopic`):

* avatar frame ⟺ `topic.length == roomId.length` and all bytes equal;
* object frame ⟺ `topic.length == roomId.length + 4`, room prefix equal, and the
  trailing four bytes are exactly `00 6F 62 6A`.

Anything else is ignored. Prefix matching alone is **not** conformant.

### 1.4 Socket options

Taken from `ConnectionManager.NetworkLoop` and cross-checked against
`client.py::_connect_sockets`; the two agree.

| Socket | Option | Value |
|---|---|---|
| Control DEALER | `LINGER` | 0 |
| Control DEALER | `SNDHWM` | 1024 |
| Control DEALER | `RCVHWM` | 1024 |
| Transform DEALER | `LINGER` | 0 |
| Transform DEALER | `SNDHWM` | 2 |
| SUB | `LINGER` | 0 |
| SUB | `RCVHWM` | 2 |
| SUB | `SUBSCRIBE` | `roomId` (UTF-8 bytes) |

All receives and sends are non-blocking (`ZMQ_DONTWAIT` / `TrySend*`). A send that
would block is **not** an error: it is backpressure and is retried on the next
network-loop iteration.

### 1.5 Send queue semantics

`ConnectionManager` maintains three outbound structures, drained every loop
iteration in this order (`FlushOutgoing`):

1. **Control outbox** — FIFO `ConcurrentQueue`, capacity `CTRL_OUTBOX_MAX = 256`,
   per-message TTL `CTRL_TTL_SECONDS = 5.0`, drained at most `maxBatch = 64` per
   iteration. Enqueue past capacity **drops the new message** and returns `false`
   (rate-limited warning, once per 5 s). Expired messages are discarded at drain
   time. On backpressure the head packet is retained and draining stops.
2. **Latest avatar transform** — a single slot, latest-wins. `SetLatestTransform`
   overwrites unconditionally. After a successful send the slot is cleared with a
   compare-exchange so a concurrent overwrite is not lost.
3. **Per-object latest transforms** — one latest-wins slot per `objectId`.
   Cleared only if the slot still holds the packet that was sent. On backpressure
   the loop stops draining objects for this iteration.

When neither a send nor a receive happened in an iteration, the loop sleeps 1 ms.

### 1.6 Threading

All socket I/O happens on a dedicated network thread. Received payloads are
decoded on that thread and the resulting events are pushed onto queues that the
main thread drains. Room-pose frames use a **latest-wins queue of depth 1**
(`MessageProcessor.MaxRoomTransformUpdatesQueueSize = 1`); when more than one
avatar frame is available in a single drain, only the newest is decoded and the
skipped count is accumulated as a diagnostic.

---

## 2. Message types

```
 1  MSG_CLIENT_TRANSFORM            (legacy; not emitted at v8)
 2  MSG_ROOM_TRANSFORM              (legacy; not emitted at v8)
 3  MSG_RPC
 4  MSG_RPC_SERVER                  (reserved — never sent, never parsed)
 5  MSG_RPC_CLIENT                  (reserved — never sent, never parsed)
 6  MSG_DEVICE_ID_MAPPING
 7  MSG_GLOBAL_VAR_SET
 8  MSG_GLOBAL_VAR_SYNC
 9  MSG_CLIENT_VAR_SET
10  MSG_CLIENT_VAR_SYNC
11  MSG_CLIENT_POSE
12  MSG_ROOM_POSE
13  MSG_OBJECT_POSE
14  MSG_ROOM_OBJECTS
15  MSG_OBJECT_OWNERSHIP_REQUEST
16  MSG_OBJECT_OWNERSHIP_CHANGED
17  MSG_OBJECT_OWNERSHIP_REJECTED
18  MSG_CLIENT_VAR_CLEAR
19  MSG_CLIENT_HELLO
```

Valid range check performed by both implementations:
`MSG_CLIENT_TRANSFORM <= type <= MSG_CLIENT_HELLO`, i.e. `1..19`. Types 1, 2, 4
and 5 are reserved/legacy and must not be repurposed.

Direction summary:

| Type | C→S | S→C | Lane |
|---|:--:|:--:|---|
| `RPC` (3) | ✔ | ✔ | control |
| `DEVICE_ID_MAPPING` (6) | | ✔ | control |
| `GLOBAL_VAR_SET` (7) | ✔ | | control |
| `GLOBAL_VAR_SYNC` (8) | | ✔ | control |
| `CLIENT_VAR_SET` (9) | ✔ | | control |
| `CLIENT_VAR_SYNC` (10) | | ✔ | control |
| `CLIENT_POSE` (11) | ✔ | | transform |
| `ROOM_POSE` (12) | | ✔ | SUB |
| `OBJECT_POSE` (13) | ✔ | | transform |
| `ROOM_OBJECTS` (14) | | ✔ | SUB |
| `OBJECT_OWNERSHIP_REQUEST` (15) | ✔ | | control |
| `OBJECT_OWNERSHIP_CHANGED` (16) | | ✔ | control |
| `OBJECT_OWNERSHIP_REJECTED` (17) | | ✔ | control |
| `CLIENT_VAR_CLEAR` (18) | ✔ | | control |
| `CLIENT_HELLO` (19) | ✔ | | control |

---

## 3. Primitive encoding

Everything is **little-endian**.

| Name | Encoding |
|---|---|
| `u8` / `i8` | 1 byte |
| `u16` / `i16` | 2 bytes LE |
| `u32` | 4 bytes LE |
| `i24` | 3 bytes LE, two's complement, sign-extended on read |
| `f64` | IEEE-754 binary64, 8 bytes LE |
| `str8` | `u8` byte length, then that many UTF-8 bytes |
| `str16` | `u16` byte length, then that many UTF-8 bytes |

`str8` fields: `deviceId`, `roomId`, RPC `functionName`, variable `name`.
`str16` fields: RPC `argumentsJson`, variable `value`.

Length limits enforced by the senders:

* `deviceId` — truncated to 255 **bytes** (`Math.Min(bytes.Length, 255)`), except in
  `SerializeRPCMessageInto` where a device id longer than 255 bytes raises instead.
* `functionName` — >255 bytes raises.
* variable `name` — truncated to **64 characters** before UTF-8 encoding.
* variable `value` — truncated to **1024 characters** before UTF-8 encoding.

---

## 4. Pose encoding

### 4.1 Constants

```
ABS_POS_SCALE      = 0.01     # metres per unit, head + object absolute position (i24)
LOCO_POS_SCALE     = 0.01     # metres per unit, XR-origin delta / moving-floor-local position (i16)
REL_POS_SCALE      = 0.005    # metres per unit, hand/virtual position relative to head (i16)
PHYSICAL_YAW_SCALE = 0.1      # degrees per unit (i16)

INT16 range : [-32768, 32767]
INT24 range : [-8388608, 8388607]
```

Quantisation is `clamp(round_half_to_even(value / scale))`. Both upstream
implementations round half-to-even: Python's `round()` and .NET's
`Math.Round(double)` (behind `Mathf.RoundToInt`) share that behaviour. Using
`std::round` (half-away-from-zero) would produce different bytes at exact `.5`
boundaries.

Dequantisation is `quantized * scale`.

### 4.2 Pose flags (`u8`)

```
0x01  IsStealth
0x02  PhysicalValid
0x04  HeadValid
0x08  RightValid
0x10  LeftValid
0x20  VirtualsValid
0x40  MovingFloorLocal
```

Sanitisation applied by the sender **before** the byte is written:

* Unity: if `HeadValid` is clear, `Right|Left|Virtuals` are cleared
  (`SanitizePoseFlags`).
* Python adds one more rule: if `IsStealth` is set, `flags` is reduced to exactly
  `IsStealth`. The Unity client reaches the same state by construction — its
  `BuildPoseFlags` returns early with only `IsStealth` in stealth mode.

`Right`, `Left` and `Virtuals` are additionally gated on `HeadValid` at *parse*
time by both implementations, so a non-conforming sender cannot desynchronise the
reader.

### 4.3 Encoding flags (`u8`)

```
0x01  PHYSICAL_YAW_ONLY
0x02  RIGHT_REL_HEAD
0x04  LEFT_REL_HEAD
0x08  VIRTUAL_REL_HEAD
0x10  PHYSICAL_IS_XRORIGIN_DELTA
```

The byte is **derived, never chosen**:

```
encoding = 0x1F
if flags & MovingFloorLocal: encoding &= ~0x10
```

A reader that sees `PhysicalValid` set, `MovingFloorLocal` clear and
`PHYSICAL_IS_XRORIGIN_DELTA` clear must reject the payload — both upstream
implementations raise there.

### 4.4 Quaternion codec — smallest-three, 32 bits

```
QUAT_COMPONENT_MIN = -0.70710677f
QUAT_COMPONENT_MAX =  0.70710677f
```

Encode:

1. Normalise the quaternion. Degenerate input (non-finite or magnitude² below the
   epsilon) becomes identity `(0,0,0,1)`.
   *Epsilon differs upstream: Python `1e-12`, Unity `1e-10`.*
2. `largestIndex` = index of the component with the greatest **absolute** value,
   scanning `x, y, z, w` in order and keeping the **first** maximum (strict `>`
   comparisons). Ties resolve to the lower index.
3. If `values[largestIndex] < 0`, negate all four components. (`q` and `-q` are the
   same rotation; this makes the encoding canonical, so sign has no effect on the
   emitted bytes.)
4. `packed = largestIndex << 30`.
5. For each `i` in `0..3`, skipping `largestIndex`, in ascending order, with
   `writeIndex` counting 0,1,2:
   ```
   clamped    = clamp(values[i], QMIN, QMAX)
   normalized = (clamped - QMIN) / (QMAX - QMIN)
   scaled     = clamp(round_half_to_even(normalized * 1023), 0, 1023)
   packed    |= scaled << (20 - writeIndex * 10)
   ```

So bits 31..30 hold the largest index, bits 29..20 the first written component,
bits 19..10 the second, bits 9..0 the third.

Decode:

```
largestIndex = (packed >> 30) & 3
a = (packed >> 20) & 0x3FF ; b = (packed >> 10) & 0x3FF ; c = packed & 0x3FF
decode(v) = QMIN + (QMAX - QMIN) * (v / 1023)
```
fill the three non-largest slots in ascending index order from `a, b, c`, then
`values[largestIndex] = sqrt(max(0, 1 - sumOfSquaresOfTheOtherThree))`, then
normalise the result.

### 4.5 Client pose body

Shared by `MSG_CLIENT_POSE` and every per-client record inside `MSG_ROOM_POSE`
(`_serialize_client_body` / `SerializeClientTransformInto`).

```
u16  poseSeq
u8   flags
u8   encodingFlags

if PhysicalValid:
    if MovingFloorLocal:
        i16 physical.position.x  / LOCO_POS_SCALE
        i16 physical.position.y  / LOCO_POS_SCALE
        i16 physical.position.z  / LOCO_POS_SCALE
        i16 yawDegrees(physical.rotation) / PHYSICAL_YAW_SCALE
    else:
        i16 xrOriginDelta.x   / LOCO_POS_SCALE
        i16 xrOriginDelta.y   / LOCO_POS_SCALE
        i16 xrOriginDelta.z   / LOCO_POS_SCALE
        i16 xrOriginDeltaYaw  / PHYSICAL_YAW_SCALE

if HeadValid:
    i24 head.position.x / ABS_POS_SCALE
    i24 head.position.y / ABS_POS_SCALE
    i24 head.position.z / ABS_POS_SCALE
    u32 packQuat(normalize(head.rotation))

if RightValid:                      # RightValid implies HeadValid
    i16 (right.position - head.position).x / REL_POS_SCALE
    i16 (right.position - head.position).y / REL_POS_SCALE
    i16 (right.position - head.position).z / REL_POS_SCALE
    u32 packQuat(inverse(normalize(head.rotation)) * normalize(right.rotation))

if LeftValid:                       # same shape as RightValid
    ...

u8   virtualCount                   # 0 unless VirtualsValid; min(count, 50)
repeat virtualCount times:
    i16 (virtual.position - head.position).x / REL_POS_SCALE
    i16 (virtual.position - head.position).y / REL_POS_SCALE
    i16 (virtual.position - head.position).z / REL_POS_SCALE
    u32 packQuat(inverse(normalize(head.rotation)) * normalize(virtual.rotation))
```

`MAX_VIRTUAL_TRANSFORMS = 50`. The relative position is the **unrotated** world
delta — it is *not* expressed in head space; only the rotation is relativised.

Note the asymmetry: the hand/virtual quaternion is relativised against the
*normalised, un-quantised* head rotation, but the receiver rebuilds it against the
*dequantised* head rotation. That is upstream behaviour and reproducing it is
required for pose agreement.

Yaw extraction (`QuaternionToYawDegrees`), on the normalised quaternion:

```
siny_cosp = 2 * (w*y + z*x)
cosy_cosp = 1 - 2 * (y*y + z*z)
yaw       = normalizeTo[-180,180)( degrees(atan2(siny_cosp, cosy_cosp)) )
```

### 4.6 Physical-pose reconstruction (receiver side)

The physical (real-world HMD) pose is never transmitted directly in the common
case; the receiver rebuilds it:

* `PhysicalValid && MovingFloorLocal` → physical position is the decoded i16
  triple; physical rotation is `yawQuaternion(decoded yaw)`.
* `PhysicalValid && HeadValid && !MovingFloorLocal` →
  ```
  translated  = headPos - xrOriginDelta
  physicalPos = rotateAroundY(translated, -xrOriginDeltaYaw)
  physicalYaw = normalizeTo[-180,180)( yawDegrees(headRot) - xrOriginDeltaYaw )
  physicalRot = yawQuaternion(physicalYaw)
  ```
  where `rotateAroundY(v, deg)` uses the Unity-handed form
  `(cos·x + sin·z, y, -sin·x + cos·z)`.
* `PhysicalValid && !HeadValid` → cannot be reconstructed; both implementations
  warn and leave the physical pose at identity.

---

## 5. Message layouts

### 5.1 `MSG_CLIENT_HELLO` (19) — C→S, control

```
u8   19
u8   8                  # PROTOCOL_VERSION
u8   flags              # 0x01 = stealth, else 0
str8 deviceId
```

### 5.2 `MSG_CLIENT_POSE` (11) — C→S, transform

```
u8   11
u8   8
str8 deviceId
<client pose body>      # section 4.5
```

The **stealth handshake** variant (`SerializeStealthHandshakeInto`) is the same
message with `poseSeq = 0`, `flags = IsStealth`, `encodingFlags = 0x1F` and
`virtualCount = 0`. Note it writes the *default* encoding byte rather than the
derived one; with `MovingFloorLocal` clear the two are identical.

### 5.3 `MSG_ROOM_POSE` (12) — S→C, SUB, topic `roomId`

```
u8   12
u8   8
str8 roomId
f64  broadcastTime      # server monotonic clock
u16  clientCount
repeat clientCount times:
    u16  clientNo
    f64  poseTime       # server monotonic clock
    <client pose body>  # section 4.5
```

`deviceId` is **not** present — it is resolved through the mapping table
(section 5.4).

### 5.4 `MSG_DEVICE_ID_MAPPING` (6) — S→C, control

```
u8   6
u8   serverVersionMajor
u8   serverVersionMinor
u8   serverVersionPatch
u16  mappingCount
repeat mappingCount times:
    u16  clientNo
    u8   isStealth      # 0x01 = stealth, else not
    str8 deviceId
```

Note: **no protocol-version byte.** The three version bytes come first.

The client's own `clientNo` is discovered by finding the mapping whose `deviceId`
equals the local device id. There is no dedicated "welcome" message.

### 5.5 `MSG_RPC` (3) — both directions, control

```
u8   3
u16  senderClientNo
str8 deviceId           # sender's device id (control identity binding)
u8   targetCount        # 0 = broadcast to the whole room
repeat targetCount times:
    u16 targetClientNo
str8  functionName
str16 argumentsJson
```

No protocol-version byte.

`argumentsJson` is `JsonConvert.SerializeObject(string[])` — a JSON array of JSON
strings, e.g. `["a","b"]`; an empty argument list is `[]`. The server relays the
field verbatim.

`targetCount > 255` is rejected by the sender. `targetClientNos` semantics: empty
⇒ every client in the room (including, per `server.py::_send_rpc_to_room`, the
sender); otherwise exactly the listed client numbers.

### 5.6 `MSG_GLOBAL_VAR_SET` (7) — C→S, control

```
u8    7
u16   senderClientNo
str8  deviceId
str8  variableName      # source string truncated to 64 characters first
str16 variableValue     # source string truncated to 1024 characters first
```

### 5.7 `MSG_GLOBAL_VAR_SYNC` (8) — S→C, control

```
u8   8
u16  variableCount
repeat variableCount times:
    str8  name
    str16 value
    u16   lastWriterClientNo
```

### 5.8 `MSG_CLIENT_VAR_SET` (9) — C→S, control

```
u8    9
u16   senderClientNo
str8  deviceId
u16   targetClientNo
str8  variableName      # truncated to 64 characters
str16 variableValue     # truncated to 1024 characters
```

### 5.9 `MSG_CLIENT_VAR_SYNC` (10) — S→C, control

```
u8   10
u16  clientCount
repeat clientCount times:
    u16 clientNo
    u16 variableCount
    repeat variableCount times:
        str8  name
        str16 value
        u16   lastWriterClientNo
```

Each included `clientNo` block is a **full authoritative snapshot** for that
client: names absent from the block have been removed.

### 5.10 `MSG_CLIENT_VAR_CLEAR` (18) — C→S, control

```
u8   18
u16  senderClientNo
str8 deviceId
```

Clears every client variable owned by the sender.

### 5.11 `MSG_OBJECT_POSE` (13) — C→S, transform

```
u8   13
u8   8
str8 deviceId
u32  objectId
u16  poseSeq
i24  position.x / ABS_POS_SCALE
i24  position.y / ABS_POS_SCALE
i24  position.z / ABS_POS_SCALE
u32  packQuat(normalize(rotation))
```

The server attributes the pose using the payload `deviceId`, not the socket
identity, so a stealth owner can drive objects.

### 5.12 `MSG_ROOM_OBJECTS` (14) — S→C, SUB, topic `roomId + "\0obj"`

```
u8   14
u8   8
f64  broadcastTime
u16  objectCount
repeat objectCount times:
    u32  objectId
    u16  ownerClientNo       # 0 = unowned
    u16  poseSeq
    f64  poseTime
    i24  position.x
    i24  position.y
    i24  position.z
    u32  packedRotation
```

Note there is **no roomId** in the body — the topic carries it.

### 5.13 `MSG_OBJECT_OWNERSHIP_REQUEST` (15) — C→S, control

```
u8   15
u8   8
str8 deviceId
u8   operationType       # 1 = release, 2 = request
u32  objectId
```

`objectId == 0` is ignored by both sides.

### 5.14 `MSG_OBJECT_OWNERSHIP_CHANGED` (16) — S→C, control

```
u8   16
u32  objectId
u16  newOwnerClientNo
u16  previousOwnerClientNo
```

No protocol-version byte. Sent to the whole room on every transition, and
additionally unicast to a newly joined client once per known object (with
`previousOwnerClientNo = 0`) as an ownership snapshot.

### 5.15 `MSG_OBJECT_OWNERSHIP_REJECTED` (17) — S→C, control

```
u8   17
u32  objectId
u16  currentOwnerClientNo
u8   reasonCode          # only 1 ("not owner") is currently emitted
```

No protocol-version byte. Emitted only when a non-owner attempts a release; a
request (`operationType = 2`) always succeeds upstream.

---

## 6. Connection lifecycle

1. Connect the three sockets and subscribe SUB to `roomId`.
2. **Immediately enqueue `MSG_CLIENT_HELLO`** as the first control message of the
   connection (`ConnectionManager.NetworkLoop` calls `EnqueueClientHello` before
   entering the loop).
3. The server assigns/looks up a `clientNo` for the `deviceId`, then, before
   releasing its room lock, unicasts `MSG_DEVICE_ID_MAPPING` to the new control
   identity. Only afterwards does it send the network-variable snapshot
   (`_sync_network_variables_to_new_client`) and the object-ownership snapshot
   (`_sync_objects_to_new_client`). This ordering is deliberate — see upstream
   `test_hello_mapping_ordering.py`.
4. The client learns its own `clientNo` from the mapping entry matching its
   `deviceId`.
5. Pose upload may begin at any time after connect; the server will create the
   room entry from a pose too, but the control lane still needs the hello.

**Ready** (`NetSyncManager.IsReady`) is the conjunction of three conditions:

```
HasServerConnection      : all three sockets exist and no connection error
HasHandshake             : clientNo > 0
HasNetworkVariablesSync  : an NV sync message arrived, OR 2.0 s elapsed since the
                           connection was established (INITIAL_SYNC_TIMEOUT)
```

The timeout exists because a room with no variables never produces a sync message.
Socket connection alone is explicitly **not** readiness.

On resume-from-pause the Unity client resets `clientNo = 0` and the NV initial-sync
flag so readiness is re-established against the new session.

---

## 7. Server discovery

Request (UDP broadcast to `serverDiscoveryPort`, default **9999**, and also over
TCP to the same port):

```
STYLY-NETSYNC-DISCOVER
```

Response:

```
STYLY-NETSYNC3|<controlPort>|<transformPort>|<pubPort>|<restApiPort>|<serverName>
```

The TCP response has a trailing `\n`; the UDP response does not.

Client acceptance rule (`ServerDiscoveryManager.ProcessDiscoveryResponse`): split
on `|`; accept **only** if there are at least 6 fields **and** field 0 is exactly
`STYLY-NETSYNC3`. Older `STYLY-NETSYNC2|` / `STYLY-NETSYNC|` replies are rejected
by clients (the server recognises them only to warn about port conflicts).

Client probe order, per upstream:

1. TCP probe to `127.0.0.1` (never cached — always available locally).
2. TCP probe to the cached last-known server IP, if any.
3. Platform-dependent broadcast phase: UDP broadcast every `DiscoveryInterval`
   (0.1 s) on every local NIC (desktop/Android), or a parallel TCP subnet scan
   (iOS/visionOS).

TCP connect timeout is 300 ms; the TCP read timeout is 1000 ms; UDP receive
timeout is 500 ms. On success the responder's IP is cached.

An explicitly configured server address skips discovery entirely.

---

## 8. Coordinate system

The wire format uses **Unity's** convention: Y-up, `+X` right, `+Z` forward,
left-handed. Godot is Y-up, `+X` right, `−Z` forward, right-handed.

Both bases share right and up; they differ only in the sign of the third axis, so
the component map is the reflection `M = diag(1, 1, −1)`, and it is its own
inverse. A rotation transforms by conjugation:

```
B_netsync = M · B_godot · M
B_godot   = M · B_netsync · M
```

Written out for a quaternion this is `(x, y, z, w) ↦ (−x, −y, z, w)` — but this
implementation derives that from the basis conjugation rather than asserting it,
and `tests/compatibility/test_coordinates.cpp` proves the two forms agree for
random rotations.

Consequently a yaw of `θ` degrees about `+Y` in Godot is `−θ` degrees about `+Y`
on the wire, which is what `xrOriginDeltaYaw` and the moving-floor-local physical
yaw require.

---

## 9. Known upstream divergences

These are places where the three upstream implementations do not agree byte-for-byte.
They are reproduced faithfully (Python chosen as the reference) and recorded in
[`UPSTREAM_COMPATIBILITY.md`](UPSTREAM_COMPATIBILITY.md).

1. **Degenerate-quaternion epsilon** — Python `1e-12`, Unity `1e-10`.
2. **Float width** — Unity quantises in `float`, Python in `double`. At exact
   rounding boundaries the two can emit different bytes.
3. **Variable-name/value truncation** — Python truncates by Unicode code point,
   Unity by UTF-16 code unit. Identical for BMP text, different for astral
   characters.
4. **Stealth flag sanitisation** — Python force-clears the transform-valid bits
   when `IsStealth` is set; Unity relies on its pose builder never setting them.
