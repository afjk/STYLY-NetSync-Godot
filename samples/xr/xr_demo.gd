# SPDX-License-Identifier: Apache-2.0
extends Node3D

## XR STYLY NetSync sample.
##
## Shows the shape of a real XR client: an [XROrigin3D] rig whose camera and
## controllers drive a local [NetSyncAvatar] through [NetSyncXRAdapter], and one
## remote avatar spawned per peer.
##
## The rig's movement away from where it started is sent as the locomotion delta,
## which is what lets peers reconstruct each user's real-world (physical) pose —
## the same thing the Unity client's XR Origin delta carries.
##
## Without a headset this still runs: OpenXR fails to initialise, the camera
## stays at the origin, and you can watch remote avatars from a flat window.

@onready var _manager: NetSyncManager = %NetSyncManager
@onready var _world: Node3D = %World
@onready var _status: Label3D = %Status

const REMOTE_AVATAR_SCENE := preload("res://samples/basic/remote_avatar.tscn")

var _remote_avatars: Dictionary = {}
var _xr_interface: XRInterface = null


func _ready() -> void:
	_start_xr()

	_manager.avatar_connected.connect(_on_avatar_connected)
	_manager.avatar_disconnected.connect(_on_avatar_disconnected)
	_manager.ready_to_sync.connect(
		func(): _manager.set_client_variable("engine", "godot-xr")
	)
	_manager.rpc_received.connect(_on_rpc_received)


func _process(_delta: float) -> void:
	_status.text = "%s\nclient %d\npeers: %d" % [
		"ready" if _manager.is_ready() else "connecting…",
		_manager.client_no,
		_remote_avatars.size(),
	]


func _start_xr() -> void:
	_xr_interface = XRServer.find_interface("OpenXR")
	if _xr_interface == null or not _xr_interface.is_initialized():
		push_warning(
			"[STYLY NetSync] OpenXR is unavailable, so this sample runs as a flat window. "
			+ "Enable OpenXR in Project Settings > XR and connect a headset for the full demo."
		)
		return
	get_viewport().use_xr = true
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)


func _on_avatar_connected(client_no: int, _device_id: String) -> void:
	if _remote_avatars.has(client_no):
		return
	var avatar: NetSyncAvatar = REMOTE_AVATAR_SCENE.instantiate()
	avatar.name = "RemoteAvatar_%d" % client_no
	avatar.is_local_avatar = false
	avatar.client_no = client_no
	_world.add_child(avatar)
	_remote_avatars[client_no] = avatar


func _on_avatar_disconnected(client_no: int) -> void:
	var avatar: Node = _remote_avatars.get(client_no)
	if is_instance_valid(avatar):
		avatar.queue_free()
	_remote_avatars.erase(client_no)


func _on_rpc_received(sender_client_no: int, function_name: String, args: PackedStringArray) -> void:
	if function_name == "Wave" and sender_client_no != _manager.client_no:
		print("client %d waved: %s" % [sender_client_no, ", ".join(args)])
