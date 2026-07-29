#pragma once

#include "core/fwTypes.h"

#include <span>
#include <string>

namespace fwog {

enum class GuardAction {
    TouchThenWait,             ///< the normal path
    Proceed,                   ///< we created this volume; copy to it
    RequireTypedConfirmation,  ///< user must type MAIN or DISPLAY
    RefuseUnidentified,        ///< no volume, no identified port
    RefuseAmbiguous,           ///< two or more volumes
    /// Wait for the volume the erase step we just completed is bringing back
    /// on this same CPU, then copy to it -- WITHOUT touching a port. There is
    /// nothing to touch: an erased RP2040 has no firmware and therefore no
    /// USB serial, and it does not need one. Returns immediately if the
    /// volume is already there; times out like any other wait if it never
    /// arrives. See VolumeState::ExpectedAfterErase.
    WaitForEraseReboot,
    /// Copy to the one mounted volume immediately: a verified CPU probe has
    /// already established that it is this step's CPU.
    ///
    /// No touch -- there is nothing running on a CPU sitting in the bootrom to
    /// touch, and it is already exactly where a touch would put it. No typed
    /// confirmation -- the confirmation asks "which CPU is this drive?", and
    /// that question has been answered by measurement rather than by asking the
    /// user to be sure. See VolumeState::MappedToTarget.
    WriteMappedVolume,
    // NOTE: an elimination arm ("this CPU is silent, the other one is running,
    // so the one mounted drive must be this one's") briefly lived here and was
    // removed rather than kept as a fallback. It reasoned only about CPUs and
    // not about BOARDS, so with two FreeWilis attached and the target CPU dead,
    // the single mounted drive it confidently claimed could belong to the other
    // board entirely. Hub position does not have that hole -- a drive is
    // enumerated under one board's own hub or it is not this board's drive --
    // and it answers the same question strictly better. See
    // CpuPortRecord::isMassStorageMain (fwTypes.h).
    /// Refuse: the one mounted volume is KNOWN to be the other CPU. Not an
    /// ambiguity and not a prompt -- a positive statement that writing here
    /// would land this image on the wrong CPU. See VolumeState::MappedToOtherCpu.
    RefuseWrongCpu,
};

/// Classify the mounted RPI-RP2 volumes for the step about to run.
///
/// `weTouched` -- we performed the 1200-baud touch that caused this volume.
/// `expectedFromPriorErase` -- the step immediately before this one, in this
/// same plan, erased THIS SAME CPU and its volume was then observed to go
/// away. Only runFlashPlan() may pass true, and only under exactly those
/// conditions; there is no other caller and no default, deliberately, so a
/// new call site has to state its answer rather than inherit one.
///
/// `identity`/`target` -- the CPU identity the step is running against and the
/// CPU it writes to. Only `identity`'s VOLUME fields are read here (see
/// CpuIdentity in fwTypes.h); the port half is the caller's business and
/// reaches this file as decideAction()'s `portIdentified`. There is no default
/// for these two either, for the same reason `expectedFromPriorErase` has none:
/// a new call site must state what it knows rather than silently inherit "know
/// nothing", which happens to be the safe answer today and would stop being one
/// the moment somebody needed it not to be.
///
/// THE VOLUME FIELDS ARE RE-VALIDATED HERE, not trusted -- but how strictly
/// depends on where they came from, which is what `mainSource`/`displaySource`
/// are read for:
///
///  - IdentitySource::VerifiedProbe is a MEASUREMENT TAKEN AT A MOMENT. It is
///    only a claim about the world while the mounted set is exactly the one
///    volume it names, so it is acted on only when `volumes` is EXACTLY that
///    one entry. A second volume appearing, the volume going away, a different
///    letter: all fall through to the states that were already there and refuse
///    or prompt exactly as before. The AGE half of that rule cannot be checked
///    without a clock and is applied by whoever fills the fields in
///    (verifiedVolumeFrom(), fwCpuProbe.h).
///
///  - IdentitySource::HubLocation is a STRUCTURAL FACT, re-derived from the
///    live USB tree on every scan: this drive is enumerated on this board's own
///    hub, at the port this CPU is soldered to. A second drive elsewhere cannot
///    falsify it, so it survives one -- which is what allows both CPUs to sit
///    in BOOTSEL at once and still be told apart. It is still checked to be
///    CURRENTLY MOUNTED before being acted on.
///
/// Consequently MappedToTarget no longer implies `volumes.size() == 1`, and a
/// caller must copy to the volume the IDENTITY names rather than to whatever
/// happens to be mounted first.
///
/// Two or more volumes still dominate everything else: an erase reboot we are
/// expecting does not make a second, unaccounted-for volume any easier to
/// tell apart from the first, and neither does a mapping -- a mapping that was
/// established against one volume says nothing whatsoever about a second one
/// that has since appeared beside it.
VolumeState classifyVolumes(std::span<const std::string> volumes, bool weTouched,
                            bool expectedFromPriorErase,
                            const CpuIdentity& identity, TargetCpu target);

/// What to do about `state`, given which CPUs are currently RUNNING FIRMWARE.
///
/// `portIdentified` -- this step's target CPU publishes a serial port.
/// `otherPortIdentified` -- the other CPU does.
///
/// Both are evidence about where the target ISN'T, and that is what makes a
/// one-click install possible in states that used to demand a ritual. A CPU
/// publishing a CDC port is running firmware and is therefore NOT any mounted
/// RPI-RP2 drive, because a drive is a CPU sitting in the bootrom. Hence:
///
///  - target port present: no mounted drive can be the target's, however many
///    are mounted. Touch the port and take the drive that APPEARS -- causation
///    identifies it, so neither a pre-existing drive nor a second board's
///    drive is an ambiguity to refuse over. This is what makes "click once and
///    it installs" true with the other CPU already sitting in BOOTSEL.
///  - target port absent, other port present, one drive: that drive is the
///    target's by elimination. See GuardAction::WriteByElimination.
///  - neither port present: nothing has been established, and the old, strict
///    answers stand unchanged.
///
/// The typed confirmation therefore survives only for the case it was actually
/// written for -- a drive whose CPU genuinely cannot be worked out -- instead
/// of firing whenever any drive happened to be mounted.
GuardAction decideAction(VolumeState state, bool portIdentified, bool otherPortIdentified);

/// True when `typed` names `cpu`. Case-insensitive, surrounding whitespace
/// ignored; nothing else is accepted.
bool confirmationMatches(std::string_view typed, TargetCpu cpu);

} // namespace fwog
