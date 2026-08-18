#pragma once

#include "core/fwTypes.h"

#include <cstdint>
#include <optional>

namespace fwog {

/// A FRESH identification of the board at `uniqueID` -- one Fw::find_all()
/// round trip, the matching Fw::FreeWiliDevice flattened through
/// toCpuPortRecords() and identifyCpus(). Nullopt when no board is at that
/// uniqueID right now (unplugged, or the whole hub is mid-re-enumeration).
///
/// `uniqueID` is fwfinder's hub-position handle (DeviceView::uniqueID): it
/// names the SOCKET, and so survives every transition a flash puts a board
/// through -- a CPU dropping from firmware into its bootrom loses its serial
/// port and its USB serial, but not its position on the board's own hub. That
/// is what makes this the right thing to poll THROUGH a BOOTSEL transition,
/// and why it takes a uniqueID rather than a serial.
///
/// This is the live io.identify() behind every production flash (see
/// makeProductionFlashIo). It calls Fw::find_all() directly rather than
/// reading fwFinderManager's cache, because the cache is only as fresh as the
/// scanner's poll interval and a flash step needs to know where a CPU is NOW,
/// not up to two seconds ago -- touching a port the CPU already left, because
/// the cache still listed it, is a wasted touch and a thirty-second timeout.
/// Fw::find_all() keeps no global state, so calling it from a worker thread
/// while the scanner thread also calls it is safe; it costs ~160 ms.
///
/// Costs a real USB enumeration; call it when the answer matters, not per frame.
/// Nullopt on the web build, where there is no fwfinder.
std::optional<CpuIdentity> identifyBoardNow(uint64_t uniqueID);

/// `live` with the VERIFIED-PROBE volumes of `snapshot` carried over for any
/// CPU that `live` has no volume for. PURE.
///
/// A CPU probe's mapping (IdentitySource::VerifiedProbe) is a fact DeviceModel
/// folds into an identity on the UI thread; a fresh identifyBoardNow() knows
/// nothing about it. The engine re-validates a probe volume against the mounted
/// set on every use (classifyVolumes()), so carrying it forward is safe, and it
/// is what keeps a probe-established mapping usable by a live flash rather than
/// only by the snapshot the flash was started with.
CpuIdentity withProbeVolumesFrom(CpuIdentity live, const CpuIdentity& snapshot);

} // namespace fwog
