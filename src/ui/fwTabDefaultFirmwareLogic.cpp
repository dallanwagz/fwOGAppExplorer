#include "ui/fwTabDefaultFirmwareLogic.h"

#include "catalog/fwCatalogEmbedded.h"
#include "flash/fwFlashPlan.h"   // isEraseAsset

#include <algorithm>
#include <cctype>

namespace fwog::detail {

std::string legacyVersion(const CatalogEntry& entry)
{
    std::string mainId, displayId;
    for (const auto& a : entry.uf2) {
        // The LegacyDirect entry now also carries the flash-erase image, as
        // its DISPLAY-erase step. That image has a "version"
        // ("pico-flash-nuke"), but it is not a FIRMWARE version and must never
        // land in this line -- "Display pico-flash-nuke" would name the wrong
        // thing, and which asset won would depend on manifest array order.
        if (isEraseAsset(a)) continue;
        if (a.cpu == TargetCpu::Main) mainId = a.ref.embeddedId;
        else                          displayId = a.ref.embeddedId;
    }

    const std::string mainVer    = mainId.empty()    ? std::string() : embeddedVersion(mainId);
    const std::string displayVer = displayId.empty() ? std::string() : embeddedVersion(displayId);

    std::string result;
    if (!mainVer.empty()) result += "Main " + mainVer;
    if (!displayVer.empty()) {
        if (!result.empty()) result += " / ";
        result += "Display " + displayVer;
    }
    return result;
}

const char* erasePhrase(TargetCpu cpu)
{
    return cpu == TargetCpu::Main ? "ERASE MAIN" : "ERASE DISPLAY";
}

bool eraseConfirmationMatches(TargetCpu cpu, std::string_view typed)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    auto first = std::find_if(typed.begin(), typed.end(), notSpace);
    auto last  = std::find_if(typed.rbegin(), typed.rend(), notSpace).base();
    if (first >= last) return false;

    std::string t(first, last);
    for (char& c : t) c = char(std::toupper(static_cast<unsigned char>(c)));

    return t == erasePhrase(cpu);
}

bool eraseButtonArmed(bool headerOpen, TargetCpu cpu, std::string_view typed)
{
    return headerOpen && eraseConfirmationMatches(cpu, typed);
}

AutoIdentify autoIdentifyDecision(std::size_t mountedVolumes, std::size_t connectedBoards)
{
    // Exactly two, never "two or more". Three or more RPI-RP2 volumes cannot
    // come from one two-CPU board at all, and the by-elimination step -- "the
    // one that is left" -- would be a guess rather than a conclusion. That is
    // identifyCpus()'s own NotExactlyTwoVolumes precondition, stated here so
    // the button never starts a run that is going to refuse on arrival.
    if (mountedVolumes != 2) return AutoIdentify::NotNeeded;
    if (connectedBoards > 1) return AutoIdentify::Blocked;
    return AutoIdentify::Run;
}

std::string autoIdentifyBlockedReason(std::size_t mountedVolumes, std::size_t connectedBoards)
{
    if (autoIdentifyDecision(mountedVolumes, connectedBoards) != AutoIdentify::Blocked)
        return {};
    // Says what is wrong and what to do, in one line. The full explanation of
    // why a second board voids the answer lives in the Recovery tab's "Two
    // RPI-RP2 drives are mounted" section; this is the doorway to it, not a
    // second copy of it.
    return "Two RPI-RP2 drives are mounted and more than one FreeWili is connected, so "
           "which drive is which cannot be established. Unplug all but the board you are "
           "flashing.";
}

bool locatedWithoutProbe(const CatalogEntry& entry, const CpuIdentity& identity)
{
    const auto plan = buildFlashPlan(entry);
    // An entry with nothing to flash is not "located"; it has no steps whose
    // CPUs could be. Saying true here would let a click sail past this check
    // into a dialog for a plan that cannot run.
    if (plan.empty()) return false;

    const auto located = [&identity](TargetCpu cpu) {
        const bool hasPort = cpu == TargetCpu::Main ? identity.mainPort.has_value()
                                                     : identity.displayPort.has_value();
        if (hasPort) return true;
        const auto& vol = cpu == TargetCpu::Main ? identity.mainVolume : identity.displayVolume;
        const auto  src = cpu == TargetCpu::Main ? identity.mainSource : identity.displaySource;
        return vol.has_value() && src == IdentitySource::HubLocation;
    };
    return std::all_of(plan.begin(), plan.end(),
                       [&](const FlashStep& s) { return located(s.cpu); });
}

} // namespace fwog::detail
