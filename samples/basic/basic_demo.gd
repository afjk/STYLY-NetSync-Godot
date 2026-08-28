# SPDX-License-Identifier: Apache-2.0
extends Node3D

## Basic STYLY NetSync sample — no XR hardware needed.
##
## Connect to a server, watch who else is in the room, move a dummy avatar with
## the arrow keys, fire RPCs, set network variables and grab a shared object.
## Everything here uses only the public [NetSyncManager] / [NetSyncObject] API.

@onready var _manager: NetSyncManager = %NetSyncManager
@onready var _server_field: LineEdit = %ServerAddress
@onready var _room_field: LineEdit = %RoomId
@onready var _connect_button: Button = %ConnectButton
@onready var _status_label: RichTextLabel = %StatusLabel
@onready var _clients_label: RichTextLabel = %ClientsLabel
@onready var _log: RichTextLabel = %Log

@onready var _rpc_name: LineEdit = %RpcName
@onready var _rpc_arg: LineEdit = %RpcArg
@onready var _global_name: LineEdit = %GlobalName
@onready var _global_value: LineEdit = %GlobalValue
@onready var _client_var_name: LineEdit = %ClientVarName
@onready var _client_var_value: LineEdit = %ClientVarValue

@onready var _local_avatar: Node3D = %LocalAvatarRoot
@onready var _shared_cube: NetSyncObject = %SharedCube

const REMOTE_AVATAR_SCENE := preload("res://samples/basic/remote_avatar.tscn")

## Remote avatars, keyed by client number.
var _remote_avatars: Dictionary = {}
var _move_speed := 3.0
var _turn_speed := 90.0


func _ready() -> void:
	_server_field.text = _manager.server_address
	_room_field.text = _manager.room_id

	_manager.connection_state_changed.connect(_on_connection_state_changed)
	_manager.ready_to_sync.connect(_on_ready_to_sync)
	_manager.connection_error.connect(func(message): _append_log("[color=#ff8080]error:[/color] " + message))
	_manager.server_discovered.connect(
		func(address, server_name): _append_log("discovered '%s' at %s" % [server_name, address])
	)
	_manager.client_no_assigned.connect(
		func(number): _append_log("assigned client number %d" % number)
	)
	_manager.avatar_connected.connect(_on_avatar_connected)
	_manager.avatar_disconnected.connect(_on_avatar_disconnected)
	_manager.rpc_received.connect(_on_rpc_received)
	_manager.global_variable_changed.connect(
		func(name, old_value, new_value): _append_log(
			"global %s: '%s' -> '%s'" % [name, old_value, new_value]
		)
	)
	_manager.client_variable_changed.connect(
		func(client_no, name, _old, new_value): _append_log(
			"client %d %s = '%s'" % [client_no, name, new_value]
		)
	)
	_manager.object_ownership_changed.connect(
		func(object_id, new_owner, previous_owner): _append_log(
			"object 0x%08X owner %d -> %d" % [object_id, previous_owner, new_owner]
		)
	)
	_manager.object_ownership_rejected.connect(
		func(object_id, current_owner, reason): _append_log(
			"object 0x%08X ownership refused (owner %d, reason %d)"
			% [object_id, current_owner, reason]
		)
	)

	_append_log("device id: " + _manager.device_id)
	_append_log("Arrow keys move the local avatar; the camera stays put.")


func _process(delta: float) -> void:
	_drive_local_avatar(delta)
	_refresh_status()


func _drive_local_avatar(delta: float) -> void:
	# A stand-in for a tracked head: the avatar this client publishes.
	var forward := Input.get_axis("ui_down", "ui_up")
	var turn := Input.get_axis("ui_right", "ui_left")

	if turn != 0.0:
		_local_avatar.rotate_y(deg_to_rad(turn * _turn_speed * delta))
	if forward != 0.0:
		# -Z is forward in Godot.
		_local_avatar.global_position += (
			-_local_avatar.global_transform.basis.z * forward * _move_speed * delta
		)


func _refresh_status() -> void:
	var lines := [
		"state: [b]%s[/b]" % _manager.get_connection_state_name(),
		"client number: [b]%d[/b]" % _manager.client_no,
		"ready: [b]%s[/b]" % ("yes" if _manager.is_ready() else "no"),
		"server: %s" % (_manager.resolved_server_address if not _manager.resolved_server_address.is_empty() else "-"),
	]
	_status_label.text = "\n".join(lines)

	var clients := PackedStringArray()
	for number in _manager.get_known_client_numbers():
		var marker := " (me)" if number == _manager.client_no else ""
		var stealth := " [stealth]" if _manager.is_client_stealth(number) else ""
		clients.append("%d%s%s" % [number, marker, stealth])
	_clients_label.text = (
		"in room: " + (", ".join(clients) if clients.size() > 0 else "-")
	)


# --- UI callbacks ---------------------------------------------------------------

func _on_connect_button_pressed() -> void:
	if _manager.is_connected_to_server():
		_manager.disconnect_from_server()
		_append_log("disconnected")
		return

	_manager.server_address = _server_field.text.strip_edges()
	_manager.room_id = _room_field.text.strip_edges()
	if _manager.room_id.is_empty():
		_manager.room_id = "default_room"
		_room_field.text = _manager.room_id

	if _manager.server_address.is_empty():
		_append_log("no address given — discovering a server on the local network...")
	if not _manager.connect_to_server():
		_append_log("[color=#ff8080]could not start connecting[/color]")


func _on_rpc_button_pressed() -> void:
	var function_name := _rpc_name.text.strip_edges()
	if function_name.is_empty():
		return
	var args := PackedStringArray()
	if not _rpc_arg.text.is_empty():
		args.append(_rpc_arg.text)
	_manager.send_rpc(function_name, args)
	_append_log("sent RPC %s(%s)" % [function_name, ", ".join(args)])


func _on_set_global_pressed() -> void:
	var name := _global_name.text.strip_edges()
	if name.is_empty():
		return
	if not _manager.set_global_variable(name, _global_value.text):
		_append_log("[color=#ff8080]global variable rejected (name/value too long?)[/color]")


func _on_set_client_var_pressed() -> void:
	var name := _client_var_name.text.strip_edges()
	if name.is_empty():
		return
	if not _manager.set_client_variable(name, _client_var_value.text):
		_append_log("[color=#ff8080]client variable rejected[/color]")


func _on_clear_client_vars_pressed() -> void:
	if _manager.clear_client_variables():
		_append_log("cleared this client's variables")


func _on_grab_cube_pressed() -> void:
	if _shared_cube.is_owned_by_me():
		_shared_cube.release_ownership()
	else:
		_shared_cube.request_ownership()


func _on_nudge_cube_pressed() -> void:
	if not _shared_cube.is_owned_by_me():
		_append_log("grab the cube first — only its owner may move it")
		return
	_shared_cube.global_position += Vector3(randf_range(-1.0, 1.0), 0.0, randf_range(-1.0, 1.0))

# --- Network callbacks ----------------------------------------------------------

func _on_connection_state_changed(_state: int, state_name: String) -> void:
	_append_log("state -> " + state_name)
	_connect_button.text = "Disconnect" if _manager.is_connected_to_server() else "Connect"


func _on_ready_to_sync() -> void:
	_append_log("[color=#80ff80]ready[/color] — client number %d" % _manager.client_no)
	# Network variables are only readable once ready.
	_manager.set_client_variable("engine", "godot")


func _on_avatar_connected(client_no: int, device_id: String) -> void:
	_append_log("client %d joined (%s)" % [client_no, device_id])
	if _remote_avatars.has(client_no):
		return
	var avatar: NetSyncAvatar = REMOTE_AVATAR_SCENE.instantiate()
	avatar.name = "RemoteAvatar_%d" % client_no
	avatar.is_local_avatar = false
	avatar.client_no = client_no
	%World.add_child(avatar)
	_remote_avatars[client_no] = avatar


func _on_avatar_disconnected(client_no: int) -> void:
	_append_log("client %d left" % client_no)
	var avatar: Node = _remote_avatars.get(client_no)
	if is_instance_valid(avatar):
		avatar.queue_free()
	_remote_avatars.erase(client_no)


func _on_rpc_received(sender_client_no: int, function_name: String, args: PackedStringArray) -> void:
	var origin := "me" if sender_client_no == _manager.client_no else "client %d" % sender_client_no
	_append_log("RPC from %s: %s(%s)" % [origin, function_name, ", ".join(args)])


func _append_log(message: String) -> void:
	_log.append_text(message + "\n")
	# Keep the newest line visible without letting the buffer grow forever.
	if _log.get_line_count() > 400:
		_log.text = ""
	_log.scroll_to_line(_log.get_line_count() - 1)
