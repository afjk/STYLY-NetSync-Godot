# SPDX-License-Identifier: Apache-2.0
@icon("res://addons/styly_netsync/icons/net_sync_xr_adapter.svg")
class_name NetSyncXRAdapter
extends Node

## Wires an OpenXR rig into a [NetSyncAvatar].
##
## This is a convenience only: the client core never touches XR. It finds the
## camera and controllers under an [XROrigin3D] and assigns them to the avatar's
## body-part slots, then keeps hand visibility in step with controller tracking
## so a dropped hand is reported as absent rather than frozen.
##
## Attach it as a child of (or sibling to) the avatar and point
## [member avatar] and [member xr_origin] at your rig.

## The avatar to drive. Defaults to the parent when it is a [NetSyncAvatar].
@export var avatar: NetSyncAvatar

## The rig root. Defaults to the first [XROrigin3D] found in the scene.
@export var xr_origin: XROrigin3D

## The head node. Defaults to the [XRCamera3D] under [member xr_origin].
@export var camera: XRCamera3D

## Right controller. Defaults to the [XRController3D] under [member xr_origin]
## whose tracker is `right_hand`.
@export var right_controller: XRController3D

## Left controller. Defaults to the tracker `left_hand`.
@export var left_controller: XRController3D

## Treat an untracked controller as an absent hand, so peers hide it instead of
## seeing it stuck in place.
@export var hide_untracked_hands: bool = true


func _ready() -> void:
	if avatar == null and get_parent() is NetSyncAvatar:
		avatar = get_parent()
	if avatar == null:
		push_warning(
			"[STYLY NetSync] NetSyncXRAdapter '%s' has no avatar assigned and its parent " % name
			+ "is not a NetSyncAvatar."
		)
		return

	if xr_origin == null:
		xr_origin = _find_first_xr_origin(get_tree().current_scene)
	if xr_origin == null:
		push_warning(
			"[STYLY NetSync] NetSyncXRAdapter '%s' found no XROrigin3D. Assign one, or " % name
			+ "drive the avatar's body parts yourself."
		)
		return

	if camera == null:
		camera = _find_child_of_type(xr_origin, "XRCamera3D") as XRCamera3D
	if right_controller == null:
		right_controller = _find_controller(xr_origin, "right_hand")
	if left_controller == null:
		left_controller = _find_controller(xr_origin, "left_hand")

	avatar.is_local_avatar = true
	avatar.xr_origin = xr_origin
	if camera != null:
		avatar.head = camera
	if right_controller != null:
		avatar.right_hand = right_controller
	if left_controller != null:
		avatar.left_hand = left_controller


func _process(_delta: float) -> void:
	if not hide_untracked_hands:
		return
	# NetSyncAvatar.build_pose() treats an invisible hand node as "no hand", which
	# is what the protocol's per-part valid flags express.
	if right_controller != null:
		right_controller.visible = right_controller.get_has_tracking_data()
	if left_controller != null:
		left_controller.visible = left_controller.get_has_tracking_data()


func _find_first_xr_origin(node: Node) -> XROrigin3D:
	if node == null:
		return null
	if node is XROrigin3D:
		return node
	for child in node.get_children():
		var found := _find_first_xr_origin(child)
		if found != null:
			return found
	return null


func _find_child_of_type(node: Node, type_name: String) -> Node:
	for child in node.get_children():
		if child.is_class(type_name):
			return child
		var found := _find_child_of_type(child, type_name)
		if found != null:
			return found
	return null


func _find_controller(node: Node, tracker: String) -> XRController3D:
	for child in node.get_children():
		if child is XRController3D and child.tracker == tracker:
			return child
		var found := _find_controller(child, tracker)
		if found != null:
			return found
	return null
