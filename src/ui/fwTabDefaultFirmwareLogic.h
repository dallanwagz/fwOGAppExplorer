#pragma once

#include "core/fwTypes.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace fwog::detail {

/// The composite version string for the two-asset LegacyDirect entry:
/// "Main <v> / Display <v>", built from embeddedVersion() per asset rather
/// than the catalog's precomputed entry.version. Branches on which of the
/// two assets are present (main-only, display-only, both, neither) --
/// "neither" is what an entry with no uf2 assets at all produces, and
/// returns an empty string.
///
/// Factored out of fwTabDefaultFirmware.cpp, which also drags in ImGui, so
/// this pure composition logic is unit-testable without linking a graphics
/// stack. Production code (the Default Firmware tab's Original FreeWili
/// card) calls this; it is not a second public entry point.
std::string legacyVersion(const CatalogEntry& entry);

/// The exact confirmation phrase a Danger zone erase button requires:
/// "ERASE MAIN" or "ERASE DISPLAY". Exposed so the prompt the user is shown
/// and the string that is compared against cannot drift apart.
const char* erasePhrase(TargetCpu cpu);

/// True when `typed` is exactly erasePhrase(cpu). Trimmed of surrounding
/// whitespace and compared case-insensitively, same trimming rule as
/// confirmationMatches() (fwVolumeState.h) -- but a DIFFERENT phrase (two
/// words) on purpose, for both CPUs:
///
/// this gate is not the flash engine's typed-CPU-name confirmation, which a
/// user reflexively typing "MAIN" or "DISPLAY" out of habit from that other
/// prompt must NOT accidentally satisfy, and it fires before FlashDialog is
/// ever opened, for an action that destroys that CPU's firmware outright. The
/// anti-habituation is the reason the two prompts differ at all, and it is why
/// adding the DISPLAY erase did not simply reuse "DISPLAY".
bool eraseConfirmationMatches(TargetCpu cpu, std::string_view typed);

/// Whether an erase button may be enabled THIS frame, given whether that
/// erase's Danger zone CollapsingHeader is currently open and what is
/// currently in its confirmation box. Pure: it says nothing about WHY
/// `headerOpen` is what it is, which is the part of Task 22's fix round that
/// cannot be made pure -- ImGui persists a CollapsingHeader's open/closed
/// state by ID across frames independent of whether the owning tab was even
/// drawn in between, so "the header reads open" does not by itself mean "the
/// user opened it just now, in this visit." The stateful half of the fix
/// (clearing `typed`'s backing buffer whenever the header is collapsed, and
/// whenever the Default Firmware tab loses focus -- see
/// fwTabDefaultFirmware.cpp / fwApp.cpp) is what keeps a stale confirmation
/// typed on a PAST visit from ever reaching this function still armed; this
/// function only guarantees that when it does receive a stale-but-uncleared
/// `typed` together with `headerOpen == false`, the button still cannot
/// enable.
bool eraseButtonArmed(bool headerOpen, TargetCpu cpu, std::string_view typed);

/// What clicking a Default Firmware INSTALL button must do about the fact that
/// the app may not know which mounted RPI-RP2 drive is which CPU.
///
/// The state this exists for: both CPUs sitting in the RP2040 bootrom, two
/// indistinguishable RPI-RP2 volumes mounted, and every write refusing with
/// FlashOutcome::RefusedAmbiguous. That refusal is correct and is NOT relaxed
/// here. What used to follow it was a ritual -- read the refusal, click "Open
/// Recovery", find "Identify CPUs", run it, come back, click Flash again --
/// and the ritual is what this removes: the install runs the identification
/// itself, as the first part of the operation the user already asked for.
enum class AutoIdentify {
    /// Nothing is in question. Fewer than two volumes are mounted, so the
    /// engine's own existing paths (touch-and-wait, the typed confirmation for
    /// one foreign volume, or a still-fresh probe mapping) already apply.
    /// Proceed straight to the flash dialog.
    NotNeeded,
    /// Two volumes are mounted and nothing can tell them apart. Run the CPU
    /// probe now, then continue if -- and only if -- it succeeds.
    Run,
    /// Identification is needed but its preconditions do not hold, so nothing
    /// is run and nothing is flashed. See autoIdentifyBlockedReason().
    Blocked,
};

/// The decision above, from the two facts it depends on: how many RPI-RP2
/// volumes are mounted right now, and how many FreeWili boards are connected.
/// PURE.
///
/// `connectedBoards` is load-bearing rather than defensive. The probe's answer
/// is reached BY ELIMINATION across one board's two CPUs, so two boards with
/// one erased CPU each also produce two volumes, and the prober's answer would
/// then say nothing whatsoever about the other board's drive. That is the one
/// precondition the identification flow itself cannot check -- identifyCpus()
/// sees volumes and ports, not boards -- so it is checked here, the same rule
/// and the same reason as identifyDisabledReason() (fwTabRecovery.cpp).
///
/// Note what is deliberately NOT an input: whether flashing is otherwise
/// allowed. This answers only "does the app need to identify the CPUs first",
/// and the caller asks it only for a button whose flashDisabledReason() is
/// already empty. A probe can make a refusal ANSWERABLE; it must never be the
/// thing that skips one.
AutoIdentify autoIdentifyDecision(std::size_t mountedVolumes, std::size_t connectedBoards);

/// Why AutoIdentify::Blocked, in one sentence that says what is wrong and what
/// to do about it. Empty for every other decision. PURE.
std::string autoIdentifyBlockedReason(std::size_t mountedVolumes, std::size_t connectedBoards);

/// True when every CPU this entry writes to is ALREADY located, so running the
/// CPU prober would answer a question that has no question left in it. PURE.
///
/// A CPU counts as located when it is answering on a serial port (something to
/// touch) or when its bootrom drive was named by hub position (something to
/// copy to). Both are facts the current scan already produced.
///
/// This is checked BEFORE autoIdentifyDecision(), which counts mounted volumes
/// and knows nothing about who they belong to. Two mounted drives used to mean
/// "indistinguishable, run the prober" unconditionally -- but two drives on a
/// FreeWili are the board's own two CPUs on their own two hub ports, and since
/// identifyCpus() reads that, they are not indistinguishable at all. Probing
/// anyway would write a prober image to a CPU purely to re-establish what the
/// USB tree already said, which is both slow and a real write to real flash.
///
/// It deliberately does NOT consider IdentitySource::VerifiedProbe volumes: a
/// probe result is what this function decides whether to GO AND GET, and
/// treating a previous one as sufficient here would be the freshness question,
/// which belongs to verifiedVolumeFrom() and its clock rather than here.
bool locatedWithoutProbe(const CatalogEntry& entry, const CpuIdentity& identity);

} // namespace fwog::detail
