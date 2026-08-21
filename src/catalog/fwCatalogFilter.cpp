#include "catalog/fwCatalogFilter.h"

#include "flash/fwFlashPlan.h"    // buildFlashPlan
#include "flash/fwVolumeState.h"  // confirmationMatches
#include "ui/fwRecoveryContent.h"  // kBoardSerialUnidentifiedTitle -- see its own comment for why this include is fine

#include <algorithm>
#include <cctype>
#include <set>

namespace fwog {
namespace {

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// `needleLower` is already lowercased by the caller so this can be called
/// once per candidate field without re-lowercasing the query every time.
bool containsLower(std::string_view haystack, const std::string& needleLower)
{
    return lower(haystack).find(needleLower) != std::string::npos;
}

bool matchesQuery(const CatalogEntry& e, const std::string& needleLower)
{
    if (needleLower.empty()) return true;
    if (containsLower(e.name, needleLower))        return true;
    if (containsLower(e.tagline, needleLower))      return true;
    if (containsLower(e.description, needleLower))  return true;
    if (containsLower(e.author, needleLower))       return true;
    if (containsLower(e.category, needleLower))     return true;
    for (const auto& tag : e.tags)
        if (containsLower(tag, needleLower)) return true;
    return false;
}

} // namespace

std::vector<const CatalogEntry*> filterEntries(std::span<const CatalogEntry> entries,
                                               std::string_view query,
                                               std::string_view category)
{
    const std::string needleLower = lower(query);

    std::vector<const CatalogEntry*> out;
    for (const auto& e : entries) {
        if (!category.empty() && e.category != category) continue;
        if (!matchesQuery(e, needleLower)) continue;
        out.push_back(&e);
    }
    return out;
}

std::vector<std::string> categoriesOf(std::span<const CatalogEntry> entries)
{
    std::set<std::string> unique;
    for (const auto& e : entries)
        if (!e.category.empty()) unique.insert(e.category);
    return std::vector<std::string>(unique.begin(), unique.end());
}

std::string flashDisabledReasonFor(bool deviceSupportAvailable,
                                    const CatalogEntry& entry, const DeviceView* device,
                                    bool identityConfirmed,
                                    bool serialSupportAvailable)
{
    // FIRST, ahead of the no-device check (Task 21). On a platform that
    // cannot reach USB at all there is never a device to find, so "No
    // FreeWili is connected." would be true and useless -- it describes a
    // cable the user could go and re-seat. Name the real blocker instead.
    if (!deviceSupportAvailable)
        return std::string(platformLimitationNotice());
    // Ahead of the device checks for the same reason the platform gate is:
    // on a serial-less platform (iPadOS) a DISPLAY-reaching plan is
    // impossible no matter what is plugged in, and the DISPLAY CPU has no
    // BOOTSEL button a user could press instead. Refusing here is what stops
    // the engine from erasing MAIN and then waiting forever for a DISPLAY
    // drive that can never appear.
    if (!serialSupportAvailable) {
        const auto plan = buildFlashPlan(entry);
        const bool touchesDisplay = std::any_of(plan.begin(), plan.end(),
            [](const FlashStep& s) { return s.cpu == TargetCpu::Display; });
        if (touchesDisplay)
            // Claims only what THIS app cannot do. The DISPLAY CPU's BOOTSEL
            // pad can still be shorted by hand -- the Recovery tab documents
            // it -- so "only the desktop app can reboot it" would be false.
            return "This install writes to the DISPLAY CPU, which this app "
                   "cannot put into its bootloader -- it has no BOOTSEL button, "
                   "and without serial access the app cannot reboot it at 1200 "
                   "baud. Use the desktop app for this one.";
    }
    if (device == nullptr)
        return "No FreeWili is connected.";
    if (!device->isOg)
        return "The selected device is not a FreeWili OG -- this app cannot flash it.";
    if (!identityConfirmed) {
        // See this function's header comment: `device` here is a raw
        // candidate (DeviceModel::selectedByIdOnly()), not the confirmed
        // selection -- most often because its serial reads fwfinder's
        // "Unknown" sentinel. Say so accurately rather than falling back to
        // "No FreeWili is connected.", which is what a null `device` would
        // have produced and which is simply false when the device bar is
        // showing this exact board's row.
        return "A FreeWili is connected, but it is not the board that was selected -- what it "
               "reports about itself contradicts the selection. Click its row in the device bar "
               "to select it. See Recovery: \"" + std::string(kBoardSerialUnidentifiedTitle) + "\".";
    }
    if (entry.uf2.empty())
        return "This entry has no firmware to flash.";
    if (std::string wrongCpu = mappedToTheWrongCpu(entry, device->identity); !wrongCpu.empty())
        return wrongCpu;
    return {};
}

std::string mappedToTheWrongCpu(const CatalogEntry& entry, const CpuIdentity& identity)
{
    // Only a probe-verified volume can produce a refusal here. The volume
    // fields are set by withVerifiedVolume() (fwDeviceModel.h) from a mapping
    // that verifiedVolumeFrom() has already found FRESH -- volume set unchanged
    // and inside kMaxMappingAge -- so an empty identity, which is what every
    // caller has until somebody runs the Recovery tab's identification, falls
    // straight through and this function has no opinion at all.
    const std::string* volume = nullptr;
    TargetCpu mountedCpu = TargetCpu::Main;
    if (identity.mainVolume)         { volume = &*identity.mainVolume;    mountedCpu = TargetCpu::Main; }
    else if (identity.displayVolume) { volume = &*identity.displayVolume; mountedCpu = TargetCpu::Display; }
    if (!volume) return {};

    // The plan, not the raw asset list: schemeAllows() drops assets a scheme
    // does not permit, so an entry that lists a DISPLAY asset under
    // FlashScheme::OgApp writes only MAIN and must be judged on that.
    const auto plan = buildFlashPlan(entry);
    if (plan.empty()) return {};   // "no firmware to flash" is the caller's earlier, better answer

    // A drive belonging to the other CPU only blocks anything when this plan
    // has nowhere else to go. Every step whose own CPU is running -- i.e. has a
    // port to touch -- can reach its CPU regardless of what else is mounted:
    // the engine touches that port and takes the drive the touch raises (see
    // decideAction, fwVolumeState.h). Refusing here anyway would grey out the
    // install for the sole reason that the OTHER CPU is sitting in BOOTSEL,
    // which is the ordinary state a user opens this app in.
    const auto stepIsReachable = [&identity](const FlashStep& s) {
        return s.cpu == TargetCpu::Main ? identity.mainPort.has_value()
                                        : identity.displayPort.has_value();
    };
    if (std::all_of(plan.begin(), plan.end(), stepIsReachable)) return {};

    for (const auto& step : plan)
        if (step.cpu == mountedCpu)
            // At least one step goes where the drive actually is. That is
            // enough: a plan touching BOTH CPUs -- the deprecated-firmware
            // install -- is not refused here, because its other steps have a
            // perfectly ordinary path (touch a port, or wait for a reboot this
            // plan itself caused) and the engine judges each step on its own
            // when it gets there. Refusing the whole entry on one step's
            // account would block the install this recovery flow exists to
            // make possible.
            return {};

    const char* mounted = mountedCpu == TargetCpu::Main ? "MAIN" : "DISPLAY";
    const char* wanted  = mountedCpu == TargetCpu::Main ? "DISPLAY" : "MAIN";
    const IdentitySource src = mountedCpu == TargetCpu::Main ? identity.mainSource
                                                             : identity.displaySource;
    // The evidence is named rather than assumed to be the probe: a bootrom
    // drive is now identified by hub position in the ordinary case, and telling
    // the user to consult a probe they never ran would send them looking for
    // something that does not exist.
    const std::string how = src == IdentitySource::VerifiedProbe
        ? " -- the Recovery tab's CPU probe measured that"
        : " -- it is on that CPU's port of the board's internal USB hub";
    return "The one RPI-RP2 drive mounted (" + *volume + ") is the " + mounted +
           " CPU" + how + ". This writes only to the " +
           wanted + " CPU, and that CPU is not reachable right now, so there is "
           "nowhere here for it to go. Flash the " + mounted +
           " CPU first, or unmount that drive.";
}

std::string flashDisabledReason(const CatalogEntry& entry, const DeviceView* device,
                                 bool identityConfirmed)
{
    return flashDisabledReasonFor(kDeviceSupportAvailable, entry, device, identityConfirmed,
                                  kSerialSupportAvailable);
}

std::vector<CatalogEntry> excludeSlug(std::vector<CatalogEntry> entries, std::string_view slug)
{
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const CatalogEntry& e) { return e.slug == slug; }),
                 entries.end());
    return entries;
}

std::vector<CatalogEntry> onlyOgApps(std::vector<CatalogEntry> entries)
{
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const CatalogEntry& e) { return e.scheme != FlashScheme::OgApp; }),
                 entries.end());
    return entries;
}

CatalogEntry restrictToCpu(const CatalogEntry& entry, TargetCpu cpu)
{
    CatalogEntry out = entry;   // copy: the caller's entry is never mutated
    std::erase_if(out.uf2, [cpu](const Uf2Asset& a) {
        return a.cpu != cpu || isEraseAsset(a);
    });
    return out;
}

CatalogEntry applyDisplayRetarget(const CatalogEntry& entry)
{
    CatalogEntry retargeted = entry;   // copy: the caller's entry is never mutated
    retargeted.scheme = FlashScheme::DisplayBootloader;
    for (auto& asset : retargeted.uf2)
        asset.cpu = TargetCpu::Display;
    return retargeted;
}

bool displayRetargetConfirmed(std::string_view typed)
{
    return confirmationMatches(typed, TargetCpu::Display);
}

} // namespace fwog
