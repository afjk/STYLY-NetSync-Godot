# SPDX-License-Identifier: Apache-2.0
@tool
extends EditorPlugin

## Editor plugin for STYLY NetSync.
##
## The runtime classes each declare `class_name` and `@icon`, so Godot already
## registers them as global classes and shows them in the "Create New Node"
## dialog with their icons — whether or not this plugin is enabled. Calling
## `add_custom_type()` for those same names would register them twice and is a
## genuine conflict, so this plugin deliberately does not.
##
## What it is for: telling you, in the editor, when the native half of the addon
## is missing. Without it every NetSync script fails to compile with a confusing
## "Could not find type NetSyncBridge", and it is not obvious that the fix is to
## build the extension.

func _enter_tree() -> void:
	if ClassDB.class_exists("NetSyncBridge"):
		return

	push_warning(
		"[STYLY NetSync] The native extension is not loaded.\n"
		+ "  • If you have just cloned or just built the project, close and reopen the "
		+ "editor once: Godot only picks up a .gdextension after the project scan that "
		+ "first discovers it.\n"
		+ "  • Otherwise build it with `scons target=template_debug` from the repository "
		+ "root, then reopen. See docs/BUILD.md."
	)
