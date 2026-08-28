# SPDX-License-Identifier: Apache-2.0
@icon("res://addons/styly_netsync/icons/net_sync_object.svg")
class_name NetSyncObject
extends Node3D

## A Node3D whose world transform is synchronised across the room.
##
## Exactly one client owns an object at a time. The owner's transform is
## authoritative and is published; everyone else has this node's transform
## applied from the network. An object with no owner (owner 0) is left alone, so
## local physics or animation can drive it until someone claims it.
##
## [b]Object ids must match across clients.[/b] Set [member object_id] to the
## same value on every client — including the Unity client, whose ids come from
## its own editor pipeline. [member object_name] is a convenience for
## Godot-only scenes: it hashes to a stable id, but that hash will not match
## anything Unity assigns.

## Emitted when this object's owner changes. Owner 0 means unowned.
signal ownership_changed(new_owner: int, previous_owner: int)

## Emitted when an ownership request was refused. [param reason] is the server's
## reason code (1 = not the owner).
signal ownership_rejected(current_owner: int, reason: int)

## The 32-bit id this object is known by, room-wide. Must be non-zero and must
## match on every client. Leave 0 to derive it from [member object_name].
@export var object_id: int = 0:
	set(value):
		object_id = value & 0xFFFFFFFF

## Derives [member object_id] by hashing this name, when [member object_id] is 0.
## Godot-only scenes can use this instead of assigning numbers by hand.
@export var object_name: String = ""

## Take ownership as soon as the client is ready. Use for objects a single
## client should drive from the start; leave off for objects that are grabbed.
@export var claim_ownership_on_ready: bool = false

## How received transforms are applied. [code]0[/code] snaps, any positive value
## is the smoothing half-life in seconds — larger is smoother and laggier.
@export_range(0.0, 0.5, 0.005) var smoothing_half_life: float = 0.05

var _manager: NetSyncManager = null
var _owner_client_no: int = 0
var _has_target: bool = false
var _target_transform: Transform3D = Transform3D.IDENTITY
var _claimed: bool = false


func _enter_tree() -> void:
	if object_id == 0 and not object_name.is_empty():
		object_id = NetSyncBridge.hash_object_id(object_name)

	if object_id == 0:
		push_warning(
			"[STYLY NetSync] NetSyncObject '%s' has no object_id and no object_name, " % name
			+ "so it will not be synchronised. Set one of them."
		)
		return

	_manager = NetSyncManager.instance()
	if _manager == null:
		push_warning(
			"[STYLY NetSync] NetSyncObject '%s' found no NetSyncManager in the tree. " % name
			+ "Add one before this node enters the tree."
		)
		return
	_manager.register_object(self)
	if claim_ownership_on_ready:
		_manager.ready_to_sync.connect(_claim_once, CONNECT_ONE_SHOT)
		if _manager.is_ready():
			_claim_once()


func _exit_tree() -> void:
	if _manager != null:
		_manager.unregister_object(self)
		_manager = null


func _process(delta: float) -> void:
	# The owner drives the transform locally; everyone else follows the network.
	if not _has_target or is_owned_by_me():
		return

	if smoothing_half_life <= 0.0:
		global_transform = _target_transform
		return

	# Exponential approach with a half-life, so the rate is frame-rate
	# independent: after `half_life` seconds the remaining error has halved.
	var weight := 1.0 - pow(0.5, delta / smoothing_half_life)
	global_transform = global_transform.interpolate_with(_target_transform, weight)


## The client number that currently owns this object; 0 when unowned.
func get_owner_client_no() -> int:
	return _owner_client_no


## True when this client owns the object and may drive its transform.
func is_owned_by_me() -> bool:
	return (
		_manager != null
		and _owner_client_no != 0
		and _owner_client_no == _manager.client_no
	)


## Ask the server for ownership. The server grants requests unconditionally, so
## the last requester wins; watch [signal ownership_changed] for confirmation.
func request_ownership() -> bool:
	if _manager == null or object_id == 0:
		return false
	return _manager.request_object_ownership(object_id)


## Give ownership up. Refused with [signal ownership_rejected] when this client
## is not the current owner.
func release_ownership() -> bool:
	if _manager == null or object_id == 0:
		return false
	return _manager.release_object_ownership(object_id)


func _claim_once() -> void:
	if not _claimed:
		_claimed = true
		request_ownership()


func _apply_remote_state(state: Dictionary) -> void:
	if state.is_empty() or not state.get("found", false):
		return

	var owner_client_no: int = state.get("owner_client_no", 0)
	if owner_client_no != _owner_client_no:
		_on_ownership_changed(owner_client_no, _owner_client_no)

	# An unowned object keeps whatever the scene is doing to it, rather than
	# being pinned to the last owner's final pose.
	if owner_client_no == 0 or is_owned_by_me():
		return

	if state.get("has_pose", false):
		_target_transform = state.get("transform", global_transform)
		if not _has_target:
			# First snapshot: adopt it outright so the object does not slide in
			# from wherever the scene placed it.
			global_transform = _target_transform
		_has_target = true


func _on_ownership_changed(new_owner: int, previous_owner: int) -> void:
	if new_owner == _owner_client_no:
		return
	_owner_client_no = new_owner
	if is_owned_by_me():
		# Taking over: stop following the network copy of our own object.
		_has_target = false
	ownership_changed.emit(new_owner, previous_owner)
