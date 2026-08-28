# SPDX-License-Identifier: Apache-2.0
@icon("res://addons/styly_netsync/icons/net_sync_avatar.svg")
class_name NetSyncAvatar
extends Node3D

## One person's avatar — either this client's (published) or a remote one
## (driven from the network).
##
## The local avatar reads its body-part transforms from the nodes you assign and
## hands them to the manager each frame. A remote avatar reads the latest pose
## for its [member client_no] and applies it to the same slots.
##
## Set [member is_local_avatar] and, for remote avatars, [member client_no].
## The [code]samples/xr[/code] scene shows the local side driven by
## [NetSyncXRAdapter]; the basic sample drives it from plain Node3Ds.

## Fires when this avatar has applied its first network pose. Useful for hiding
## a remote avatar until it has a real position instead of sitting at the origin.
signal first_pose_applied

## True for the avatar this client publishes; false for a remote peer's.
@export var is_local_avatar: bool = false

## Which remote client this avatar represents. Ignored when
## [member is_local_avatar] is true.
@export var client_no: int = 0

@export_group("Body parts")
## Head (usually the XR camera for the local avatar).
@export var head: Node3D
## Right hand or controller.
@export var right_hand: Node3D
## Left hand or controller.
@export var left_hand: Node3D
## Extra world-space transforms, synchronised in order. The order must match on
## every client.
@export var virtual_transforms: Array[Node3D] = []

@export_group("Local avatar")
## The XR rig root. Its movement relative to where it started is sent as the
## locomotion delta, which is what lets remote clients reconstruct this user's
## real-world (physical) pose. Leave unset for a non-XR client.
@export var xr_origin: Node3D

@export_group("Remote avatar")
## Smoothing half-life in seconds for received poses. 0 snaps.
@export_range(0.0, 0.5, 0.005) var smoothing_half_life: float = 0.08
## Hide this avatar until its first pose arrives.
@export var hide_until_first_pose: bool = true

var _manager: NetSyncManager = null
var _origin_start_position: Vector3 = Vector3.ZERO
var _origin_start_yaw: float = 0.0
var _has_origin_baseline: bool = false
var _has_applied_pose: bool = false


func _enter_tree() -> void:
	_try_register()


func _exit_tree() -> void:
	if _manager != null:
		_manager.unregister_pose_source(self)
		_manager = null


func _try_register() -> void:
	_manager = NetSyncManager.instance()
	if _manager != null:
		_manager.register_pose_source(self)


func _ready() -> void:
	# _enter_tree runs top-down, so a manager placed later in the scene has not
	# registered itself yet; _ready runs bottom-up, when every sibling exists.
	if _manager == null:
		_try_register()
		if _manager == null:
			push_warning(
				"[STYLY NetSync] NetSyncAvatar '%s' found no NetSyncManager in the tree, " % name
				+ "so it will neither publish nor receive a pose."
			)

	if is_local_avatar and xr_origin != null:
		# Latch the rig's starting pose: the locomotion delta is measured
		# against it, exactly as the Unity client latches its XR Origin at start.
		_origin_start_position = xr_origin.global_position
		_origin_start_yaw = xr_origin.global_rotation.y
		_has_origin_baseline = true

	if not is_local_avatar and hide_until_first_pose:
		visible = false


## Build the pose dictionary the manager sends. Called once per frame for the
## local avatar; override in a subclass if you source poses differently.
func build_pose() -> Dictionary:
	var pose := {}

	if head != null:
		pose["head"] = head.global_transform
	if right_hand != null and right_hand.visible:
		pose["right_hand"] = right_hand.global_transform
	if left_hand != null and left_hand.visible:
		pose["left_hand"] = left_hand.global_transform

	if not virtual_transforms.is_empty():
		var virtuals: Array = []
		for node in virtual_transforms:
			if node != null:
				virtuals.append(node.global_transform)
		if not virtuals.is_empty():
			pose["virtuals"] = virtuals

	if _has_origin_baseline and xr_origin != null:
		# SE(2) delta: how far the rig has moved and turned since it started.
		var yaw_delta := wrapf(xr_origin.global_rotation.y - _origin_start_yaw, -PI, PI)
		var rotated_start := _origin_start_position.rotated(Vector3.UP, yaw_delta)
		pose["xr_origin_delta_position"] = xr_origin.global_position - rotated_start
		pose["xr_origin_delta_yaw"] = rad_to_deg(yaw_delta)

	return pose


func _apply_remote_pose(pose: Dictionary) -> void:
	if pose.is_empty():
		return

	# A stealth client has no avatar to draw.
	if pose.get("is_stealth", false):
		visible = false
		return

	var weight := 1.0
	if smoothing_half_life > 0.0 and _has_applied_pose:
		var delta := get_process_delta_time()
		weight = 1.0 - pow(0.5, delta / smoothing_half_life)

	if pose.get("has_head", false) and head != null:
		_apply(head, pose["head"], weight)
	if pose.get("has_right_hand", false) and right_hand != null:
		right_hand.visible = true
		_apply(right_hand, pose["right_hand"], weight)
	elif right_hand != null:
		# Hand tracking dropped out: hide rather than freezing a stale pose.
		right_hand.visible = false
	if pose.get("has_left_hand", false) and left_hand != null:
		left_hand.visible = true
		_apply(left_hand, pose["left_hand"], weight)
	elif left_hand != null:
		left_hand.visible = false

	if pose.get("has_virtuals", false):
		var virtuals: Array = pose.get("virtuals", [])
		for i in range(min(virtuals.size(), virtual_transforms.size())):
			var node: Node3D = virtual_transforms[i]
			if node != null:
				_apply(node, virtuals[i], weight)

	if not _has_applied_pose:
		_has_applied_pose = true
		visible = true
		first_pose_applied.emit()


func _apply(node: Node3D, target: Transform3D, weight: float) -> void:
	if weight >= 1.0:
		node.global_transform = target
	else:
		node.global_transform = node.global_transform.interpolate_with(target, weight)
