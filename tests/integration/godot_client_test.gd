# SPDX-License-Identifier: Apache-2.0
extends SceneTree

## Headless Godot client used by tests/integration/test_godot_client.py.
##
## Two instances of this script join the same room on a real STYLY NetSync
## server. One publishes ("sender"), the other observes ("receiver"). The
## receiver asserts that the pose it reads back — after a full round trip
## through the wire's Unity-handed coordinates and the server's quantisation —
## lands at the [b]Godot[/b] position and rotation the sender set.
##
## That is the check that a Unity peer would see the same avatar in the same
## place: if the boundary conversion were wrong in either direction, the
## round trip would still be self-consistent only if the two errors cancelled,
## which the asymmetric test pose below rules out.
##
## Output is one JSON object per line, so the Python driver can assert on it.

const ROLE_SENDER := "sender"
const ROLE_RECEIVER := "receiver"

# An asymmetric pose: every axis differs in sign and magnitude, and the rotation
# is not axis-aligned, so a swapped axis or a flipped rotation sense cannot pass.
const TEST_POSITION := Vector3(1.25, 1.6, -3.5)
const TEST_YAW_DEGREES := 37.0
const TEST_RIGHT_HAND_OFFSET := Vector3(0.3, -0.25, -0.15)
const OBJECT_ID := 0x51A7C0DE
const OBJECT_POSITION := Vector3(-2.75, 0.5, 4.25)
const RECEIVER_POSITION := Vector3(-1.0, 1.5, 2.0)

var _manager: NetSyncManager
var _role := ROLE_SENDER
var _room := "godot_test_room"
var _server := "127.0.0.1"
var _control := 5555
var _transform := 5557
var _sub := 5556
var _device := ""
var _timeout := 40.0

var _checks := 0
var _failures: Array[String] = []
var _elapsed := 0.0
var _phase := "connecting"
var _peer_client_no := 0
var _rpc_seen := false
var _finished := false


func _initialize() -> void:
	_parse_arguments()

	if not ClassDB.class_exists("NetSyncBridge"):
		_emit({"result": "error", "message": "the native extension is not loaded"})
		quit(2)
		return

	_manager = NetSyncManager.new()
	_manager.name = "NetSyncManager"
	_manager.server_address = _server
	_manager.control_port = _control
	_manager.transform_port = _transform
	_manager.sub_port = _sub
	_manager.room_id = _room
	_manager.device_id = _device
	_manager.transform_send_rate = 20.0
	_manager.auto_connect = false
	_manager.verbose_logging = false
	root.add_child(_manager)

	_manager.ready_to_sync.connect(_on_ready)
	_manager.avatar_connected.connect(_on_avatar_connected)
	_manager.rpc_received.connect(_on_rpc_received)
	_manager.connection_error.connect(
		func(message): _emit({"result": "error", "message": message})
	)

	_emit({"result": "started", "role": _role, "device_id": _device})
	if not _manager.connect_to_server():
		_emit({"result": "error", "message": "connect_to_server refused"})
		quit(2)


func _process(delta: float) -> bool:
	if _finished:
		return true

	_elapsed += delta
	if _elapsed > _timeout:
		_fail("timed out in phase '%s'" % _phase)
		_finish()
		return true

	match _phase:
		"connecting":
			pass
		"publishing":
			_publish()
		"observing":
			_observe()
	return false


func _parse_arguments() -> void:
	for argument in OS.get_cmdline_user_args():
		var parts := argument.split("=", true, 1)
		if parts.size() != 2:
			continue
		var key: String = parts[0].lstrip("-")
		var value: String = parts[1]
		match key:
			"role": _role = value
			"room": _room = value
			"server": _server = value
			"control": _control = int(value)
			"transform": _transform = int(value)
			"sub": _sub = int(value)
			"device": _device = value
			"timeout": _timeout = float(value)
	if _device.is_empty():
		_device = "godot-test-" + _role + "-" + str(randi())


func _on_ready() -> void:
	_emit({"result": "ready", "client_no": _manager.client_no, "role": _role})
	_check(_manager.client_no > 0, "the server assigned a client number")
	_check(_manager.is_ready(), "is_ready() is true once ready_to_sync fires")
	# Both sides register the shared object: room-object broadcasts are only
	# tracked for ids this client knows about.
	_manager.register_object_id(OBJECT_ID)
	_phase = "publishing" if _role == ROLE_SENDER else "observing"


func _on_avatar_connected(client_no: int, device_id: String) -> void:
	_peer_client_no = client_no
	_emit({"result": "peer", "client_no": client_no, "device_id": device_id})


func _on_rpc_received(sender_client_no: int, function_name: String, args: PackedStringArray) -> void:
	if function_name != "InteropPing":
		return
	# Broadcast RPCs come back to the sender too; only the peer's copy counts.
	if sender_client_no == _manager.client_no:
		return
	_rpc_seen = true
	_check(args.size() == 2, "the RPC carried two arguments")
	if args.size() == 2:
		_check(args[0] == "godot", "RPC argument 0 survived the round trip")
		_check(args[1] == "日本語 \"quoted\"", "a non-ASCII, quoted RPC argument survived")


# --- Sender ---------------------------------------------------------------------

var _claimed_object := false


func _publish() -> void:
	var head := Transform3D(Basis(Vector3.UP, deg_to_rad(TEST_YAW_DEGREES)), TEST_POSITION)
	var right := Transform3D(Basis.IDENTITY, TEST_POSITION + TEST_RIGHT_HAND_OFFSET)
	_manager.set_local_pose({"head": head, "right_hand": right})

	if not _claimed_object:
		_claimed_object = true
		_manager.request_object_ownership(OBJECT_ID)
		_manager.set_global_variable("interop_phase", "sending")

	if _manager.get_object_state(OBJECT_ID).get("is_owned_by_me", false):
		_manager.submit_object_transform(OBJECT_ID, Transform3D(Basis.IDENTITY, OBJECT_POSITION))

	# Keep publishing so the receiver has time to observe, and send the RPC once
	# the peer is known.
	if _peer_client_no != 0 and not _rpc_seen and _elapsed > 3.0:
		_manager.send_rpc("InteropPing", PackedStringArray(["godot", "日本語 \"quoted\""]))
		_rpc_seen = true  # only send once

	if _elapsed > _timeout - 5.0:
		_finish()


# --- Receiver -------------------------------------------------------------------

var _pose_ok := false
var _object_ok := false
var _global_ok := false
var _rpc_ok := false


func _observe() -> void:
	# Publish a pose so the sender sees this client join. Presence is derived
	# from the room pose broadcast, so a client that never sends one is invisible
	# to its peers (which is exactly how stealth mode works).
	_manager.set_local_pose({
		"head": Transform3D(Basis.IDENTITY, RECEIVER_POSITION),
	})

	if _peer_client_no == 0:
		return

	if not _pose_ok:
		var pose: Dictionary = _manager.get_remote_pose(_peer_client_no)
		if not pose.is_empty() and pose.get("has_head", false):
			var head: Transform3D = pose["head"]
			# 0.01 m quantisation on the head position: half a step, plus slack.
			var position_error := (head.origin - TEST_POSITION).length()
			var yaw := rad_to_deg(head.basis.get_euler().y)
			var yaw_error: float = abs(angle_difference(deg_to_rad(yaw), deg_to_rad(TEST_YAW_DEGREES)))
			if position_error <= 0.02:
				_pose_ok = true
				_check(true, "the peer's head position round-tripped through the wire")
				_check(
					rad_to_deg(yaw_error) <= 0.5,
					"the peer's head yaw round-tripped (error %.3f degrees)" % rad_to_deg(yaw_error)
				)
				_emit({
					"result": "pose",
					"x": head.origin.x, "y": head.origin.y, "z": head.origin.z,
					"yaw": yaw,
					"position_error": position_error,
				})

				if pose.get("has_right_hand", false):
					var right: Transform3D = pose["right_hand"]
					var expected := TEST_POSITION + TEST_RIGHT_HAND_OFFSET
					var hand_error := (right.origin - expected).length()
					# Hands are sent relative to the head, so the error is the
					# sum of the head (0.01 m) and relative (0.005 m) steps.
					_check(
						hand_error <= 0.03,
						"the peer's right hand round-tripped (error %.4f m)" % hand_error
					)
			elif _elapsed > _timeout - 8.0:
				_fail("head position error %.4f m is too large" % position_error)
				_pose_ok = true

	if not _object_ok:
		var state: Dictionary = _manager.get_object_state(OBJECT_ID)
		if state.get("found", false) and state.get("has_pose", false):
			var transform: Transform3D = state["transform"]
			var error := (transform.origin - OBJECT_POSITION).length()
			if error <= 0.02:
				_object_ok = true
				_check(true, "the shared object's pose round-tripped")
				_check(
					state["owner_client_no"] == _peer_client_no,
					"the shared object is owned by the peer"
				)
				_emit({
					"result": "object",
					"x": transform.origin.x, "y": transform.origin.y, "z": transform.origin.z,
					"owner": state["owner_client_no"],
				})

	if not _global_ok and _manager.get_global_variable("interop_phase", "") == "sending":
		_global_ok = true
		_check(true, "the global network variable reached the receiver")

	if not _rpc_ok and _rpc_seen:
		_rpc_ok = true
		_check(true, "the RPC reached the receiver")

	if _pose_ok and _object_ok and _global_ok and _rpc_ok:
		_finish()


# --- Plumbing --------------------------------------------------------------------

func _check(condition: bool, message: String) -> void:
	_checks += 1
	if not condition:
		_failures.append(message)
		_emit({"result": "check_failed", "message": message})


func _fail(message: String) -> void:
	_checks += 1
	_failures.append(message)
	_emit({"result": "check_failed", "message": message})


func _finish() -> void:
	if _finished:
		return
	_finished = true
	if _role == ROLE_RECEIVER:
		if not _pose_ok:
			_fail("never received the peer's pose")
		if not _object_ok:
			_fail("never received the shared object's pose")
		if not _global_ok:
			_fail("never received the global network variable")
		if not _rpc_ok:
			_fail("never received the RPC")

	_emit({
		"result": "summary",
		"role": _role,
		"checks": _checks,
		"failures": _failures.size(),
		"messages": _failures,
	})
	if _manager != null:
		_manager.disconnect_from_server()
	quit(0 if _failures.is_empty() else 1)


func _emit(payload: Dictionary) -> void:
	print("@@" + JSON.stringify(payload))
