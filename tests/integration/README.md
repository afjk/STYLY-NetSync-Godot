# Integration tests

Two harnesses, both of which start a **real** `styly_netsync.NetSyncServer` on
ephemeral ports and drive clients against it. They are not registered with
CTest, because they need the upstream server package (and, for one of them, a
Godot binary) that the native suites deliberately do not.

| Harness | What it drives | What it proves |
|---|---|---|
| `test_against_server.py` | the native client, via `netsync_probe` | the protocol works against the real server: handshake, readiness, pose relay, RPC, network variables, object ownership, stealth, reconnect identity |
| `test_godot_client.py` | two headless Godot clients, via the addon | the whole stack works, including GDScript, the GDExtension and the coordinate boundary |

## Running them

```bash
# once: the upstream server, read-only
pip install -e /path/to/STYLY-NetSync/STYLY-NetSync-Server

# native client
cmake -S . -B build && cmake --build build --target netsync_probe
python3 tests/integration/test_against_server.py --probe build/netsync_probe

# Godot addon (needs the extension built: scons target=template_debug)
GODOT=/path/to/godot python3 tests/integration/test_godot_client.py
```

---

## `netsync_probe` command reference

`netsync_probe` is a headless NetSync client driven over stdin. Every event and
command result is written to stdout as one JSON object per line, so a harness
can assert on it. It is useful on its own for poking at a server by hand:

```bash
build/netsync_probe --server 127.0.0.1 --room my_room --device my-device
```

### Command-line options

| Option | Default | Meaning |
|---|---|---|
| `--server HOST` | — | Server host, with or without `tcp://`. Omit with `--discover`. |
| `--control PORT` | 5555 | Control ROUTER port |
| `--transform PORT` | 5557 | Transform ROUTER port |
| `--sub PORT` | 5556 | PUB port |
| `--discovery-port PORT` | 9999 | LAN discovery port |
| `--room ID` | `probe_room` | Room to join |
| `--device ID` | random UUID | Device id (the server's identity key) |
| `--stealth` | off | Join without an avatar |
| `--discover` | off | Find a server on the LAN instead of using `--server` |
| `--send-rate HZ` | 20 | Pose sends per second |

### Commands

Poses are given in **wire (Unity-handed) coordinates** — the probe sits below
the Godot layer, so nothing is converted for it.

| Command | Effect |
|---|---|
| `connect` | Start connecting |
| `disconnect` | Tear the connection down |
| `wait_ready [seconds]` | Block until ready; replies `ready` or `timeout` |
| `sleep SECONDS` | Keep polling for a while |
| `state` | Dump the full connection state |
| `pose X Y Z [YAW]` | Publish a head pose |
| `hands X Y Z RIGHT_DX LEFT_DX YAW` | Publish head plus both hands |
| `rpc NAME [ARG…]` | Broadcast an RPC |
| `rpc_to CLIENT_NO NAME [ARG…]` | Send an RPC to one client |
| `set_global NAME VALUE` | Set a global network variable |
| `get_global NAME` | Read one back |
| `set_client_var NAME VALUE` | Set one of this client's variables |
| `set_client_var_for CLIENT_NO NAME VALUE` | Set another client's variable |
| `get_client_var CLIENT_NO NAME` | Read a client variable |
| `clear_client_vars` | Clear every variable this client owns |
| `register_object ID` | Track a synchronised object |
| `request_ownership ID` / `release_ownership ID` | Ownership control |
| `object_pose ID X Y Z [YAW]` | Publish an owned object's pose |
| `get_object ID` | Read an object's owner and pose |
| `remote_pose CLIENT_NO` | Read a peer's latest pose |
| `remote_clients` / `known_clients` | List peers (posing / all, including stealth) |
| `stats` | Transport counters |
| `quit` | Exit |

Arguments are whitespace-separated, so values containing spaces are not
expressible from this interface — the C++ and Godot suites cover those.

### Output

Two shapes, one JSON object per line:

* **Command results** carry `"result"` — `started`, `connect`, `ready`,
  `timeout`, `state`, `get_object`, `remote_pose`, and so on.
* **Events** carry `"event"` — `ready`, `connection_state_changed`,
  `avatar_connected`, `avatar_disconnected`, `rpc_received`,
  `global_variable_changed`, `client_variable_changed`,
  `object_ownership_changed`, `object_ownership_rejected`, `server_version`,
  `log`.

Events use the core's generic field names, so their meaning depends on the
event: for `object_ownership_changed`, `value_a` is the new owner and `value_b`
the previous one; for `object_ownership_rejected`, `value_a` is the current
owner and `value_b` the reason code; for `server_version`, `value_a`/`value_b`/
`value_c` are major/minor/patch.

### A short session

```
$ build/netsync_probe --server 127.0.0.1 --control 5555 --transform 5557 --sub 5556 --room demo
{"result":"started","device_id":"…","room_id":"demo"}
connect
{"result":"connect","ok":true}
wait_ready 10
{"event":"connection_state_changed","value_a":2,"name":"connected",…}
{"event":"server_version","value_a":0,"value_b":17,"value_c":4,…}
{"event":"client_no_assigned","client_no":1,…}
{"event":"ready",…}
{"result":"ready","client_no":1}
pose 1.5 1.6 -2.5 45
{"result":"pose"}
rpc Hello world
{"result":"rpc"}
{"event":"rpc_received","client_no":1,"name":"Hello","args":["world"],…}
quit
{"result":"exited"}
```

Note the RPC coming back: the server echoes a broadcast to its sender, which is
upstream behaviour and not a quirk of this client.
