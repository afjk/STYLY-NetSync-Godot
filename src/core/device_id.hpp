// SPDX-License-Identifier: Apache-2.0
// Stable per-installation device id.
//
// The protocol treats deviceId as the client's durable identity: the server
// keys its room entries on it, reuses the same client number for a returning
// device, and refreshes a stale control-lane identity when one reconnects. So
// the value must survive a process restart — including on Android.
//
// This implementation does not depend on STYLY's Device-ID-Provider. The value
// is either supplied by the application or generated once and persisted; see
// docs/UPSTREAM_COMPATIBILITY.md for what that means for a device that also
// runs the Unity client.
#pragma once

#include <string>

namespace styly {
namespace netsync {

/// Generate a random RFC 4122 version 4 UUID in lowercase hyphenated form.
std::string generate_uuid_v4();

/// Load the persisted device id from `path`, generating and storing one when the
/// file is missing, empty or unreadable. Returns an empty string only when the
/// value could neither be read nor written *and* generation failed, which cannot
/// happen in practice; a generated-but-unpersisted id is still returned so the
/// session works (it will simply differ next launch).
///
/// `path` must be somewhere writable and durable. The Godot layer passes a file
/// under `user://`, which maps to app-private storage on Android.
std::string load_or_create_device_id(const std::string &path);

/// Read a persisted device id without creating one. Empty when absent.
std::string read_device_id(const std::string &path);

/// Persist `device_id` to `path`. Returns false when the write failed.
bool write_device_id(const std::string &path, const std::string &device_id);

}  // namespace netsync
}  // namespace styly
