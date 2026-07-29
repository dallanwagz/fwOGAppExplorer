#pragma once

#include "core/fwTypes.h"

#include <span>
#include <string>
#include <vector>

namespace fwog {

/// Slug of the embedded "erase MAIN CPU" entry (firmware/manifest.json,
/// Task 22). It uses FlashScheme::OgApp -- no new scheme was added, and
/// src/core/fwTypes.h is not touched by that task -- but its description and
/// warning text must not read like an ordinary app install: writing
/// flash_nuke.uf2 destroys the MAIN CPU's firmware outright, done only to
/// silence a chattering MAIN CPU long enough for the DISPLAY bootloader's
/// USB console to enumerate (see fwTabRecovery.cpp's DisplayNotIdentified
/// section). buildFlashPlan()/planWarnings() key off this exact slug to swap
/// in that entry-specific wording instead of OgApp's generic text. Exposed
/// here, rather than repeated as a string literal, so the UI layer -- the
/// Default Firmware tab's own lookup, and the App Explorer exclusion filter
/// that keeps this destructive entry from ever appearing behind an ordinary
/// "Flash" button (see fwCatalogFilter.h's excludeSlug()) -- shares the
/// exact same string.
inline constexpr const char* kEraseMainCpuSlug = "erase-main-cpu";

/// Slug of the embedded "erase DISPLAY CPU" entry (firmware/manifest.json),
/// the sibling of kEraseMainCpuSlug above. Both write the same CPU-agnostic
/// flash-erase image; they are two entries rather than one retargetable entry
/// so that each stays pinned to exactly one CPU by the SCHEME, which no
/// catalog and no UI control can restate.
///
/// It is a FlashScheme::DisplayBootloader entry, which is what pins it: that
/// scheme permits only TargetCpu::Display (schemeAllows, fwFlashPlan.cpp), so
/// this entry can no more reach the MAIN CPU than the erase-MAIN entry can
/// reach DISPLAY. The second, independent mechanism is eraseAllowsCpu(), which
/// still refuses a DISPLAY erase to every DisplayBootloader entry EXCEPT this
/// exact slug -- so adding an erase asset to some other display entry does not
/// start working by accident, and the bootloader install cannot acquire one.
///
/// Erasing DISPLAY is destructive but RECOVERABLE, and that is why it can be
/// offered at all despite the DISPLAY CPU having no BOOTSEL button: an RP2040
/// with blank flash re-enumerates RPI-RP2 unprompted, with nothing pressed.
/// That is the same property the LegacyDirect plan's leading erase depends on.
/// The UI's typed confirmation for it is "ERASE DISPLAY", not "DISPLAY" -- see
/// eraseConfirmationMatches() (fwTabDefaultFirmwareLogic.h).
inline constexpr const char* kEraseDisplayCpuSlug = "erase-display-cpu";

/// Slug of the embedded display-bootloader install (firmware/manifest.json).
///
/// Named here for one reason: that entry is the ONLY one permitted to carry an
/// erase for a CPU its scheme does not otherwise allow -- it erases MAIN before
/// writing the DISPLAY bootloader. See eraseOnlyException() (fwFlashPlan.cpp).
/// Checked by slug rather than by scheme so that a DisplayBootloader entry
/// arriving from a remote catalog cannot acquire the same power merely by
/// listing the erase image beside its own.
inline constexpr const char* kOgBootloaderSlug = "freewili-og-bootloader";

/// The embedded id of the standard Raspberry Pi Pico flash-erase image
/// (firmware/manifest.json's "images" list). An asset carrying this id is an
/// ERASE, not an install, and buildFlashPlan() marks its step
/// StepAction::Erase.
///
/// Keyed on the IMAGE rather than on a catalog-authored flag on purpose. A
/// step being an erase changes two things -- the wording the user confirms,
/// and (via StepAction, see fwTypes.h) the engine's willingness to write to
/// the volume that CPU brings back by itself afterwards -- and neither should
/// be something a remote catalog can assert about an arbitrary image simply
/// by setting a boolean beside it. `flash_nuke.uf2` is compiled into this
/// executable; what it does is a fact about the bytes, not a claim in JSON.
///
/// It is emphatically NOT a licence to write erase images anywhere: see
/// eraseAllowsCpu() in fwFlashPlan.cpp for the separate, narrower rule
/// governing which CPUs an erase asset may reach at all.
inline constexpr const char* kFlashNukeImageId = "flash_nuke";

/// True when this asset is the flash-erase image rather than firmware.
/// Exposed because the Default Firmware tab's version line must skip erase
/// assets -- "Display pico-flash-nuke" is not a firmware version.
bool isEraseAsset(const Uf2Asset& asset);

/// Turn a catalog entry into the ordered steps that flash it.
///
/// Pure. Any asset whose CPU the scheme does not permit is dropped -- a scheme
/// decides which CPUs may be written, and a malformed catalog entry must not
/// be able to widen that. The surviving assets are then ordered by rules the
/// CODE fixes, with the catalog's `order` used only to break ties between
/// assets that are otherwise equivalent; see buildFlashPlan()'s comment in
/// fwFlashPlan.cpp for the ordering and the hardware reason behind it.
std::vector<FlashStep> buildFlashPlan(const CatalogEntry& entry);

/// Remove the steps a verified CPU probe has already made pointless, and
/// return what is left. PURE; `plan` is taken by value.
///
/// EXACTLY ONE rule, and it is narrow on purpose:
///
///   an ERASE step is dropped when a verified probe says that CPU's RPI-RP2
///   volume is mounted right now, AND a LATER step of the same plan writes to
///   that same CPU.
///
/// Both halves are load-bearing.
///
/// The first is what makes the erase pointless rather than merely inconvenient.
/// An erase's entire product is "this CPU is sitting in the RP2040's own
/// bootloader with blank flash, presenting RPI-RP2" -- that is why the
/// deprecated-firmware plan leads with one, since the DISPLAY CPU has no
/// BOOTSEL button and no other way to get there. A verified probe naming that
/// CPU's mounted volume is a MEASUREMENT that it is already in exactly that
/// state. Writing flash_nuke.uf2 to it would spend a reboot cycle to reach the
/// state it is being observed in, and worse: the reboot changes the mounted
/// volume set, which VOIDS the very mapping that made the write possible (see
/// classifyVolumes()). The redundant step is not just wasted, it is what would
/// stop the rest of the plan.
///
/// The second is what stops this from ever eating a step whose erasing IS the
/// point. The `erase-main-cpu` entry (kEraseMainCpuSlug) is a single ERASE MAIN
/// step with nothing after it: a MAIN CPU held in BOOTSEL by its button still
/// has its firmware, erasing it is exactly what the user asked for, and this
/// leaves it alone because no later step writes MAIN. The `erase-display-cpu`
/// entry (kEraseDisplayCpuSlug) is the same shape and is protected by the same
/// half of the rule -- a DISPLAY CPU whose volume a probe has named still has
/// its firmware, and nothing follows the erase to consume the erased state.
/// Only an erase that exists
/// to PREPARE a following write on the same CPU can be redundant, because only
/// that erase's product is a state something else is about to consume.
///
/// Deliberately separate from buildFlashPlan() rather than folded into it.
/// buildFlashPlan owns the ORDER, which is a fact about the hardware that no
/// catalog and no board state may reorder; this owns REDUNDANCY, which is a
/// fact about the board in front of the user right now. Keeping buildFlashPlan
/// pure over the entry alone is what keeps that ordering property statable and
/// tested on its own.
std::vector<FlashStep> dropRedundantErases(std::vector<FlashStep> plan, const CpuIdentity& identity);

/// The one sentence explaining what dropRedundantErases() removed from
/// `fullPlan` and why, or empty when it removed nothing. PURE.
///
/// `fullPlan` is the plan BEFORE the drop -- buildFlashPlan()'s own output --
/// because a note about a step can only be written while the step is still
/// there to describe. Shown next to the plan preview so that a plan which is
/// visibly one step shorter than the documentation says never looks like
/// something quietly went missing.
std::string droppedEraseNote(std::span<const FlashStep> fullPlan, const CpuIdentity& identity);

/// Advisory warnings shown before the user commits. These are warnings, not
/// refusals: the app cannot prove a bootloader's absence without interrogating
/// the board, and a false refusal is worse than a false warning.
///
/// `identity` is read for one thing only: whether dropRedundantErases() will
/// remove this entry's erase step. A warning that says "the DISPLAY CPU is
/// ERASED first" in front of a plan that contains no erase is not a harmless
/// over-warning -- it is the app describing an action it is not going to take,
/// on the one screen where the user is deciding whether to consent to it. Pass
/// a default-constructed CpuIdentity for "nothing is known about the board",
/// which yields exactly the warnings this returned before probe mappings
/// existed. There is no default parameter, deliberately: see classifyVolumes()
/// (fwVolumeState.h) for why a call site must state what it knows.
std::vector<std::string> planWarnings(const CatalogEntry& entry, const CpuIdentity& identity);

} // namespace fwog
