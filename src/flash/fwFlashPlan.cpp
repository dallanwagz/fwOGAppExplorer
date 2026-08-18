#include "flash/fwFlashPlan.h"

#include <algorithm>

namespace fwog {
namespace {

/// Which CPUs a scheme is permitted to write. Everything else is dropped.
///
/// UNCHANGED by the DISPLAY-first reordering, and it must stay that way:
///
///  - OgApp permits ONLY Main. That is the layer that keeps ordinary app
///    images -- which drive GPIO 29 against the PDM microphone's own output
///    if they ever run on the DISPLAY CPU -- off that CPU entirely. The
///    `erase-main-cpu` entry is an OgApp entry and is MAIN-only because of
///    this line; nothing below makes it retargetable.
///  - LegacyDirect already permitted both CPUs, because it has always written
///    a display image to the DISPLAY CPU. The new erase-DISPLAY step needs no
///    widening here at all -- it is a write to a CPU this scheme could already
///    write to. There is no "erase images may target any CPU" rule anywhere,
///    and eraseAllowsCpu() below exists specifically to make sure one cannot
///    be introduced by accident.
///  - DisplayBootloader permits ONLY Display, and that is what pins the
///    standalone `erase-display-cpu` entry (kEraseDisplayCpuSlug) to the
///    DISPLAY CPU as firmly as OgApp pins `erase-main-cpu` to MAIN. Neither
///    entry can be retargeted: rewriting its asset's `cpu` produces an EMPTY
///    plan, not an erase of the other CPU.
///
/// The other half of the DISPLAY-write story lives in the UI: the App
/// Explorer's display-retarget control is gated on
/// `Unlisted && schemeInferred` (fwTabAppExplorer.cpp), so a catalog entry
/// that declares a scheme can never be pointed at the DISPLAY CPU by hand.
bool schemeAllows(FlashScheme scheme, TargetCpu cpu)
{
    switch (scheme) {
    case FlashScheme::OgApp:             return cpu == TargetCpu::Main;
    case FlashScheme::DisplayBootloader: return cpu == TargetCpu::Display;
    case FlashScheme::LegacyDirect:      return true;
    }
    return false;
}

/// An EXTRA restriction applied to the flash-erase image on top of
/// schemeAllows(), never a relaxation of it. Both must pass.
///
/// Erasing the DISPLAY CPU is safe in itself -- flash_nuke.uf2 only wipes
/// flash and drops into the bootrom, and never drives GPIO 29, so the PDM
/// microphone hazard that makes a main APPLICATION image dangerous on that
/// CPU does not apply to it -- and it is what makes the DISPLAY CPU
/// recoverable without a BOOTSEL button, since an RP2040 with blank flash
/// enumerates RPI-RP2 unprompted. But "safe" is not "unrestricted": it is
/// permitted on DISPLAY for exactly two entries, and both are named here.
///
///  - Any LegacyDirect plan, which goes on to write a display image
///    immediately afterwards. Unchanged.
///  - The `erase-display-cpu` entry (kEraseDisplayCpuSlug) BY SLUG, and only
///    by slug. That entry is the deliberate standalone DISPLAY erase the
///    Default Firmware tab's Danger zone offers, gated behind its own typed
///    "ERASE DISPLAY" confirmation before the flash dialog is ever opened.
///
/// The slug is checked rather than the scheme being widened, and that is the
/// whole point: FlashScheme::DisplayBootloader must keep meaning "install the
/// display bootloader", so the bootloader-install entry -- and any future
/// display entry, local or remote -- still cannot acquire an erase step merely
/// by listing the erase image beside its own. Adding a second standalone erase
/// stays a deliberate edit here.
bool eraseAllowsCpu(const CatalogEntry& entry, TargetCpu cpu)
{
    // MAIN always: it has a reachable BOOTSEL button, so an erase there is
    // recoverable by hand no matter what else is true. This is the
    // `erase-main-cpu` entry's path.
    if (cpu == TargetCpu::Main) return true;
    if (entry.scheme == FlashScheme::LegacyDirect) return true;
    return entry.slug == kEraseDisplayCpuSlug;
}

/// The ONE case where an asset reaches a CPU its scheme does not permit: the
/// display-bootloader install erasing the MAIN CPU.
///
/// Why it exists: installing the bootloader means writing the DISPLAY CPU, and
/// a MAIN CPU running an app talks continuously on the inter-CPU link -- the
/// same chatter that makes the DISPLAY bootloader console wait for roughly ten
/// seconds of MAIN silence (see cpuRank()). Erasing MAIN first makes it silent
/// for the write that follows, and leaves a board whose next step is obviously
/// "flash an app", which is the state the bootloader exists to enable.
///
/// Why it is expressed as a NARROW EXCEPTION rather than by widening
/// schemeAllows(): FlashScheme::DisplayBootloader must keep meaning "writes the
/// DISPLAY CPU". Widening it to include MAIN would let ANY DisplayBootloader
/// entry -- including one arriving from a remote catalog -- carry a main-CPU
/// APPLICATION image and have it written, which is the wrong-CPU write this
/// project exists to prevent. This permits an ERASE and nothing else, and only
/// for an entry whose scheme is DisplayBootloader.
///
/// The erase image is CPU-agnostic (flash_nuke wipes flash and drops into the
/// bootrom; it drives no GPIO), so erasing MAIN with it is the same operation
/// the `erase-main-cpu` entry already performs, and MAIN has a reachable
/// BOOTSEL button if anything goes wrong.
///
/// AND IT IS KEYED ON THE SLUG, not merely on the scheme. Without that, ANY
/// DisplayBootloader entry -- a remote catalog's included -- could erase the
/// MAIN CPU just by listing the erase image, and a test that pins exactly this
/// ("the real erase-display-cpu entry is DISPLAY-only and stays DISPLAY-only if
/// retargeted", test_fwFlashPlan.cpp) caught it doing so. Same reasoning, and
/// the same remedy, as eraseAllowsCpu()'s DISPLAY slug check below.
bool eraseOnlyException(const CatalogEntry& entry, const Uf2Asset& asset)
{
    return entry.slug == kOgBootloaderSlug
        && entry.scheme == FlashScheme::DisplayBootloader
        && asset.cpu == TargetCpu::Main
        && isEraseAsset(asset);
}

/// Sort rank for the CPU a step targets: DISPLAY before MAIN.
///
/// THIS ORDER WAS DELIBERATELY INVERTED, and the comment that used to argue
/// for MAIN-first is gone because hardware disproved it, not because someone
/// preferred the other way round. Do not "restore" MAIN-first.
///
/// What happened: the DISPLAY bootloader's USB console only enumerates after
/// roughly TEN SECONDS OF MAIN-CPU SILENCE. On a real LegacyDirect run with
/// the old [MAIN, DISPLAY] plan, step 1 wrote MAIN successfully; MAIN then
/// began running the freshly written legacy firmware, which chatters on the
/// inter-CPU link continuously; the DISPLAY console consequently never came
/// up; step 2 waited the full thirty seconds for an RPI-RP2 volume, timed out,
/// and the display image was never written. MAIN-first does not merely risk
/// the DISPLAY step -- it makes it unreachable, and it strands the board in a
/// mixed state every single time.
///
/// So DISPLAY is dealt with FIRST, while MAIN is still quiet, and MAIN -- the
/// one CPU with a reachable BOOTSEL button, and therefore the one that is
/// always recoverable by hand -- goes last. The old comment's worry (a failed
/// second step leaving stale firmware on the CPU with no BOOTSEL button) is
/// answered by the erase that now leads the plan: an RP2040 with blank flash
/// re-enumerates RPI-RP2 by itself, so a plan that stops after the erase
/// leaves the DISPLAY CPU sitting in its own bootloader, writable, rather
/// than stranded. That is the same reasoning the erase-MAIN-then-install-
/// bootloader procedure already rests on.
int cpuRank(TargetCpu cpu) { return cpu == TargetCpu::Display ? 0 : 1; }

/// Sort rank within one CPU: erase before write. Structural for the same
/// reason cpuRank is -- writing an image and then erasing it is never what
/// anyone meant, and `order` comes from catalog JSON.
int actionRank(const Uf2Asset& a) { return isEraseAsset(a) ? 0 : 1; }

std::string describe(const CatalogEntry& entry, const Uf2Asset& asset)
{
    // The erase test comes FIRST, ahead of every scheme. It used to sit below
    // the DisplayBootloader branch, which was harmless only while no
    // DisplayBootloader entry could carry an erase asset -- the
    // `erase-display-cpu` entry (kEraseDisplayCpuSlug) is one, and under the
    // old order its single step would have been announced as "Install the
    // display serial bootloader" in the list the user reads before consenting
    // to it. An erase says it is an erase whatever scheme carries it.
    //
    // Takes the ASSET, not just the CPU. It used to distinguish the erase case
    // by `entry.slug == kEraseMainCpuSlug`, which was sufficient only while
    // erasing was something a whole entry did; the LegacyDirect entry now
    // carries an erase asset AND a write asset for the SAME CPU, so the slug
    // (and the CPU) can no longer tell the two steps apart. Guessing from the
    // CPU would have mislabelled one of them, in the one list the user reads
    // before consenting to the whole thing.
    if (isEraseAsset(asset)) {
        // See kEraseMainCpuSlug's comment (fwFlashPlan.h): an erase must say
        // so in the plan itself, not just in a warning below it.
        if (asset.cpu == TargetCpu::Main)
            return "Erase the MAIN CPU -- write the standard Pico flash-erase image, "
                   "destroying its current firmware";
        return "Erase the DISPLAY CPU -- write the standard Pico flash-erase image, "
               "destroying its current firmware and leaving it in its own bootloader";
    }

    if (entry.scheme == FlashScheme::DisplayBootloader)
        return "Install the display serial bootloader on the DISPLAY CPU";

    if (asset.cpu == TargetCpu::Main)
        return "Write the application image to the MAIN CPU";
    return "Write the display image to the DISPLAY CPU";
}

} // namespace

bool isEraseAsset(const Uf2Asset& asset)
{
    return asset.ref.embeddedId == kFlashNukeImageId;
}

std::vector<FlashStep> buildFlashPlan(const CatalogEntry& entry)
{
    std::vector<Uf2Asset> assets;
    for (const auto& a : entry.uf2)
        if ((schemeAllows(entry.scheme, a.cpu) || eraseOnlyException(entry, a)) &&
            (!isEraseAsset(a) || eraseAllowsCpu(entry, a.cpu)))
            assets.push_back(a);

    // Both ranks dominate `order`, which is authored in catalog JSON (it comes
    // from array position). Letting `order` decide either of them would let a
    // catalog entry -- hostile, or merely careless about the order it happened
    // to list its assets in -- reorder a sequence whose correctness is a fact
    // about the hardware, not a preference. `order` still breaks ties between
    // assets that agree on both, which is how several images for one CPU stay
    // sequenced.
    //
    // ERASES COME FIRST, GLOBALLY -- ahead of the CPU rank, not inside it.
    //
    // This ordering was changed when the bootloader install gained its MAIN
    // erase (see eraseOnlyException). The rule is "silence the CPU you are not
    // writing, before you write the one you are": an erase exists to stop a CPU
    // doing anything, and every reason to erase is a reason to have done it
    // already by the time the writes start.
    //
    // IT CHANGES NO EXISTING PLAN, which is why it is safe to make the primary
    // key. The deprecated-firmware entry's only erase is on DISPLAY, which the
    // CPU rank already put first, so it still produces:
    //     1. ERASE  DISPLAY (flash_nuke.uf2)
    //     2. WRITE  DISPLAY (FreeWiliDisplayV67.uf2)
    //     3. WRITE  MAIN    (FreeWiliMainV92.uf2)
    // and the two standalone erase entries have one step each. What it newly
    // produces is the bootloader install:
    //     1. ERASE  MAIN    (flash_nuke.uf2)
    //     2. WRITE  DISPLAY (bl_display.uf2)
    // which the old key order would have emitted backwards -- writing the
    // bootloader while MAIN was still running, then erasing MAIN afterwards.
    //
    // cpuRank() still decides the order of the WRITES, so DISPLAY-before-MAIN
    // -- the rule hardware settled, see its comment -- is untouched.
    //
    // Both ranks still dominate `order`, which is authored in catalog JSON (it
    // comes from array position). Letting `order` decide either would let a
    // catalog entry -- hostile, or merely careless about the order it happened
    // to list its assets in -- reorder a sequence whose correctness is a fact
    // about the hardware, not a preference. `order` still breaks ties between
    // assets that agree on both.
    std::stable_sort(assets.begin(), assets.end(),
                     [](const Uf2Asset& a, const Uf2Asset& b) {
                         if (actionRank(a) != actionRank(b))
                             return actionRank(a) < actionRank(b);
                         if (cpuRank(a.cpu) != cpuRank(b.cpu))
                             return cpuRank(a.cpu) < cpuRank(b.cpu);
                         return a.order < b.order;
                     });

    std::vector<FlashStep> plan;
    plan.reserve(assets.size());
    for (const auto& a : assets)
        plan.push_back(FlashStep{ a.ref, a.cpu, a.sha256, describe(entry, a),
                                  isEraseAsset(a) ? StepAction::Erase
                                                  : StepAction::Write });
    return plan;
}

namespace {

/// True when step `i` of `plan` is an erase whose product a verified probe has
/// already observed, AND some later step writes the same CPU. The one rule
/// dropRedundantErases() and droppedEraseNote() both key off -- written once so
/// the note can never describe a step the drop did not actually remove.
bool eraseIsRedundant(std::span<const FlashStep> plan, std::size_t i,
                      const CpuIdentity& identity)
{
    if (plan[i].action != StepAction::Erase) return false;
    if (!volumeForCpu(identity, plan[i].cpu).has_value()) return false;
    for (std::size_t j = i + 1; j < plan.size(); ++j)
        if (plan[j].cpu == plan[i].cpu) return true;
    return false;
}

} // namespace

std::vector<FlashStep> dropRedundantErases(std::vector<FlashStep> plan, const CpuIdentity& identity)
{
    std::vector<FlashStep> kept;
    kept.reserve(plan.size());
    for (std::size_t i = 0; i < plan.size(); ++i)
        if (!eraseIsRedundant(plan, i, identity))
            kept.push_back(std::move(plan[i]));
    return kept;
}

std::string droppedEraseNote(std::span<const FlashStep> fullPlan, const CpuIdentity& identity)
{
    for (std::size_t i = 0; i < fullPlan.size(); ++i) {
        if (!eraseIsRedundant(fullPlan, i, identity)) continue;
        const char* cpu = fullPlan[i].cpu == TargetCpu::Main ? "MAIN" : "DISPLAY";
        const std::string& volume = *volumeForCpu(identity, fullPlan[i].cpu);
        // Says what was removed, what replaced the need for it, and -- last,
        // because it is the part a user would otherwise wonder about -- that
        // nothing about the outcome changes.
        //
        // NAME THE RIGHT SOURCE. This used to say "the CPU probe has already
        // identified", unconditionally, because the probe was the case in mind
        // when it was written. But eraseIsRedundant() keys only on there BEING
        // a volume for the CPU, and a volume can equally come from hub
        // position -- which is what happens on an ordinary FreeWili, and what
        // was observed on Linux while this note was being displayed. Telling a
        // user a probe ran when none did is a false statement on a consent
        // screen, in an app whose whole argument for being trusted is that it
        // does not guess about which CPU is which.
        const IdentitySource src = fullPlan[i].cpu == TargetCpu::Main
                                       ? identity.mainSource : identity.displaySource;
        const char* how = src == IdentitySource::VerifiedProbe
                              ? "the CPU probe has already identified "
                          : src == IdentitySource::HubLocation
                              ? "its position on the board's USB hub already identifies "
                          : src == IdentitySource::ProductString
                              ? "its USB product string already identifies "
                              : "this app has already identified ";
        return std::string("The ERASE of the ") + cpu + " CPU is not in this plan: " + how +
               volume + " as the " + cpu + " CPU sitting "
               "in its own bootloader, which is the state that erase existed to produce. "
               "Running it anyway would destroy nothing that is not about to be overwritten "
               "and would reboot the CPU, changing the drive letters this identification "
               "depends on. The firmware written to the " + cpu + " CPU is unchanged.";
    }
    return {};
}

std::vector<std::string> planWarnings(const CatalogEntry& entry, const CpuIdentity& identity)
{
    // Checked before the scheme switch, and unconditionally for this one
    // slug: FlashScheme::OgApp's generic "expects the wiliOGBsp display
    // bootloader" warning below is simply wrong for an entry that erases the
    // MAIN CPU rather than installing an app on it. See kEraseMainCpuSlug's
    // comment (fwFlashPlan.h).
    if (entry.slug == kEraseMainCpuSlug) {
        return { "This destroys the MAIN CPU's firmware entirely. It cannot be undone "
                 "from this app -- reflash the MAIN CPU afterward from App Explorer or "
                 "the OG Bootloader Installer tab." };
    }
    // Same shape and the same reason as the erase-MAIN branch above:
    // FlashScheme::DisplayBootloader's own answer below is an empty warning
    // list, which is right for installing a bootloader and silent about
    // destroying one. Checked by slug, ahead of the scheme, exactly as
    // eraseAllowsCpu() is.
    if (entry.slug == kEraseDisplayCpuSlug) {
        return { "This destroys the DISPLAY CPU's firmware entirely, including the "
                 "display bootloader if one is installed.",
                 // Said out loud because the DISPLAY CPU having no BOOTSEL button is
                 // the reason every other refusal in this app exists, and a user who
                 // knows that will otherwise read this action as unrecoverable.
                 "It is recoverable: an RP2040 with blank flash presents RPI-RP2 by "
                 "itself, with no button, so the display bootloader can be reinstalled "
                 "from this tab straight afterward." };
    }
    switch (entry.scheme) {
    case FlashScheme::LegacyDirect: {
        std::vector<std::string> warnings{
            "This removes the display bootloader. Returning to FreeWili "
            "1-OG will require reinstalling it from this tab." };
        // The erase is destructive and lands on the CPU with no BOOTSEL button,
        // so it is said out loud rather than left to be inferred from the step
        // list -- together with the reason it is nonetheless the safe way
        // round. But only when it is actually going to happen: warning about an
        // erase that dropRedundantErases() has removed would describe an action
        // this run will not take, on the screen where the user consents to it.
        if (droppedEraseNote(buildFlashPlan(entry), identity).empty())
            warnings.push_back(
                "The DISPLAY CPU is ERASED first, before anything is written to "
                "it. That is deliberate: it is the only CPU with no BOOTSEL "
                "button, so it is dealt with while the MAIN CPU is still quiet, "
                "and an erased RP2040 returns to its own bootloader by itself. "
                "The MAIN CPU is written last.");
        else
            warnings.push_back(
                "The DISPLAY CPU is NOT erased by this run -- it is already in "
                "its own bootloader, which is all that erase was for. The "
                "display image is written straight to it, and the MAIN CPU is "
                "written last.");
        return warnings;
    }
    case FlashScheme::OgApp:
        return { "This image expects the wiliOGBsp display bootloader. If it "
                 "is not installed, the display will not come up." };
    case FlashScheme::DisplayBootloader:
        return {};
    }
    return {};
}

} // namespace fwog
