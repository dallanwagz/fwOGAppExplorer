#pragma once

// iOS only: the folder-grant that stands in for volume discovery. iPadOS has
// no mount table an app may scan; what it has is a user-granted folder,
// remembered as a security-scoped bookmark that survives the board being
// replugged (measured on the device -- see the Pocket Wili scope's spike).
//
// Declared unconditionally so callers can include this without ceremony;
// defined only in fwVolumeGrant.mm, which only the iOS build compiles.

#include <optional>
#include <string>

namespace fwog {
namespace grant {

/// True once the user has ever granted a folder (the bookmark exists),
/// whether or not the drive is currently plugged in.
bool hasGrant();

/// The granted folder's path, with its security scope opened and held for
/// the life of the process -- so plain POSIX/std::filesystem calls through
/// the returned path work from any thread. nullopt when there is no grant or
/// the drive is not currently mounted (unplugged, or a dead bookmark).
///
/// NO verification here: whether the folder is a real RP2040 bootrom volume
/// is findRpiRp2Volumes()'s question, answered the same way as every other
/// platform -- by reading INFO_UF2.TXT.
std::optional<std::string> grantedVolumePath();

/// Present the system folder picker (main thread, over the SDL window's view
/// controller). Saves the bookmark on success; returns immediately -- the
/// grant shows up on a later grantedVolumePath() poll, which is how the
/// device scan already works.
void presentGrantPicker();

/// Drop the stored bookmark (Settings affordance; also the recovery from a
/// grant to the wrong folder).
void clearGrant();

/// One live sentence naming exactly which stage of the grant currently
/// holds: no grant / bookmark no longer resolves / scope refused / resolved
/// but unmounted / mounted at <path>. Diagnostic surface for the device bar
/// -- the failure modes differ in remedy (re-grant vs. plug the board in)
/// and are invisible from the outside without this.
std::string statusDescription();

} // namespace grant
} // namespace fwog
