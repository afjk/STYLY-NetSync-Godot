#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate protocol v8 golden vectors from the upstream STYLY-NetSync serializer.

The upstream Python serializer is the interoperability baseline. This script
feeds it a fixed set of inputs and records the exact bytes it produces; the C++
test suite (tests/protocol/test_golden_vectors.cpp) rebuilds the same inputs and
asserts byte equality.

Upstream is never modified: it is cloned read-only at a pinned commit.

Usage:
    python3 tests/tools/generate_golden_vectors.py               # clone upstream, regenerate
    python3 tests/tools/generate_golden_vectors.py --upstream DIR
    python3 tests/tools/generate_golden_vectors.py --check       # fail if the file is stale
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = REPO_ROOT / "tests" / "golden" / "vectors.json"

UPSTREAM_URL = "https://github.com/styly-dev/STYLY-NetSync.git"
UPSTREAM_COMMIT = "72fc88409d36e146b03f2d90bd11ebd9998408a3"

# Pose flags (mirrors src/protocol/message_types.hpp)
STEALTH = 1 << 0
PHYSICAL = 1 << 1
HEAD = 1 << 2
RIGHT = 1 << 3
LEFT = 1 << 4
VIRTUALS = 1 << 5
MOVING_FLOOR = 1 << 6


# --- Newtonsoft-compatible JSON string-array encoding ------------------------
# Reproduces JsonConvert.SerializeObject(string[]) with default settings, which
# is how the Unity client builds MSG_RPC argumentsJson. Note this is NOT
# json.dumps(): json.dumps escapes non-ASCII by default and Newtonsoft does not.

_SHORT_ESCAPES = {
    '"': '\\"',
    "\\": "\\\\",
    "\b": "\\b",
    "\f": "\\f",
    "\n": "\\n",
    "\r": "\\r",
    "\t": "\\t",
}


def newtonsoft_json_string(value: str) -> str:
    out = ['"']
    for ch in value:
        if ch in _SHORT_ESCAPES:
            out.append(_SHORT_ESCAPES[ch])
        elif ord(ch) < 0x20:
            out.append("\\u%04x" % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def newtonsoft_json_string_array(values: list[str]) -> str:
    return "[" + ",".join(newtonsoft_json_string(v) for v in values) + "]"


# --- Case construction helpers ----------------------------------------------


def transform(px, py, pz, qx, qy, qz, qw):
    """A pose transform, in the JSON shape shared by the generator and the C++ test."""
    return {"p": [px, py, pz], "q": [qx, qy, qz, qw]}


def to_wire_transform(t):
    """Convert the JSON shape into the dict the upstream serializer expects."""
    return {
        "posX": t["p"][0],
        "posY": t["p"][1],
        "posZ": t["p"][2],
        "rotX": t["q"][0],
        "rotY": t["q"][1],
        "rotZ": t["q"][2],
        "rotW": t["q"][3],
    }


def body_to_wire(body):
    wire = {
        "poseSeq": body["pose_seq"],
        "flags": body["flags"],
        "xrOriginDeltaX": body["xr_origin_delta"][0],
        "xrOriginDeltaY": body["xr_origin_delta"][1],
        "xrOriginDeltaZ": body["xr_origin_delta"][2],
        "xrOriginDeltaYaw": body["xr_origin_delta_yaw"],
        "physical": to_wire_transform(body["physical"]),
        "head": to_wire_transform(body["head"]),
        "rightHand": to_wire_transform(body["right_hand"]),
        "leftHand": to_wire_transform(body["left_hand"]),
        "virtuals": [to_wire_transform(v) for v in body["virtuals"]],
    }
    return wire


def make_body(
    pose_seq=0,
    flags=0,
    xr_origin_delta=(0.0, 0.0, 0.0),
    xr_origin_delta_yaw=0.0,
    physical=None,
    head=None,
    right_hand=None,
    left_hand=None,
    virtuals=None,
):
    identity = transform(0, 0, 0, 0, 0, 0, 1)
    return {
        "pose_seq": pose_seq,
        "flags": flags,
        "xr_origin_delta": list(xr_origin_delta),
        "xr_origin_delta_yaw": xr_origin_delta_yaw,
        "physical": physical or identity,
        "head": head or identity,
        "right_hand": right_hand or identity,
        "left_hand": left_hand or identity,
        "virtuals": virtuals or [],
    }


def random_unit_quaternion(rng):
    """Uniformly distributed random rotation (Shoemake's method)."""
    u1, u2, u3 = rng.random(), rng.random(), rng.random()
    s1, s2 = math.sqrt(1.0 - u1), math.sqrt(u1)
    t1, t2 = 2.0 * math.pi * u2, 2.0 * math.pi * u3
    return [s1 * math.sin(t1), s1 * math.cos(t1), s2 * math.sin(t2), s2 * math.cos(t2)]


# --- Vector definitions ------------------------------------------------------


def build_cases(bs):
    """Build every golden case. `bs` is the upstream binary_serializer module."""
    cases = []
    rng = random.Random(20240816)

    def add(name, message, payload, data):
        cases.append(
            {
                "name": name,
                "message": message,
                "input": payload,
                "bytes": data.hex(),
            }
        )

    # --- MSG_CLIENT_HELLO ---------------------------------------------------
    for name, device_id, stealth in [
        ("hello_basic", "godot-device-0001", False),
        ("hello_stealth", "godot-device-0001", True),
        ("hello_empty_device", "", False),
        ("hello_unicode_device", "デバイス-éè-01", False),
        ("hello_long_device", "d" * 200, False),
    ]:
        add(
            name,
            "client_hello",
            {"device_id": device_id, "is_stealth": stealth},
            bs.serialize_client_hello(device_id, stealth),
        )

    # --- MSG_CLIENT_POSE ----------------------------------------------------
    pose_cases = []

    pose_cases.append(
        (
            "pose_stealth",
            "stealth-device",
            make_body(pose_seq=0, flags=STEALTH),
        )
    )

    pose_cases.append(
        (
            "pose_head_only",
            "dev-head",
            make_body(
                pose_seq=1,
                flags=HEAD,
                head=transform(1.25, 1.6, -3.75, 0.0, 0.3826834, 0.0, 0.9238795),
            ),
        )
    )

    pose_cases.append(
        (
            "pose_head_physical",
            "dev-hp",
            make_body(
                pose_seq=4242,
                flags=HEAD | PHYSICAL,
                xr_origin_delta=(2.5, 0.0, -1.25),
                xr_origin_delta_yaw=37.5,
                head=transform(-0.5, 1.7, 2.25, 0.1, 0.2, 0.3, 0.9273618),
            ),
        )
    )

    pose_cases.append(
        (
            "pose_full",
            "dev-full",
            make_body(
                pose_seq=65535,
                flags=HEAD | PHYSICAL | RIGHT | LEFT | VIRTUALS,
                xr_origin_delta=(-12.34, 0.5, 7.89),
                xr_origin_delta_yaw=-179.9,
                head=transform(3.14159, 1.75, -2.71828, 0.3, -0.4, 0.5, 0.7071068),
                right_hand=transform(3.4, 1.2, -2.5, 0.0, 0.7071068, 0.0, 0.7071068),
                left_hand=transform(2.9, 1.1, -2.9, 0.5, 0.5, 0.5, 0.5),
                virtuals=[
                    transform(3.0, 0.5, -3.0, 0.0, 0.0, 0.0, 1.0),
                    transform(3.2, 0.6, -2.8, 0.0, -0.258819, 0.0, 0.9659258),
                    transform(-1.0, 2.0, 0.0, 0.7071068, 0.0, 0.0, 0.7071068),
                ],
            ),
        )
    )

    pose_cases.append(
        (
            "pose_moving_floor_local",
            "dev-mf",
            make_body(
                pose_seq=99,
                flags=HEAD | PHYSICAL | MOVING_FLOOR,
                physical=transform(1.5, 0.0, -2.5, 0.0, 0.258819, 0.0, 0.9659258),
                head=transform(0.25, 1.6, 0.5, 0.0, 0.0, 0.0, 1.0),
            ),
        )
    )

    pose_cases.append(
        (
            "pose_hands_without_head",
            "dev-nohead",
            # RightValid/LeftValid must be dropped by the sender's sanitisation.
            make_body(
                pose_seq=7,
                flags=RIGHT | LEFT | VIRTUALS,
                right_hand=transform(1, 1, 1, 0, 0, 0, 1),
                virtuals=[transform(2, 2, 2, 0, 0, 0, 1)],
            ),
        )
    )

    pose_cases.append(
        (
            "pose_clamping_extremes",
            "dev-clamp",
            make_body(
                pose_seq=1,
                flags=HEAD | PHYSICAL | RIGHT,
                # Beyond the int24 (±83886.07 m) and int16 (±163.835 m at REL scale) ranges.
                xr_origin_delta=(1e6, -1e6, 400.0),
                xr_origin_delta_yaw=100000.0,
                head=transform(1e9, -1e9, 0.005, 0, 0, 0, 1),
                right_hand=transform(1e9 + 500.0, -1e9 - 500.0, 0.005, 0, 0, 0, 1),
            ),
        )
    )

    pose_cases.append(
        (
            "pose_rounding_boundaries",
            "dev-round",
            # Values that land exactly on .5 quantisation boundaries, where
            # half-to-even and half-away-from-zero disagree.
            make_body(
                pose_seq=2,
                flags=HEAD | PHYSICAL | RIGHT | LEFT,
                xr_origin_delta=(0.005, 0.015, -0.005),
                xr_origin_delta_yaw=0.05,
                head=transform(0.005, 0.015, -0.025, 0, 0, 0, 1),
                right_hand=transform(0.0075, 0.0125, -0.0025, 0, 0, 0, 1),
                left_hand=transform(-0.0075, -0.0125, 0.0025, 0, 0, 0, 1),
            ),
        )
    )

    pose_cases.append(
        (
            "pose_degenerate_quaternion",
            "dev-degen",
            make_body(
                pose_seq=3,
                flags=HEAD | RIGHT,
                head=transform(1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0),
                right_hand=transform(1.2, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0),
            ),
        )
    )

    pose_cases.append(
        (
            "pose_max_virtuals",
            "dev-many",
            make_body(
                pose_seq=11,
                flags=HEAD | VIRTUALS,
                head=transform(0, 1.6, 0, 0, 0, 0, 1),
                # 55 entries: the serializer must emit exactly 50.
                virtuals=[
                    transform(
                        0.1 * i,
                        0.05 * i,
                        -0.02 * i,
                        *random_unit_quaternion(rng),
                    )
                    for i in range(55)
                ],
            ),
        )
    )

    for i in range(6):
        pose_cases.append(
            (
                "pose_random_%d" % i,
                "dev-rand-%d" % i,
                make_body(
                    pose_seq=rng.randrange(0, 65536),
                    flags=HEAD | PHYSICAL | RIGHT | LEFT | VIRTUALS,
                    xr_origin_delta=(
                        rng.uniform(-50, 50),
                        rng.uniform(-5, 5),
                        rng.uniform(-50, 50),
                    ),
                    xr_origin_delta_yaw=rng.uniform(-180, 180),
                    head=transform(
                        rng.uniform(-100, 100),
                        rng.uniform(0, 3),
                        rng.uniform(-100, 100),
                        *random_unit_quaternion(rng),
                    ),
                    right_hand=transform(
                        rng.uniform(-100, 100),
                        rng.uniform(0, 3),
                        rng.uniform(-100, 100),
                        *random_unit_quaternion(rng),
                    ),
                    left_hand=transform(
                        rng.uniform(-100, 100),
                        rng.uniform(0, 3),
                        rng.uniform(-100, 100),
                        *random_unit_quaternion(rng),
                    ),
                    virtuals=[
                        transform(
                            rng.uniform(-100, 100),
                            rng.uniform(0, 3),
                            rng.uniform(-100, 100),
                            *random_unit_quaternion(rng),
                        )
                        for _ in range(rng.randrange(0, 4))
                    ],
                ),
            )
        )

    for name, device_id, body in pose_cases:
        wire = body_to_wire(body)
        wire["deviceId"] = device_id
        add(
            name,
            "client_pose",
            {"device_id": device_id, "body": body},
            bs.serialize_client_transform(wire),
        )

    # --- MSG_ROOM_POSE (server → client; verified by decoding in C++) --------
    room_clients = [
        (1, 1234.5, pose_cases[1][2]),
        (2, 1234.75, pose_cases[3][2]),
        (65535, 0.0, pose_cases[4][2]),
    ]
    room_payload = {
        "room_id": "default_room",
        "broadcast_time": 987654.125,
        "clients": [
            {"client_no": no, "pose_time": t, "body": body} for no, t, body in room_clients
        ],
    }
    room_wire = {
        "roomId": room_payload["room_id"],
        "broadcastTime": room_payload["broadcast_time"],
        "clients": [
            dict(body_to_wire(body), clientNo=no, poseTime=t) for no, t, body in room_clients
        ],
    }
    add("room_pose_three_clients", "room_pose", room_payload, bs.serialize_room_transform(room_wire))

    add(
        "room_pose_empty",
        "room_pose",
        {"room_id": "r", "broadcast_time": 0.0, "clients": []},
        bs.serialize_room_transform({"roomId": "r", "broadcastTime": 0.0, "clients": []}),
    )

    # --- MSG_RPC ------------------------------------------------------------
    rpc_cases = [
        ("rpc_broadcast_no_args", 1, "dev-a", [], "ChangeColor", []),
        ("rpc_broadcast_args", 7, "dev-b", [], "Spawn", ["cube", "1.5", "true"]),
        ("rpc_single_target", 3, "dev-c", [9], "Ping", ["hello"]),
        ("rpc_many_targets", 42, "dev-d", list(range(1, 21)), "Broadcast", ["x"]),
        (
            "rpc_escaped_args",
            5,
            "dev-e",
            [],
            "Log",
            ['quote " backslash \\ newline \n tab \t', "unicode é日本", "\x01\x1f"],
        ),
        ("rpc_empty_string_arg", 0, "", [], "F", [""]),
    ]
    for name, sender, device_id, targets, fn, args in rpc_cases:
        args_json = newtonsoft_json_string_array(args)
        add(
            name,
            "rpc",
            {
                "sender_client_no": sender,
                "device_id": device_id,
                "target_client_nos": targets,
                "function_name": fn,
                "arguments": args,
                "arguments_json": args_json,
            },
            bs.serialize_rpc_message(
                {
                    "senderClientNo": sender,
                    "deviceId": device_id,
                    "targetClientNos": targets,
                    "functionName": fn,
                    "argumentsJson": args_json,
                }
            ),
        )

    # --- MSG_DEVICE_ID_MAPPING ----------------------------------------------
    mappings = [(1, "dev-a", False), (2, "dev-b", True), (65535, "デバイス", False)]
    add(
        "device_id_mapping",
        "device_id_mapping",
        {
            "server_version": [0, 17, 4],
            "mappings": [
                {"client_no": n, "device_id": d, "is_stealth": s} for n, d, s in mappings
            ],
        },
        bs.serialize_device_id_mapping(mappings, (0, 17, 4)),
    )
    add(
        "device_id_mapping_empty",
        "device_id_mapping",
        {"server_version": [1, 2, 3], "mappings": []},
        bs.serialize_device_id_mapping([], (1, 2, 3)),
    )

    # --- Network variables ---------------------------------------------------
    nv_set_cases = [
        ("global_var_set_basic", 5, "dev-a", "score", "100"),
        ("global_var_set_empty_value", 1, "dev-b", "flag", ""),
        ("global_var_set_unicode", 2, "dev-c", "名前", "値é"),
        ("global_var_set_truncation", 3, "dev-d", "n" * 80, "v" * 1200),
    ]
    for name, sender, device_id, var_name, var_value in nv_set_cases:
        add(
            name,
            "global_var_set",
            {
                "sender_client_no": sender,
                "device_id": device_id,
                "variable_name": var_name,
                "variable_value": var_value,
            },
            bs.serialize_global_var_set(
                {
                    "senderClientNo": sender,
                    "deviceId": device_id,
                    "variableName": var_name,
                    "variableValue": var_value,
                }
            ),
        )

    for name, sender, device_id, target, var_name, var_value in [
        ("client_var_set_basic", 5, "dev-a", 5, "nickname", "godot"),
        ("client_var_set_other", 5, "dev-a", 9, "state", "ready"),
        ("client_var_set_truncation", 1, "dev-b", 2, "k" * 100, "w" * 2000),
    ]:
        add(
            name,
            "client_var_set",
            {
                "sender_client_no": sender,
                "device_id": device_id,
                "target_client_no": target,
                "variable_name": var_name,
                "variable_value": var_value,
            },
            bs.serialize_client_var_set(
                {
                    "senderClientNo": sender,
                    "deviceId": device_id,
                    "targetClientNo": target,
                    "variableName": var_name,
                    "variableValue": var_value,
                }
            ),
        )

    add(
        "client_var_clear",
        "client_var_clear",
        {"sender_client_no": 12, "device_id": "dev-clear"},
        bs.serialize_client_var_clear({"senderClientNo": 12, "deviceId": "dev-clear"}),
    )

    gv_sync = [
        {"name": "score", "value": "42", "lastWriterClientNo": 3},
        {"name": "phase", "value": "", "lastWriterClientNo": 0},
    ]
    add(
        "global_var_sync",
        "global_var_sync",
        {
            "variables": [
                {"name": v["name"], "value": v["value"], "last_writer_client_no": v["lastWriterClientNo"]}
                for v in gv_sync
            ]
        },
        bs.serialize_global_var_sync({"variables": gv_sync}),
    )

    cv_sync = {
        "1": [{"name": "nickname", "value": "alice", "lastWriterClientNo": 1}],
        "2": [
            {"name": "nickname", "value": "bob", "lastWriterClientNo": 2},
            {"name": "team", "value": "red", "lastWriterClientNo": 1},
        ],
    }
    add(
        "client_var_sync",
        "client_var_sync",
        {
            "clients": [
                {
                    "client_no": int(k),
                    "variables": [
                        {
                            "name": v["name"],
                            "value": v["value"],
                            "last_writer_client_no": v["lastWriterClientNo"],
                        }
                        for v in vs
                    ],
                }
                for k, vs in cv_sync.items()
            ]
        },
        bs.serialize_client_var_sync({"clientVariables": cv_sync}),
    )

    # --- Object sync ---------------------------------------------------------
    object_pose_cases = [
        ("object_pose_basic", "dev-o", 0x1234ABCD, 77, [1.5, 0.25, -3.0], [0, 0.7071068, 0, 0.7071068]),
        ("object_pose_zero", "dev-o", 1, 0, [0, 0, 0], [0, 0, 0, 1]),
        (
            "object_pose_max_id",
            "dev-o",
            0xFFFFFFFF,
            65535,
            [-99999.0, 99999.0, 0.005],
            [0.5, -0.5, 0.5, -0.5],
        ),
    ]
    for name, device_id, object_id, pose_seq, pos, rot in object_pose_cases:
        add(
            name,
            "object_pose",
            {
                "device_id": device_id,
                "object_id": object_id,
                "pose_seq": pose_seq,
                "position": pos,
                "rotation": rot,
            },
            bs.serialize_object_pose(
                {
                    "deviceId": device_id,
                    "objectId": object_id,
                    "poseSeq": pose_seq,
                    "posX": pos[0],
                    "posY": pos[1],
                    "posZ": pos[2],
                    "rotX": rot[0],
                    "rotY": rot[1],
                    "rotZ": rot[2],
                    "rotW": rot[3],
                }
            ),
        )

    room_objects = [
        {
            "objectId": 0x0000BEEF,
            "ownerClientNo": 4,
            "poseSeq": 12,
            "poseTime": 555.25,
            "posX": 1.0,
            "posY": 2.0,
            "posZ": 3.0,
            "rotX": 0.0,
            "rotY": 0.0,
            "rotZ": 0.0,
            "rotW": 1.0,
        },
        {
            "objectId": 0x0000CAFE,
            "ownerClientNo": 0,
            "poseSeq": 0,
            "poseTime": 0.0,
            "posX": -7.5,
            "posY": 0.0,
            "posZ": 12.25,
            "rotX": 0.2588190,
            "rotY": 0.0,
            "rotZ": 0.0,
            "rotW": 0.9659258,
        },
    ]
    add(
        "room_objects",
        "room_objects",
        {
            "broadcast_time": 4242.5,
            "objects": [
                {
                    "object_id": o["objectId"],
                    "owner_client_no": o["ownerClientNo"],
                    "pose_seq": o["poseSeq"],
                    "pose_time": o["poseTime"],
                    "position": [o["posX"], o["posY"], o["posZ"]],
                    "rotation": [o["rotX"], o["rotY"], o["rotZ"], o["rotW"]],
                }
                for o in room_objects
            ],
        },
        bs.serialize_room_objects("room", 4242.5, room_objects),
    )

    for name, device_id, op, object_id in [
        ("ownership_request_take", "dev-own", 2, 0x1234ABCD),
        ("ownership_request_release", "dev-own", 1, 0x1234ABCD),
    ]:
        # Upstream Python has no serializer for this client→server message, so
        # the expected bytes are asserted through its deserializer instead: the
        # C++ output is fed back and the fields must match.
        cases.append(
            {
                "name": name,
                "message": "object_ownership_request",
                "input": {
                    "device_id": device_id,
                    "operation_type": op,
                    "object_id": object_id,
                },
                "bytes": None,
                "decode_only": True,
            }
        )

    add(
        "ownership_changed",
        "ownership_changed",
        {"object_id": 0x1234ABCD, "new_owner_client_no": 7, "previous_owner_client_no": 3},
        bs.serialize_object_ownership_changed(0x1234ABCD, 7, 3),
    )
    add(
        "ownership_rejected",
        "ownership_rejected",
        {"object_id": 0x1234ABCD, "current_owner_client_no": 9, "reason_code": 1},
        bs.serialize_object_ownership_rejected(0x1234ABCD, 9, 1),
    )

    return cases


def build_quaternion_vectors(bs, count=2000):
    """Random quaternions with their upstream packed representation."""
    rng = random.Random(1337)
    vectors = []

    # Deterministic edge cases first.
    edges = [
        [0.0, 0.0, 0.0, 1.0],
        [1.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
        [-1.0, 0.0, 0.0, 0.0],
        [0.0, 0.0, 0.0, -1.0],
        [0.5, 0.5, 0.5, 0.5],
        [-0.5, -0.5, -0.5, -0.5],
        [0.7071067811865476, 0.7071067811865476, 0.0, 0.0],
        [0.70710677, -0.70710677, 0.0, 0.0],
        # Non-normalised input: the codec must normalise first.
        [2.0, 0.0, 0.0, 2.0],
        [1e-8, 0.0, 0.0, 1e-8],
        # Degenerate: below the normalise epsilon, must collapse to identity.
        [0.0, 0.0, 0.0, 0.0],
        [1e-7, 0.0, 0.0, 0.0],
        # Exact ties between components, where the "first maximum wins" rule bites.
        [0.5, 0.5, -0.5, -0.5],
        [0.6, 0.6, 0.0, 0.5291502622129181],
    ]
    for q in edges:
        vectors.append(
            {"q": q, "packed": bs._compress_quaternion_smallest_three(*q)}
        )

    for _ in range(count):
        q = random_unit_quaternion(rng)
        vectors.append({"q": q, "packed": bs._compress_quaternion_smallest_three(*q)})

    return vectors


def build_quantization_vectors(bs):
    """Quantisation vectors targeting rounding and clamping behaviour."""
    values = [
        0.0,
        -0.0,
        0.005,
        -0.005,
        0.015,
        -0.015,
        0.0025,
        0.0075,
        0.0125,
        0.0175,
        1.0,
        -1.0,
        0.014999999,
        0.015000001,
        163.835,
        -163.84,
        1e6,
        -1e6,
        83886.07,
        -83886.08,
        1e9,
        -1e9,
        0.1,
        1.0 / 3.0,
        -1.0 / 3.0,
        12345.678,
    ]
    out = []
    for scale_name, scale in [
        ("abs", bs.ABS_POS_SCALE),
        ("rel", bs.REL_POS_SCALE),
        ("loco", bs.LOCO_POS_SCALE),
        ("yaw", bs.PHYSICAL_YAW_SCALE),
    ]:
        for value in values:
            out.append(
                {
                    "scale": scale_name,
                    "value": value,
                    "i16": bs._quantize_signed(value, scale),
                    "i24": bs._quantize_signed_int24(value, scale),
                }
            )
    return out


# --- Upstream acquisition ----------------------------------------------------


def ensure_upstream(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit).resolve()
        if not (path / "STYLY-NetSync-Server" / "src" / "styly_netsync").is_dir():
            raise SystemExit(f"{path} does not look like a STYLY-NetSync checkout")
        return path

    cache_root = Path(
        os.environ.get("STYLY_NETSYNC_UPSTREAM_CACHE", Path(tempfile.gettempdir()) / "styly-netsync-upstream")
    )
    cache_root.parent.mkdir(parents=True, exist_ok=True)
    if not (cache_root / ".git").is_dir():
        print(f"Cloning upstream into {cache_root} ...", file=sys.stderr)
        subprocess.run(
            ["git", "clone", "--filter=blob:none", UPSTREAM_URL, str(cache_root)],
            check=True,
        )
    subprocess.run(["git", "-C", str(cache_root), "fetch", "--all", "--quiet"], check=False)
    subprocess.run(["git", "-C", str(cache_root), "checkout", "--quiet", UPSTREAM_COMMIT], check=True)
    return cache_root


def load_upstream_serializer(upstream: Path):
    """Import upstream's binary_serializer.py directly, by path.

    Importing `styly_netsync` as a package would pull in server.py and its
    third-party dependencies (loguru, pyzmq, fastapi). The serializer itself
    depends only on the standard library, so loading the single module keeps
    golden generation dependency-free.
    """
    module_path = upstream / "STYLY-NetSync-Server" / "src" / "styly_netsync" / "binary_serializer.py"
    if not module_path.is_file():
        raise SystemExit(f"upstream serializer not found at {module_path}")
    spec = importlib.util.spec_from_file_location("upstream_binary_serializer", module_path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {module_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", help="Path to an existing STYLY-NetSync checkout")
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT))
    parser.add_argument(
        "--check",
        action="store_true",
        help="Regenerate in memory and fail if the committed file differs",
    )
    args = parser.parse_args()

    upstream = ensure_upstream(args.upstream)
    bs = load_upstream_serializer(upstream)

    if bs.PROTOCOL_VERSION != 8:
        raise SystemExit(f"upstream protocol version is {bs.PROTOCOL_VERSION}, expected 8")

    document = {
        "_comment": (
            "Generated by tests/tools/generate_golden_vectors.py from the upstream "
            "STYLY-NetSync Python serializer. Do not edit by hand."
        ),
        "upstream_repository": UPSTREAM_URL,
        "upstream_commit": UPSTREAM_COMMIT,
        "protocol_version": bs.PROTOCOL_VERSION,
        "quaternion_vectors": build_quaternion_vectors(bs),
        "quantization_vectors": build_quantization_vectors(bs),
        "cases": build_cases(bs),
    }

    text = json.dumps(document, indent=1, ensure_ascii=False, sort_keys=False) + "\n"
    output = Path(args.output)

    if args.check:
        if not output.exists():
            print(f"{output} does not exist", file=sys.stderr)
            return 1
        if output.read_text(encoding="utf-8") != text:
            print(
                f"{output} is stale — rerun tests/tools/generate_golden_vectors.py",
                file=sys.stderr,
            )
            return 1
        print(f"{output} is up to date ({len(document['cases'])} cases)")
        return 0

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")
    print(
        f"Wrote {output}: {len(document['cases'])} message cases, "
        f"{len(document['quaternion_vectors'])} quaternion vectors, "
        f"{len(document['quantization_vectors'])} quantisation vectors"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
