#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Integration tests against a real STYLY NetSync server.

Starts an actual `styly_netsync.NetSyncServer` in-process and drives one or two
instances of the `netsync_probe` binary against it over ZeroMQ. This exercises
the whole stack — handshake, readiness, pose upload and relay, RPC, network
variables, object ownership — against the same server the Unity client talks to.

Requires:
  * the probe binary (cmake --build; see docs/BUILD.md)
  * the upstream server package: pip install -e <upstream>/STYLY-NetSync-Server

Run:
    python3 tests/integration/test_against_server.py
    python3 tests/integration/test_against_server.py --probe build/netsync_probe
"""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

DEFAULT_PROBE_PATHS = [
    REPO_ROOT / "build" / "netsync_probe",
    REPO_ROOT / "build" / "tests" / "netsync_probe",
    REPO_ROOT / "netsync_probe",
]


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class Probe:
    """One `netsync_probe` process, with line-oriented JSON in and out."""

    def __init__(self, binary: Path, name: str, **kwargs: object):
        self.name = name
        self.device_id = str(kwargs.pop("device_id", uuid.uuid4()))
        command = [
            str(binary),
            "--server",
            str(kwargs.pop("server", "127.0.0.1")),
            "--control",
            str(kwargs.pop("control", 0)),
            "--transform",
            str(kwargs.pop("transform", 0)),
            "--sub",
            str(kwargs.pop("sub", 0)),
            "--room",
            str(kwargs.pop("room", "probe_room")),
            "--device",
            self.device_id,
        ]
        if kwargs.pop("stealth", False):
            command.append("--stealth")
        for key, value in kwargs.items():
            command += ["--" + str(key).replace("_", "-"), str(value)]

        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.messages: list[dict] = []
        self._lock = threading.Lock()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                message = {"raw": line}
            with self._lock:
                self.messages.append(message)

    def send(self, command: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def snapshot(self) -> list[dict]:
        with self._lock:
            return list(self.messages)

    def wait_for(self, predicate, timeout: float = 15.0, what: str = "condition") -> dict:
        deadline = time.monotonic() + timeout
        seen = 0
        while time.monotonic() < deadline:
            messages = self.snapshot()
            for message in messages[seen:]:
                if predicate(message):
                    return message
            seen = len(messages)
            if self.process.poll() is not None:
                raise AssertionError(
                    f"{self.name}: process exited ({self.process.returncode}) waiting for {what}"
                )
            time.sleep(0.02)
        raise AssertionError(f"{self.name}: timed out waiting for {what}")

    def wait_result(self, result: str, timeout: float = 15.0) -> dict:
        return self.wait_for(
            lambda m: m.get("result") == result, timeout, f"result '{result}'"
        )

    def wait_event(self, event: str, timeout: float = 15.0, **fields) -> dict:
        def matches(message: dict) -> bool:
            if message.get("event") != event:
                return False
            return all(message.get(key) == value for key, value in fields.items())

        return self.wait_for(matches, timeout, f"event '{event}' {fields}")

    def query(self, command: str, result: str, timeout: float = 15.0) -> dict:
        """Send a command and return the reply it produces."""
        before = len(self.snapshot())
        self.send(command)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            messages = self.snapshot()
            for message in messages[before:]:
                if message.get("result") == result:
                    return message
            time.sleep(0.02)
        raise AssertionError(f"{self.name}: no '{result}' reply to '{command}'")

    def close(self) -> None:
        try:
            if self.process.poll() is None:
                self.send("quit")
                self.process.wait(timeout=5)
        except Exception:
            pass
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=5)


class ServerFixture:
    """A real NetSyncServer on ephemeral ports."""

    def __init__(self):
        # The server configures loguru itself at INFO/DEBUG, which buries the
        # test output. Re-point the sink before starting it.
        try:
            from loguru import logger

            logger.remove()
            logger.add(sys.stderr, level="WARNING")
        except ImportError:
            pass

        from styly_netsync.server import NetSyncServer

        self.control_port = free_port()
        self.transform_port = free_port()
        self.pub_port = free_port()

        self.server = NetSyncServer(
            control_port=self.control_port,
            transform_port=self.transform_port,
            pub_port=self.pub_port,
            # Discovery binds a fixed well-known port and would clash between
            # concurrent runs; it is covered separately by the unit tests.
            enable_server_discovery=False,
        )
        self._thread = threading.Thread(target=self.server.start, daemon=True)
        self._thread.start()
        self._wait_until_listening()

    def _wait_until_listening(self, timeout: float = 20.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if getattr(self.server, "running", False):
                # The ROUTER sockets are bound by the time `running` is set;
                # give the publisher thread a moment to come up too.
                time.sleep(0.3)
                return
            time.sleep(0.05)
        raise AssertionError("server did not start in time")

    def stop(self) -> None:
        try:
            self.server.stop()
        except Exception:
            pass


# --- Test bodies ---------------------------------------------------------------

FAILURES: list[str] = []
CHECKS = 0


def check(condition: bool, message: str) -> None:
    global CHECKS
    CHECKS += 1
    if not condition:
        FAILURES.append(message)
        print("FAIL: " + message)


def make_probe(binary: Path, server: ServerFixture, name: str, room: str, **kwargs) -> Probe:
    return Probe(
        binary,
        name,
        server="127.0.0.1",
        control=server.control_port,
        transform=server.transform_port,
        sub=server.pub_port,
        room=room,
        **kwargs,
    )


def test_handshake_and_ready(binary: Path, server: ServerFixture) -> None:
    print("\n== handshake, client number and readiness ==")
    probe = make_probe(binary, server, "solo", "room_ready")
    try:
        probe.wait_result("started")
        probe.send("connect")
        check(probe.wait_result("connect")["ok"], "connect was accepted")

        probe.send("wait_ready 20")
        ready = probe.wait_result("ready", timeout=25)
        check(ready["client_no"] > 0, f"server assigned a client number ({ready['client_no']})")

        state = probe.query("state", "state")
        check(state["connected"], "socket connection is up")
        check(state["handshake"], "handshake completed")
        check(state["nv_sync"], "network variables synced")
        check(state["ready"], "client reports ready")
        check(state["state"] == "ready", f"state is 'ready' (got {state['state']})")

        version = probe.wait_event("server_version", timeout=10)
        check(
            (version["value_a"], version["value_b"]) != (0, 0)
            or version["value_c"] != 0,
            "server reported a version",
        )
        print(
            f"   server version {version['value_a']}.{version['value_b']}.{version['value_c']}, "
            f"client number {ready['client_no']}"
        )
    finally:
        probe.close()


def test_two_clients_see_each_other(binary: Path, server: ServerFixture) -> None:
    print("\n== presence, pose relay and RPC between two clients ==")
    room = "room_pair"
    alice = make_probe(binary, server, "alice", room)
    bob = make_probe(binary, server, "bob", room)
    try:
        for probe in (alice, bob):
            probe.wait_result("started")
            probe.send("connect")
            probe.wait_result("connect")
            probe.send("wait_ready 20")

        alice_ready = alice.wait_result("ready", timeout=25)
        bob_ready = bob.wait_result("ready", timeout=25)
        alice_no = alice_ready["client_no"]
        bob_no = bob_ready["client_no"]
        check(alice_no != bob_no, "the two clients got different client numbers")

        # Poses: each client publishes a distinct head position.
        alice.send("pose 1.5 1.6 -2.5 45")
        bob.send("pose -3.0 1.7 4.25 -90")

        alice.wait_event("avatar_connected", timeout=20, client_no=bob_no)
        bob.wait_event("avatar_connected", timeout=20, client_no=alice_no)
        print(f"   alice is client {alice_no}, bob is client {bob_no}; both saw each other")

        # Give the server a broadcast cycle, then read the relayed pose back.
        time.sleep(1.0)
        seen = alice.query(f"remote_pose {bob_no}", "remote_pose")
        check(seen["found"], "alice has a pose for bob")
        if seen["found"]:
            # 0.01 m quantisation, so half a step of slack.
            check(abs(seen["x"] - (-3.0)) <= 0.01, f"bob's x relayed correctly ({seen['x']})")
            check(abs(seen["y"] - 1.7) <= 0.01, f"bob's y relayed correctly ({seen['y']})")
            check(abs(seen["z"] - 4.25) <= 0.01, f"bob's z relayed correctly ({seen['z']})")
            check(abs(seen["yaw"] - (-90.0)) <= 0.5, f"bob's yaw relayed correctly ({seen['yaw']})")

        # RPC: broadcast reaches both, targeted reaches only the target.
        alice.send("rpc Greet hello world")
        received = bob.wait_event("rpc_received", timeout=15, name="Greet")
        check(received["client_no"] == alice_no, "RPC carried alice's client number")
        check(received["args"] == ["hello", "world"], f"RPC arguments survived ({received['args']})")
        # The server echoes broadcasts back to the sender, as it does for Unity.
        alice.wait_event("rpc_received", timeout=15, name="Greet")

        bob.send(f"rpc_to {alice_no} Direct secret")
        direct = alice.wait_event("rpc_received", timeout=15, name="Direct")
        check(direct["args"] == ["secret"], "targeted RPC arguments survived")
        check(direct["client_no"] == bob_no, "targeted RPC carried bob's client number")

        time.sleep(0.5)
        bob_saw_direct = [
            m
            for m in bob.snapshot()
            if m.get("event") == "rpc_received" and m.get("name") == "Direct"
        ]
        check(not bob_saw_direct, "a targeted RPC did not reach the non-target")
    finally:
        bob.close()
        alice.close()


def test_network_variables(binary: Path, server: ServerFixture) -> None:
    print("\n== global and client network variables ==")
    room = "room_variables"
    alice = make_probe(binary, server, "alice", room)
    bob = make_probe(binary, server, "bob", room)
    try:
        for probe in (alice, bob):
            probe.wait_result("started")
            probe.send("connect")
            probe.wait_result("connect")
            probe.send("wait_ready 20")
        alice_no = alice.wait_result("ready", timeout=25)["client_no"]
        bob.wait_result("ready", timeout=25)

        # Global variable set by alice, observed by bob.
        alice.send("set_global phase combat")
        bob.wait_event("global_variable_changed", timeout=15, name="phase")
        value = bob.query("get_global phase", "get_global")
        check(value["value"] == "combat", f"bob sees the global value ({value['value']})")

        # Alice sees it too, through the server's authoritative sync.
        alice_value = alice.query("get_global phase", "get_global")
        check(alice_value["value"] == "combat", "alice sees her own global value")

        # Client variable set by alice on herself, observed by bob.
        alice.send("set_client_var nickname aria")
        bob.wait_event("client_variable_changed", timeout=15, name="nickname")
        seen = bob.query(f"get_client_var {alice_no} nickname", "get_client_var")
        check(seen["value"] == "aria", f"bob sees alice's client variable ({seen['value']})")

        # Clearing removes it everywhere.
        alice.send("clear_client_vars")
        bob.wait_for(
            lambda m: (
                m.get("event") == "client_variable_changed"
                and m.get("name") == "nickname"
                and m.get("removed") is True
            ),
            timeout=15,
            what="nickname removal",
        )
        cleared = bob.query(f"get_client_var {alice_no} nickname", "get_client_var")
        check(cleared["value"] == "", "the cleared client variable is gone for bob")
    finally:
        bob.close()
        alice.close()


def test_object_sync_and_ownership(binary: Path, server: ServerFixture) -> None:
    print("\n== object ownership and pose sync ==")
    room = "room_objects"
    object_id = 0x1234ABCD
    alice = make_probe(binary, server, "alice", room)
    bob = make_probe(binary, server, "bob", room)
    try:
        for probe in (alice, bob):
            probe.wait_result("started")
            probe.send("connect")
            probe.wait_result("connect")
            probe.send(f"register_object {object_id}")
            probe.wait_result("register_object")
            probe.send("wait_ready 20")
        alice_no = alice.wait_result("ready", timeout=25)["client_no"]
        bob_no = bob.wait_result("ready", timeout=25)["client_no"]

        # Alice claims the object; both clients learn about it.
        alice.send(f"request_ownership {object_id}")
        alice.wait_event("object_ownership_changed", timeout=15, value_a=alice_no)
        bob.wait_event("object_ownership_changed", timeout=15, value_a=alice_no)
        state = bob.query(f"get_object {object_id}", "get_object")
        check(
            state["owner_client_no"] == alice_no,
            f"bob sees alice as the owner ({state['owner_client_no']})",
        )

        # Alice drives it; bob receives the pose through the room broadcast.
        alice.send(f"object_pose {object_id} 4.5 1.25 -6.75")
        deadline = time.monotonic() + 15.0
        received = None
        while time.monotonic() < deadline:
            candidate = bob.query(f"get_object {object_id}", "get_object")
            if candidate["has_pose"] and abs(candidate["x"] - 4.5) <= 0.01:
                received = candidate
                break
            time.sleep(0.2)
        check(received is not None, "bob received alice's object pose")
        if received is not None:
            check(abs(received["y"] - 1.25) <= 0.01, f"object y relayed ({received['y']})")
            check(abs(received["z"] - (-6.75)) <= 0.01, f"object z relayed ({received['z']})")
            check(received["pose_seq"] > 0, "object pose sequence advanced")

        # Bob takes over; the server grants requests unconditionally.
        bob.send(f"request_ownership {object_id}")
        alice.wait_event("object_ownership_changed", timeout=15, value_a=bob_no)
        after = alice.query(f"get_object {object_id}", "get_object")
        check(after["owner_client_no"] == bob_no, "ownership moved to bob")

        # Alice is no longer the owner, so releasing must be refused.
        alice.send(f"release_ownership {object_id}")
        rejected = alice.wait_event("object_ownership_rejected", timeout=15)
        check(rejected["value_b"] == 1, f"rejection reason is 'not owner' ({rejected['value_b']})")
        check(
            rejected["value_a"] == bob_no,
            f"rejection names the current owner ({rejected['value_a']})",
        )

        # Bob releases legitimately.
        bob.send(f"release_ownership {object_id}")
        bob.wait_event("object_ownership_changed", timeout=15, value_a=0)
    finally:
        bob.close()
        alice.close()


def test_stealth_client(binary: Path, server: ServerFixture) -> None:
    print("\n== stealth client ==")
    room = "room_stealth"
    ghost = make_probe(binary, server, "ghost", room, stealth=True)
    watcher = make_probe(binary, server, "watcher", room)
    try:
        for probe in (ghost, watcher):
            probe.wait_result("started")
            probe.send("connect")
            probe.wait_result("connect")
            probe.send("wait_ready 20")
        ghost_no = ghost.wait_result("ready", timeout=25)["client_no"]
        watcher.wait_result("ready", timeout=25)

        # A stealth client still joins the room and can use the control lane...
        ghost.send("set_global stealth_says hello")
        watcher.wait_event("global_variable_changed", timeout=15, name="stealth_says")

        # ...and is listed in the mapping, flagged as stealth.
        time.sleep(1.0)
        known = watcher.query("known_clients", "known_clients")
        check(
            str(ghost_no) in known["clients"],
            f"the stealth client appears in the mapping ({known['clients']})",
        )

        # ...but publishes no avatar pose.
        remote = watcher.query("remote_clients", "remote_clients")
        check(
            str(ghost_no) not in remote["clients"],
            f"the stealth client publishes no pose ({remote['clients']})",
        )
    finally:
        watcher.close()
        ghost.close()


def test_reconnect_keeps_identity(binary: Path, server: ServerFixture) -> None:
    print("\n== reconnect reuses the same client number ==")
    room = "room_reconnect"
    device_id = str(uuid.uuid4())
    first = make_probe(binary, server, "first", room, device_id=device_id)
    try:
        first.wait_result("started")
        first.send("connect")
        first.wait_result("connect")
        first.send("wait_ready 20")
        original = first.wait_result("ready", timeout=25)["client_no"]
    finally:
        first.close()

    time.sleep(1.0)
    second = make_probe(binary, server, "second", room, device_id=device_id)
    try:
        second.wait_result("started")
        second.send("connect")
        second.wait_result("connect")
        second.send("wait_ready 20")
        again = second.wait_result("ready", timeout=25)["client_no"]
        check(
            again == original,
            f"the same device id kept client number {original} (got {again})",
        )
    finally:
        second.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", help="Path to the netsync_probe binary")
    arguments = parser.parse_args()

    if arguments.probe:
        binary = Path(arguments.probe).resolve()
    else:
        binary = next((p for p in DEFAULT_PROBE_PATHS if p.is_file()), None)
        if binary is None:
            print(
                "netsync_probe not found. Build it first:\n"
                "    cmake -S . -B build && cmake --build build\n"
                "or pass --probe <path>.",
                file=sys.stderr,
            )
            return 2
    if not binary.is_file():
        print(f"no probe binary at {binary}", file=sys.stderr)
        return 2

    try:
        import styly_netsync  # noqa: F401
    except ImportError:
        print(
            "The upstream server package is not installed. Install it read-only with:\n"
            "    pip install -e <STYLY-NetSync checkout>/STYLY-NetSync-Server",
            file=sys.stderr,
        )
        return 2

    print(f"probe:  {binary}")
    server = ServerFixture()
    print(
        f"server: control={server.control_port} transform={server.transform_port} "
        f"pub={server.pub_port}"
    )
    try:
        test_handshake_and_ready(binary, server)
        test_two_clients_see_each_other(binary, server)
        test_network_variables(binary, server)
        test_object_sync_and_ownership(binary, server)
        test_stealth_client(binary, server)
        test_reconnect_keeps_identity(binary, server)
    finally:
        server.stop()

    print()
    if FAILURES:
        print(f"FAIL integration against real server: {len(FAILURES)} of {CHECKS} checks failed")
        for failure in FAILURES:
            print("  - " + failure)
        return 1
    print(f"PASS integration against real server ({CHECKS} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
