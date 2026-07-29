#pragma once

#include "core/fwTypes.h"
#include "device/fwDeviceModel.h"  // DeviceView

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fwog {

/// Case-insensitive substring search across name, tagline, description,
/// author, category and every tag; narrowed further by an exact category
/// match when `category` is non-empty (empty `category` means "All").
///
/// PURE. Returns pointers into `entries` rather than copies, which is what
/// keeps this cheap enough to re-run every frame and makes the search box
/// feel instant. The caller owns `entries` and must not mutate it (no
/// push_back, erase, reallocation, or reorder of the vector) for as long as
/// the returned pointers are in use -- they alias the caller's storage, not
/// a copy of it.
std::vector<const CatalogEntry*> filterEntries(std::span<const CatalogEntry> entries,
                                               std::string_view query,
                                               std::string_view category);

/// The distinct, non-empty categories present in `entries`, sorted
/// ascending. PURE.
std::vector<std::string> categoriesOf(std::span<const CatalogEntry> entries);

/// Why the Flash button must stay disabled for `entry` given the currently
/// selected device, or an empty string when flashing is allowed.
///
/// `device` is nullptr when nothing at all is selected -- no device present,
/// or (see `identityConfirmed` below) not even a raw candidate to report on.
/// `identityConfirmed` defaults to true, matching every caller before Task
/// 22: `device` was always DeviceModel::selected()'s result, which already
/// applies its own serial check before returning anything (see
/// DeviceModel::selected()'s comment), so by the time this function saw a
/// non-null `device` its identity was already confirmed.
///
/// Task 22 fixed a regression: a board whose serial reads fwfinder's
/// "Unknown" sentinel (see serialIsUnidentified(), fwDeviceModel.h) can no
/// longer be returned by DeviceModel::selected() -- correctly, for flashing
/// purposes, since a board that will not say what it is must not be treated
/// as the confirmed selection. But a caller that collapsed that straight to
/// `device = nullptr` produced "No FreeWili is connected." while the device
/// bar still listed that exact board's row -- a false statement, and a
/// worse one than what it replaced. A caller in that situation should
/// instead pass the raw candidate (DeviceModel::selectedByIdOnly(), which
/// does not apply the serial check) as `device` and `identityConfirmed =
/// false`, so this function can still check `device->isOg` (the most
/// fundamental blocker, and still knowable) while wording the identity
/// problem accurately instead of pretending nothing is connected. `device`
/// used this way must NEVER be treated as safe to flash from -- callers
/// must gate any actual flashDialog.open()/FlashController::begin() call on
/// the CONFIRMED selection (DeviceModel::selected()), never on this raw one;
/// this function only WORDS a refusal, it does not relax one.
///
/// Returns the FIRST applicable reason, in this order: this platform cannot
/// reach the hardware at all -> no device connected -> the connected device is
/// not a FreeWili OG -> its identity could not be confirmed -> the entry has
/// no UF2 assets -> the only mounted RPI-RP2 drive is measurably the wrong CPU
/// for this entry (see mappedToTheWrongCpu below) -> (empty, flashing allowed).
/// Order matters: the message must
/// name the most fundamental blocker, not an incidental one further down the
/// list -- a user with no board plugged in should be told that, not that this
/// particular entry happens to lack firmware.
///
/// The platform check is first (Task 21) for the same reason: on the web build
/// there is never a device to find, so "No FreeWili is connected." would be
/// literally true and thoroughly misleading -- it reads as a cable fault the
/// user could go and fix. platformLimitationNotice() (fwTypes.h) says what is
/// actually going on instead.
std::string flashDisabledReason(const CatalogEntry& entry, const DeviceView* device,
                                 bool identityConfirmed = true);

/// flashDisabledReason's rule with the platform capability passed in rather
/// than read from kDeviceSupportAvailable. PURE.
///
/// flashDisabledReason() is exactly this called with kDeviceSupportAvailable.
/// The split exists so the ordering above is TESTABLE in both directions from
/// either build: kDeviceSupportAvailable is a compile-time constant, so a test
/// that could only call flashDisabledReason() would assert nothing whatsoever
/// about the other platform's behaviour -- on Windows the web branch would be
/// dead code and the case would be a test that passes by being unreachable.
/// Callers in the app should keep using flashDisabledReason(); this overload
/// is for tests and for anything that genuinely needs to reason about a
/// platform other than the one it was compiled for.
std::string flashDisabledReasonFor(bool deviceSupportAvailable,
                                    const CatalogEntry& entry, const DeviceView* device,
                                    bool identityConfirmed = true);

/// Why this entry cannot be flashed against a board whose one mounted RPI-RP2
/// drive a verified CPU probe has measured to be the CPU this entry does not
/// write to -- or empty, which is the answer whenever no such measurement
/// exists. PURE.
///
/// This is a NEW refusal, and it refuses a case that used to be permitted and
/// was dangerous. Before the probe existed, one foreign mounted volume plus a
/// DISPLAY-only entry meant the engine asked the user to type DISPLAY and then
/// wrote there -- and if that drive was in fact the MAIN CPU, the user got a
/// wrong-CPU write for correctly typing the word they were prompted for. The
/// mapping does not loosen the confirmation; it makes the question the
/// confirmation asks answerable, and here the answer is no.
///
/// It fires only when NO step of buildFlashPlan(entry) targets the CPU whose
/// drive is mounted. An entry writing both CPUs -- the deprecated-firmware
/// install -- is never refused here: its other steps have ordinary paths, and
/// the engine evaluates each step against the volumes as they are when it
/// reaches it. Blocking the whole install on one step's account would block the
/// thing this recovery flow exists to make possible.
///
/// Exposed separately from flashDisabledReason() (which calls it) because it is
/// the one rule here that depends on the BOARD's live state rather than on the
/// catalog, and it deserves to be stated -- and tested -- as its own sentence.
std::string mappedToTheWrongCpu(const CatalogEntry& entry, const CpuIdentity& identity);

/// Removes every entry whose slug is `slug` from `entries`, preserving the
/// relative order of the rest. PURE.
///
/// Exists specifically to keep a destructive, specially-confirmed catalog
/// entry (Task 22's kEraseMainCpuSlug -- see fwFlashPlan.h) out of App
/// Explorer's general browse-and-flash list, where it would sit behind
/// nothing but an ordinary "Flash" button like any other app. The entry
/// still needs to exist in the embedded catalog (so loadEmbeddedImage() and
/// embeddedVersion() can find it) and Default Firmware still finds and
/// renders it -- via its own direct embeddedEntries() lookup by slug, with
/// its own danger-styled, confirmation-gated card -- so this only trims what
/// App Explorer is handed, never the underlying catalog.
std::vector<CatalogEntry> excludeSlug(std::vector<CatalogEntry> entries, std::string_view slug);

/// Keeps only entries that are FreeWili OG APPS -- FlashScheme::OgApp, a single
/// UF2 written to the MAIN CPU -- preserving the relative order of the rest.
/// PURE.
///
/// This is the wiliOGbsp contract expressed as a filter. Under it an app is a
/// `<name>_main.uf2`: one image, MAIN CPU, with the display half riding along
/// INSIDE it rather than being flashed separately ("A display app and its main
/// companion are one deliverable -- the main UF2 carries the display image
/// inside it"). Nothing else is an app.
///
/// So the display bootloader (FlashScheme::DisplayBootloader) and the original
/// deprecated firmware (FlashScheme::LegacyDirect) are not apps and do not
/// belong in a browse-and-flash list: they are one-off board-level operations,
/// and each already has its own purpose-built card on the Default Firmware tab.
/// Listing them here offered them behind an ordinary Flash button, which for
/// the bootloader means writing to the DISPLAY CPU -- the one thing the
/// contract says never to do from an app ("Never fw flash a display
/// application. It will not boot and it takes the display CPU off USB").
///
/// NOT a replacement for excludeSlug(): the erase-MAIN entry is itself
/// FlashScheme::OgApp (it writes the CPU-agnostic flash-erase image to MAIN),
/// so it survives this filter and still has to be removed by slug.
std::vector<CatalogEntry> onlyOgApps(std::vector<CatalogEntry> entries);

/// A copy of `entry` that writes ONE CPU and performs no erase.
///
/// For the Original FreeWili firmware's per-CPU options: its full plan is
/// erase DISPLAY -> write DISPLAY -> write MAIN, and repairing one CPU should
/// not drag the other's steps along with it.
///
/// ERASES ARE DROPPED, deliberately and by owner decision. "Flash DISPLAY only"
/// writes the display image and nothing else -- it does not wipe the CPU first
/// the way the full plan does. That makes each option exactly what its label
/// says, at the cost of the two halves not reassembling into the whole; if a
/// display CPU needs wiping, the Danger zone's standalone erase is the control
/// that says so out loud.
///
/// The scheme is left ALONE. It still says LegacyDirect, which is what permits
/// either CPU (schemeAllows, fwFlashPlan.cpp) and what keeps the resulting plan
/// describing itself honestly as legacy firmware. Narrowing the assets is the
/// whole of the change; nothing here widens what may be written.
CatalogEntry restrictToCpu(const CatalogEntry& entry, TargetCpu cpu);

/// Produce a copy of `entry` retargeted to the DISPLAY CPU: `scheme` becomes
/// FlashScheme::DisplayBootloader (the only scheme buildFlashPlan permits a
/// DISPLAY-CPU asset through -- see fwFlashPlan.cpp's schemeAllows) and every
/// UF2 asset's `cpu` becomes TargetCpu::Display.
///
/// PURE: `entry` is returned unmodified via a copy; the caller's original is
/// never mutated. This is the one control in the App Explorer tab that can
/// point a main-CPU image at the DISPLAY CPU, so it is deliberately
/// extracted out of the ImGui-coupled tab code and tested directly here,
/// rather than only being reachable through widget interaction. It exists
/// only for Unlisted entries, whose scheme was inferred rather than
/// declared -- see flashDisabledReason's header comment and
/// displayRetargetConfirmed below for the gating this sits behind; a
/// described entry's scheme must never be second-guessed like this.
CatalogEntry applyDisplayRetarget(const CatalogEntry& entry);

/// True when `typed` is the exact confirmation required before
/// applyDisplayRetarget may be applied: the same typed-CPU-name check
/// (confirmationMatches, Task 6) used everywhere else in this app that
/// gates a write to an unidentified target. Named separately from a raw
/// confirmationMatches(typed, TargetCpu::Display) call so the UI's "is the
/// retarget armed" question reads -- and is tested -- as its own business
/// rule, not an easy-to-flip inline argument.
bool displayRetargetConfirmed(std::string_view typed);

} // namespace fwog
