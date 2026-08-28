// SPDX-License-Identifier: Apache-2.0
// GDExtension entry point: registers the native classes with Godot.

#include <gdextension_interface.h>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "godot/netsync_bridge.hpp"

using namespace godot;

namespace {

void initialize_styly_netsync(ModuleInitializationLevel level) {
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    GDREGISTER_CLASS(styly::netsync::NetSyncBridge);
}

void uninitialize_styly_netsync(ModuleInitializationLevel level) {
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
}

}  // namespace

extern "C" {

GDExtensionBool GDE_EXPORT styly_netsync_library_init(
    GDExtensionInterfaceGetProcAddress p_get_proc_address,
    const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
    GDExtensionBinding::InitObject init_object(p_get_proc_address, p_library, r_initialization);
    init_object.register_initializer(initialize_styly_netsync);
    init_object.register_terminator(uninitialize_styly_netsync);
    init_object.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_object.init();
}

}  // extern "C"
