#pragma once

#include "core/fwTypes.h"
#include "flash/fwFlashEngine.h"

#include <span>
#include <string>

namespace fwog {

/// What happened when a plan was prepared -- see quietDisplayBeforeMainWrite().
enum class PrepOutcome {
    NotNeeded,      ///< the plan writes no MAIN firmware, or DISPLAY was not running
    Quieted,        ///< DISPLAY was rebooted into BOOTSEL and its drive was seen
    NotQuieted,     ///< the touch was made but no DISPLAY drive appeared in time;
                    ///< the plan goes ahead regardless (see below)
    Cancelled,      ///< io.waitTick() reported a cancel
};

struct PrepResult {
    PrepOutcome outcome = PrepOutcome::NotNeeded;
    std::string message;   ///< one line for the log, empty for NotNeeded
};

/// True when some step of `plan` INSTALLS firmware on the MAIN CPU -- a
/// StepAction::Write to TargetCpu::Main. An erase-only plan does not count:
/// see quietDisplayBeforeMainWrite() for why that distinction matters.
bool planWritesMainFirmware(std::span<const FlashStep> plan);

/// Put the DISPLAY CPU into BOOTSEL before a plan that installs MAIN firmware,
/// so that the MAIN write is stable. Reports through `progress` as
/// FlashPhase::Preparing; touches nothing when there is nothing to do.
///
/// THE HARDWARE REASON, learned on a real board and not to be argued away: a
/// running DISPLAY app churns the board's shared USB hub the instant the MAIN
/// CPU drops into BOOTSEL. It loses the inter-CPU link, bounces, and re-
/// enumerates -- and that bounce can take MAIN's RPI-RP2 drive down in the
/// middle of the copy. A torn MAIN image reboot-loops the MAIN CPU. With the
/// DISPLAY already sitting silent in its own bootrom, MAIN's drive is steady
/// for as long as the write takes.
///
/// ORDER MATTERS. The DISPLAY is quieted FIRST, while its CDC port is still
/// there to open at 1200 baud; once MAIN is in BOOTSEL the display starts
/// bouncing and the touch can no longer land. MAIN itself is left to the
/// plan's own step, which touches it in the ordinary way -- the engine's
/// delta wait identifies MAIN's drive by causation whatever else is mounted,
/// including the DISPLAY drive this just raised.
///
/// It runs ONLY for a plan that INSTALLS firmware on MAIN (planWritesMainFirmware),
/// never for an erase-only one. The stand-alone "erase MAIN CPU" entry exists
/// to silence a chattering MAIN so that the DISPLAY bootloader's console can
/// enumerate; putting the DISPLAY into BOOTSEL first would stop that console
/// from ever appearing and defeat the erase's entire purpose.
///
/// After the plan finishes, the freshly booted MAIN firmware resets the DISPLAY
/// CPU out of BOOTSEL itself (observed: a DISPLAY quieted this way came back
/// running its app once the new MAIN image was up), so nothing here needs
/// undoing.
///
/// A DISPLAY that cannot be quieted -- no port to touch, or a touch that raises
/// no drive within kVolumeWaitMs -- is NOT a failure. The plan proceeds as it
/// always did, with the write merely less protected, and the outcome says so
/// (PrepOutcome::NotQuieted) so the log records what was and was not achieved.
/// Only a cancel stops the plan.
///
/// `io.identify()` is expected to be LIVE (see makeProductionFlashIo): the
/// DISPLAY is considered quieted when a fresh identity shows a DISPLAY volume,
/// which is what lets the plan's steps then see it as a hub-located drive and
/// leave it alone.
PrepResult quietDisplayBeforeMainWrite(const FlashIo& io, std::span<const FlashStep> plan,
                                       const ProgressFn& progress);

} // namespace fwog
