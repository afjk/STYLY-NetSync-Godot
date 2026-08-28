#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""End-to-end test of the Godot addon against a real STYLY NetSync server.

Starts an actual `styly_netsync.NetSyncServer`, then runs two headless Godot
instances of `godot_client_test.gd` in the same room: one publishes an avatar
pose, a shared object, a global variable and an RPC; the other reads them back
and asserts they arrive at the right *Godot-space* position and rotation.

This is the check the C++ tests cannot make on their own: it goes through
GDScript, the GDExtension bridge, the coordinate boundary, ZeroMQ and the real
server, in both directions.

Requires a Godot 4 binary (`--godot`, `$GODOT`, or `godot` on PATH) with the
extension already built (`scons target=template_debug`).

Run:
    python3 tests/integration/test_godot_client.py
    python3 tests/integration/test_godot_client.py --godot /path/to/godot
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CLIENT_SCRIPT = "res://tests/integration/godot_client_test.gd"
TIMEOUT_SECONDS = 45.0


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def find_godot(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    from_env = os.environ.get("GODOT")
    if from_env:
        return from_env
    return shutil.which("godot") or shutil.which("godot4")


def start_server():
    try:
        from loguru import logger

        logger.remove()
        logger.add(sys.stderr, level="WARNING")
    except ImportError:
        pass

    from styly_netsync.server import NetSyncServer

    ports = (free_port(), free_port(), free_port())
    server = NetSyncServer(
        control_port=ports[0],
        transform_port=ports[1],
        pub_port=ports[2],
        enable_server_discovery=False,
    )
    thread = threading.Thread(target=server.start, daemon=True)
    thread.start()

    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        if getattr(server, "running", False):
            time.sleep(0.3)
            return server, ports
        time.sleep(0.05)
    raise AssertionError("server did not start in time")


class GodotClient:
    def __init__(self, godot: str, role: str, room: str, ports: tuple[int, int, int]):
        self.role = role
        self.lines: list[dict] = []
        self.raw: list[str] = []
        self._lock = threading.Lock()
        command = [
            godot,
            "--headless",
            "--path",
            str(REPO_ROOT),
            "--script",
            CLIENT_SCRIPT,
            "--",
            f"--role={role}",
            f"--room={room}",
            "--server=127.0.0.1",
            f"--control={ports[0]}",
            f"--transform={ports[1]}",
            f"--sub={ports[2]}",
            f"--device=godot-{role}-{uuid.uuid4()}",
            f"--timeout={TIMEOUT_SECONDS}",
        ]
        self.process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def _read(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            line = line.rstrip("\n")
            with self._lock:
                self.raw.append(line)
                # The client prefixes its structured output with "@@".
                if line.startswith("@@"):
                    try:
                        self.lines.append(json.loads(line[2:]))
                    except json.JSONDecodeError:
                        pass

    def wait(self, timeout: float) -> int:
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
            return -1

    def messages(self) -> list[dict]:
        with self._lock:
            return list(self.lines)

    def output(self) -> str:
        with self._lock:
            return "\n".join(self.raw)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", help="Path to a Godot 4 binary")
    arguments = parser.parse_args()

    godot = find_godot(arguments.godot)
    if godot is None:
        print(
            "No Godot binary found. Pass --godot <path>, set $GODOT, or put `godot` on PATH.",
            file=sys.stderr,
        )
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

    if not any(REPO_ROOT.joinpath("addons/styly_netsync/bin").glob("libstyly_netsync.*")):
        print(
            "The native extension has not been built. Run `scons target=template_debug` "
            "from the repository root first.",
            file=sys.stderr,
        )
        return 2

    print(f"godot:  {godot}")
    server, ports = start_server()
    print(f"server: control={ports[0]} transform={ports[1]} pub={ports[2]}")

    room = "godot_interop_" + uuid.uuid4().hex[:8]
    failures: list[str] = []
    total_checks = 0

    try:
        sender = GodotClient(godot, "sender", room, ports)
        # Let the sender establish itself first so the receiver reliably sees a
        # join rather than racing the server's first room broadcast.
        time.sleep(2.0)
        receiver = GodotClient(godot, "receiver", room, ports)

        receiver_code = receiver.wait(TIMEOUT_SECONDS + 20.0)
        sender_code = sender.wait(TIMEOUT_SECONDS + 20.0)

        for client, code in ((sender, sender_code), (receiver, receiver_code)):
            summary = next(
                (m for m in client.messages() if m.get("result") == "summary"), None
            )
            if summary is None:
                failures.append(f"{client.role}: produced no summary (exit {code})")
                print(f"\n--- {client.role} output ---\n{client.output()}\n")
                continue

            total_checks += int(summary.get("checks", 0))
            for message in summary.get("messages", []):
                failures.append(f"{client.role}: {message}")

            print(
                f"   {client.role}: {summary['checks']} checks, "
                f"{summary['failures']} failures (exit {code})"
            )
            for message in client.messages():
                if message.get("result") in ("pose", "object"):
                    print(f"      {message}")
                if message.get("result") == "error":
                    failures.append(f"{client.role}: {message.get('message')}")
    finally:
        try:
            server.stop()
        except Exception:
            pass

    print()
    if failures:
        print(f"FAIL Godot client against real server: {len(failures)} problems")
        for failure in failures:
            print("  - " + failure)
        return 1
    print(f"PASS Godot client against real server ({total_checks} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
