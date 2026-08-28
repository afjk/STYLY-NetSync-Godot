# SPDX-License-Identifier: Apache-2.0
@icon("res://addons/styly_netsync/icons/net_sync_manager.svg")
class_name NetSyncManager
extends Node

## Godot client for STYLY NetSync.
##
## Add one of these to your scene, set [member room_id] (and optionally
## [member server_address]), and call [method connect_to_server]. Everything the
## protocol needs — the handshake, pose upload, RPC, network variables and
## object ownership — is driven from [method _process].
##
## Coordinates: this node speaks Godot space. The conversion to the wire's
## Unity-handed convention happens once, inside the native bridge, so a Unity
## peer sees your avatar and objects at the position and rotation you set here.
##
## [b]Threading:[/b] ZeroMQ runs on its own thread inside the extension. Signals
## from this node are always emitted from [method _process] on the main thread,
## so it is safe to touch nodes in their handlers.

# --- Signals ------------------------------------------------------------------

## Emitted once the client is fully usable: connected, handshaken and network
## variables synchronised. Reading a network variable before this fires returns
## the fallback, so wait for it.
signal ready_to_sync

## The connection state changed. [param state] is one of the [enum State] values.
signal connection_state_changed(state: int, state_name: String)

## The network thread failed. The client stays in [constant STATE_ERROR] until
## [method connect_to_server] is called again.
signal connection_error(message: String)

## LAN discovery found a server. Only fires when [member server_address] is empty.
signal server_discovered(address: String, server_name: String)

## The server assigned this client its number.
signal client_no_assigned(client_no: int)

## A remote client joined the room.
signal avatar_connected(client_no: int, device_id: String)

## A remote client left the room.
signal avatar_disconnected(client_no: int)

## An RPC arrived. Broadcast RPCs are echoed back to the sender by the server,
## so this also fires for RPCs you sent yourself — check [param sender_client_no]
## against [member client_no] if you need to ignore your own.
signal rpc_received(sender_client_no: int, function_name: String, args: PackedStringArray)

## A global network variable changed. [param old_value] is empty when the name
## is new.
signal global_variable_changed(name: String, old_value: String, new_value: String)

## A client network variable changed. [param new_value] is empty when the
## variable was removed by an authoritative snapshot.
signal client_variable_changed(client_no: int, name: String, old_value: String, new_value: String)

## Ownership of a synchronised object moved. Owner 0 means unowned.
signal object_ownership_changed(object_id: int, new_owner: int, previous_owner: int)

## An ownership request was refused. [param reason] is the server's reason code
## (1 = not the owner).
signal object_ownership_rejected(object_id: int, current_owner: int, reason: int)

## The server reported its version, once per connection.
signal server_version_received(major: int, minor: int, patch: int)

# --- Enums --------------------------------------------------------------------

enum State {
	STATE_DISCONNECTED = 0,
	STATE_CONNECTING = 1,
	STATE_CONNECTED = 2,
	STATE_SYNCHRONIZING = 3,
	STATE_READY = 4,
	STATE_ERROR = 5,
}

# --- Configuration ------------------------------------------------------------

## Server host, with or without the `tcp://` scheme. Leave empty to discover a
## server on the local network.
@export var server_address: String = ""

## Room to join. Every client in the same room sees each other.
@export var room_id: String = "default_room"

## Stable identity for this installation. Leave empty to generate one on first
## run and persist it under `user://`, which survives restarts on desktop and
## on Android.
@export var device_id: String = ""

## A stealth client has no avatar: it joins the room and can use RPC and network
## variables, but publishes no pose and is not drawn by other clients.
@export var stealth_mode: bool = false

## Pose sends per second. Clamped to 0.5–60.
@export_range(0.5, 60.0, 0.1) var transform_send_rate: float = 10.0

## Connect as soon as this node enters the tree.
@export var auto_connect: bool = true

@export_group("Ports")
@export var control_port: int = 5555
@export var transform_port: int = 5557
@export var sub_port: int = 5556
@export var discovery_port: int = 9999

@export_group("Advanced")
## Forward native log lines to the Godot console. Useful while bringing up a
## connection; noisy in production.
@export var verbose_logging: bool = false
## Maximum RPCs per [member rpc_rate_limit_window] seconds. 0 disables the limit.
@export var rpc_rate_limit: int = 30
@export var rpc_rate_limit_window: float = 1.0

# --- Constants ----------------------------------------------------------------

const DEVICE_ID_PATH := "user://styly_netsync_device_id.txt"

# --- State --------------------------------------------------------------------

static var _instance: NetSyncManager = null

var _bridge: NetSyncBridge = null
var _objects: Dictionary = {}
var _pose_sources: Array[NetSyncAvatar] = []

## The single manager in the scene tree, or null. Convenient for
## [code]NetSyncManager.instance()[/code] from anywhere; a scene with more than
## one manager keeps the first and warns.
static func instance() -> NetSyncManager:
	return _instance

# --- Lifecycle ----------------------------------------------------------------

func _init() -> void:
	if not ClassDB.class_exists("NetSyncBridge"):
		push_error(
			"[STYLY NetSync] The native extension is not loaded. Build it with "
			+ "`scons target=template_debug` and make sure "
			+ "addons/styly_netsync/styly_netsync.gdextension is present."
		)
		return
	_bridge = ClassDB.instantiate("NetSyncBridge")


func _enter_tree() -> void:
	if _instance != null and _instance != self:
		push_warning(
			"[STYLY NetSync] More than one NetSyncManager is in the tree; '%s' will not be "
			% name
			+ "used as the shared instance. Keep exactly one."
		)
	else:
		_instance = self


func _exit_tree() -> void:
	if _instance == self:
		_instance = null
	disconnect_from_server()


func _ready() -> void:
	if _bridge == null:
		set_process(false)
		return

	_bridge.connection_state_changed.connect(_on_connection_state_changed)
	_bridge.ready.connect(_on_ready)
	_bridge.connection_error.connect(_on_connection_error)
	_bridge.server_discovered.connect(_on_server_discovered)
	_bridge.client_no_assigned.connect(_on_client_no_assigned)
	_bridge.avatar_connected.connect(_on_avatar_connected)
	_bridge.avatar_disconnected.connect(_on_avatar_disconnected)
	_bridge.rpc_received.connect(_on_rpc_received)
	_bridge.global_variable_changed.connect(_on_global_variable_changed)
	_bridge.client_variable_changed.connect(_on_client_variable_changed)
	_bridge.object_ownership_changed.connect(_on_object_ownership_changed)
	_bridge.object_ownership_rejected.connect(_on_object_ownership_rejected)
	_bridge.server_version_received.connect(_on_server_version_received)
	_bridge.log_message.connect(_on_log_message)

	if auto_connect:
		connect_to_server()


func _process(_delta: float) -> void:
	if _bridge == null:
		return

	# Publish the local avatar pose before draining, so a pose set this frame is
	# considered by the send throttle in the same tick.
	for source in _pose_sources:
		if is_instance_valid(source) and source.is_local_avatar:
			_bridge.set_local_pose(source.build_pose())

	# Owned objects push their transform every frame; the native side decides
	# what actually goes on the wire (rate limit, change detection, heartbeat).
	for object_id in _objects:
		var object: NetSyncObject = _objects[object_id]
		if not is_instance_valid(object):
			continue
		if object.is_owned_by_me():
			_bridge.submit_object_pose(object_id, object.global_transform)

	# Drains the network queues and emits every signal for this frame.
	_bridge.poll()

	# Apply received poses to non-owned objects.
	for object_id in _objects:
		var object: NetSyncObject = _objects[object_id]
		if is_instance_valid(object):
			object._apply_remote_state(_bridge.get_object_state(object_id))

	for source in _pose_sources:
		if is_instance_valid(source) and not source.is_local_avatar:
			source._apply_remote_pose(_bridge.get_remote_pose(source.client_no))

# --- Connection ----------------------------------------------------------------

## Connect to the server, or start LAN discovery when [member server_address] is
## empty. Returns false when the extension is missing or a connection is already
## in progress.
func connect_to_server() -> bool:
	if _bridge == null:
		return false

	var resolved_device_id := device_id
	if resolved_device_id.is_empty():
		resolved_device_id = _load_or_create_device_id()
		device_id = resolved_device_id

	var config := {
		"server_address": server_address,
		"control_port": control_port,
		"transform_port": transform_port,
		"sub_port": sub_port,
		"discovery_port": discovery_port,
		"room_id": room_id,
		"device_id": resolved_device_id,
		"stealth_mode": stealth_mode,
		"transform_send_rate": transform_send_rate,
		"enable_discovery": server_address.is_empty(),
	}
	var started: bool = _bridge.connect_to_server(config)
	if started:
		_bridge.configure_rpc_rate_limit(rpc_rate_limit, rpc_rate_limit_window, 0.5)
	return started


## Tear down the connection and drop all room-scoped state.
func disconnect_from_server() -> void:
	if _bridge != null:
		_bridge.disconnect_from_server()


## True when all three sockets are up and no error has occurred. This is [i]not[/i]
## the same as being usable — see [method is_ready].
func is_connected_to_server() -> bool:
	return _bridge != null and _bridge.is_connected_to_server()


## True when the client is connected, has been assigned a client number and has
## received the initial network-variable sync. Matches Unity's
## [code]NetSyncManager.IsReady[/code].
func is_ready() -> bool:
	return _bridge != null and _bridge.is_ready()


## The current [enum State].
func get_connection_state() -> int:
	return _bridge.get_connection_state() if _bridge != null else State.STATE_DISCONNECTED


## The client number the server assigned, or 0 before the handshake completes.
var client_no: int:
	get:
		return _bridge.get_client_no() if _bridge != null else 0


## The resolved server address. Empty until discovery completes.
var resolved_server_address: String:
	get:
		return _bridge.get_server_address() if _bridge != null else ""

# --- Remote clients --------------------------------------------------------------

## Client numbers currently publishing a pose in this room, excluding this client.
func get_remote_client_numbers() -> PackedInt32Array:
	return _bridge.get_remote_client_numbers() if _bridge != null else PackedInt32Array()


## Every client number the server has told us about, including stealth clients
## that publish no pose.
func get_known_client_numbers() -> PackedInt32Array:
	return _bridge.get_known_client_numbers() if _bridge != null else PackedInt32Array()


## The latest pose of a remote client, in Godot space. Empty when unknown.
## Keys: head, right_hand, left_hand, physical (Transform3D), virtuals (Array),
## has_head/has_right_hand/has_left_hand/has_virtuals (bool), pose_time,
## broadcast_time, is_stealth.
func get_remote_pose(target_client_no: int) -> Dictionary:
	return _bridge.get_remote_pose(target_client_no) if _bridge != null else {}


func get_device_id_for(target_client_no: int) -> String:
	return _bridge.get_device_id_for(target_client_no) if _bridge != null else ""


func is_client_stealth(target_client_no: int) -> bool:
	return _bridge.is_client_stealth(target_client_no) if _bridge != null else false

# --- Local pose -------------------------------------------------------------------

## Set this client's avatar pose directly, in Godot world space. Pass only the
## parts you have; a missing part is not transmitted.
##
## Prefer attaching a [NetSyncAvatar] (optionally driven by [NetSyncXRAdapter])
## over calling this every frame yourself.
func set_local_pose(pose: Dictionary) -> void:
	if _bridge != null:
		_bridge.set_local_pose(pose)


## Stop transmitting an avatar pose.
func clear_local_pose() -> void:
	if _bridge != null:
		_bridge.clear_local_pose()


## Register a pose source so the manager drives it each frame. Called by
## [NetSyncAvatar]; you do not normally call this yourself.
func register_pose_source(source: NetSyncAvatar) -> void:
	if not _pose_sources.has(source):
		_pose_sources.append(source)


func unregister_pose_source(source: NetSyncAvatar) -> void:
	_pose_sources.erase(source)

# --- RPC ---------------------------------------------------------------------------

## Broadcast an RPC to every client in the room, including this one.
func send_rpc(function_name: String, args: PackedStringArray = PackedStringArray()) -> void:
	if _bridge != null:
		_bridge.send_rpc(function_name, args)


## Send an RPC to a single client.
func send_rpc_to(
	target_client_no: int, function_name: String, args: PackedStringArray = PackedStringArray()
) -> void:
	if _bridge != null:
		_bridge.send_rpc_to(target_client_no, function_name, args)


## Send an RPC to a specific set of clients. At most 255 targets.
func send_rpc_to_many(
	target_client_nos: PackedInt32Array,
	function_name: String,
	args: PackedStringArray = PackedStringArray()
) -> void:
	if _bridge != null:
		_bridge.send_rpc_to_many(target_client_nos, function_name, args)

# --- Network variables ---------------------------------------------------------------

## Set a room-wide variable. Names are limited to 64 characters and values to
## 1024; longer input is rejected and returns false.
func set_global_variable(name: String, value: String) -> bool:
	return _bridge.set_global_variable(name, value) if _bridge != null else false


## Read a room-wide variable. Returns [param default_value] before the initial
## sync arrives, so wait for [signal ready_to_sync].
func get_global_variable(name: String, default_value: String = "") -> String:
	return _bridge.get_global_variable(name, default_value) if _bridge != null else default_value


func get_all_global_variables() -> Dictionary:
	return _bridge.get_all_global_variables() if _bridge != null else {}


## Set one of this client's own variables.
func set_client_variable(name: String, value: String) -> bool:
	return _bridge.set_client_variable(name, value) if _bridge != null else false


## Set a variable on another client. The server accepts writes to any client.
func set_client_variable_for(target_client_no: int, name: String, value: String) -> bool:
	return (
		_bridge.set_client_variable_for(target_client_no, name, value) if _bridge != null else false
	)


func get_client_variable(
	target_client_no: int, name: String, default_value: String = ""
) -> String:
	return (
		_bridge.get_client_variable(target_client_no, name, default_value)
		if _bridge != null
		else default_value
	)


func get_all_client_variables(target_client_no: int) -> Dictionary:
	return _bridge.get_all_client_variables(target_client_no) if _bridge != null else {}


## Remove every variable this client owns, on the server and locally.
func clear_client_variables() -> bool:
	return _bridge.clear_my_client_variables() if _bridge != null else false

# --- Object sync ------------------------------------------------------------------------

## Register a [NetSyncObject]. Called from the object's `_enter_tree`.
func register_object(object: NetSyncObject) -> bool:
	if _bridge == null or object.object_id == 0:
		return false
	_objects[object.object_id] = object
	return _bridge.register_object(object.object_id)


func unregister_object(object: NetSyncObject) -> void:
	if _bridge == null:
		return
	_objects.erase(object.object_id)
	_bridge.unregister_object(object.object_id)


## Register a synchronised object by id, without a [NetSyncObject] node. Use
## this when the object's transform lives somewhere the scene tree does not —
## a script-driven simulation, a MultiMesh instance, a headless client.
## You are then responsible for calling [method submit_object_transform] while
## you own it, and for reading [method get_object_state] when you do not.
func register_object_id(object_id: int) -> bool:
	return _bridge.register_object(object_id) if _bridge != null else false


func unregister_object_id(object_id: int) -> void:
	if _bridge != null:
		_bridge.unregister_object(object_id)


## Publish the current transform of an object registered with
## [method register_object_id]. Ignored unless this client owns it. The send
## rate, change detection and heartbeat are handled for you.
func submit_object_transform(object_id: int, transform: Transform3D) -> void:
	if _bridge != null:
		_bridge.submit_object_pose(object_id, transform)


## Ask the server for ownership of an object. The server grants requests
## unconditionally; watch [signal object_ownership_changed] for the result.
func request_object_ownership(object_id: int) -> bool:
	return _bridge.request_object_ownership(object_id) if _bridge != null else false


## Give up ownership. Refused with [signal object_ownership_rejected] when this
## client is not the current owner.
func release_object_ownership(object_id: int) -> bool:
	return _bridge.release_object_ownership(object_id) if _bridge != null else false


func get_object_state(object_id: int) -> Dictionary:
	return _bridge.get_object_state(object_id) if _bridge != null else {}

# --- Diagnostics ---------------------------------------------------------------------------

## Transport counters: messages sent/received, dropped transform frames,
## backpressure events and control-queue depth.
func get_transport_stats() -> Dictionary:
	return _bridge.get_transport_stats() if _bridge != null else {}

# --- Internals -----------------------------------------------------------------------------

func _load_or_create_device_id() -> String:
	# The native side does the file I/O so the same code path works on Android,
	# where `user://` maps to app-private storage that survives a restart.
	var absolute := ProjectSettings.globalize_path(DEVICE_ID_PATH)
	if absolute.is_empty():
		# globalize_path returns "" for user:// inside an exported PCK on some
		# platforms; fall back to a Godot-side read/write.
		return _load_or_create_device_id_via_godot()
	var value: String = NetSyncBridge.load_or_create_device_id(absolute)
	if value.is_empty():
		return _load_or_create_device_id_via_godot()
	return value


func _load_or_create_device_id_via_godot() -> String:
	if FileAccess.file_exists(DEVICE_ID_PATH):
		var reader := FileAccess.open(DEVICE_ID_PATH, FileAccess.READ)
		if reader != null:
			var stored := reader.get_as_text().strip_edges()
			reader.close()
			if not stored.is_empty():
				return stored
	var generated: String = NetSyncBridge.generate_device_id()
	var writer := FileAccess.open(DEVICE_ID_PATH, FileAccess.WRITE)
	if writer != null:
		writer.store_string(generated)
		writer.close()
	else:
		push_warning(
			"[STYLY NetSync] Could not persist the device id to %s; a new identity " % DEVICE_ID_PATH
			+ "will be generated on the next run."
		)
	return generated


func _on_connection_state_changed(state: int, state_name: String) -> void:
	connection_state_changed.emit(state, state_name)


func _on_ready() -> void:
	ready_to_sync.emit()


func _on_connection_error(message: String) -> void:
	push_error("[STYLY NetSync] " + message)
	connection_error.emit(message)


func _on_server_discovered(
	address: String, server_name: String, _control: int, _transform: int, _sub: int
) -> void:
	server_discovered.emit(address, server_name)


func _on_client_no_assigned(assigned: int) -> void:
	client_no_assigned.emit(assigned)


func _on_avatar_connected(remote_client_no: int, remote_device_id: String) -> void:
	avatar_connected.emit(remote_client_no, remote_device_id)


func _on_avatar_disconnected(remote_client_no: int) -> void:
	avatar_disconnected.emit(remote_client_no)


func _on_rpc_received(
	sender_client_no: int, function_name: String, args: PackedStringArray
) -> void:
	rpc_received.emit(sender_client_no, function_name, args)


func _on_global_variable_changed(name: String, old_value: String, new_value: String) -> void:
	global_variable_changed.emit(name, old_value, new_value)


func _on_client_variable_changed(
	owner_client_no: int, name: String, old_value: String, new_value: String
) -> void:
	client_variable_changed.emit(owner_client_no, name, old_value, new_value)


func _on_object_ownership_changed(object_id: int, new_owner: int, previous_owner: int) -> void:
	var object: NetSyncObject = _objects.get(object_id)
	if is_instance_valid(object):
		object._on_ownership_changed(new_owner, previous_owner)
	object_ownership_changed.emit(object_id, new_owner, previous_owner)


func _on_object_ownership_rejected(object_id: int, current_owner: int, reason: int) -> void:
	var object: NetSyncObject = _objects.get(object_id)
	if is_instance_valid(object):
		object.ownership_rejected.emit(current_owner, reason)
	object_ownership_rejected.emit(object_id, current_owner, reason)


func _on_server_version_received(major: int, minor: int, patch: int) -> void:
	server_version_received.emit(major, minor, patch)


func _on_log_message(message: String) -> void:
	if verbose_logging:
		print("[STYLY NetSync] ", message)
