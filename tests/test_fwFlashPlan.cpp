#include <doctest/doctest.h>
#include "flash/fwFlashPlan.h"
#include "catalog/fwCatalogEmbedded.h"

using namespace fwog;

namespace {

Uf2Asset asset(TargetCpu cpu, std::string id, int order) {
    Uf2Asset a;
    a.cpu = cpu;
    a.ref.embeddedId = std::move(id);
    a.order = order;
    return a;
}

CatalogEntry ogApp() {
    CatalogEntry e;
    e.slug = "wili-blinky";
    e.scheme = FlashScheme::OgApp;
    e.uf2 = { asset(TargetCpu::Main, "blinky", 0) };
    return e;
}

CatalogEntry bootloader() {
    CatalogEntry e;
    e.slug = "display-bootloader";
    e.scheme = FlashScheme::DisplayBootloader;
    e.uf2 = { asset(TargetCpu::Display, "bl_display", 0) };
    return e;
}

CatalogEntry eraseMain() {
    CatalogEntry e;
    e.slug = kEraseMainCpuSlug;
    e.scheme = FlashScheme::OgApp;   // no new scheme was added for this -- see kEraseMainCpuSlug's comment
    e.uf2 = { asset(TargetCpu::Main, "flash_nuke", 0) };
    return e;
}

CatalogEntry legacy() {
    CatalogEntry e;
    e.slug = "freewili-original";
    e.scheme = FlashScheme::LegacyDirect;
    // Deliberately out of order, to prove the plan sorts rather than trusting
    // the input order.
    e.uf2 = { asset(TargetCpu::Main,    "FreeWiliMain",    1),
              asset(TargetCpu::Display, "FreeWiliDisplay", 0) };
    return e;
}

/// The real deprecated-firmware entry out of the compiled-in catalog, not a
/// fixture. Empty slug if the manifest ever stops carrying one, which the
/// tests using it REQUIRE against rather than passing vacuously.
CatalogEntry realLegacyEntry() {
    for (const auto& e : embeddedEntries())
        if (e.scheme == FlashScheme::LegacyDirect) return e;
    return {};
}

} // namespace

TEST_CASE("OgApp produces one step targeting the main CPU") {
    auto plan = buildFlashPlan(ogApp());
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].cpu == TargetCpu::Main);
    CHECK(plan[0].image.embeddedId == "blinky");
}

TEST_CASE("DisplayBootloader produces one step targeting the display CPU") {
    auto plan = buildFlashPlan(bootloader());
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].cpu == TargetCpu::Display);
    CHECK(plan[0].image.embeddedId == "bl_display");
}

TEST_CASE("LegacyDirect is display first, then main") {
    // Order is not cosmetic, and this assertion is the INVERSE of what it used
    // to be. It was changed because hardware disproved the old rule, not
    // because the new one reads nicer: the DISPLAY CPU's bootloader console
    // only enumerates after ~10 s of MAIN-CPU silence, so a real MAIN-first
    // run wrote MAIN, watched the freshly installed firmware chatter on the
    // inter-CPU link, and then timed out waiting thirty seconds for a DISPLAY
    // volume that could not appear. MAIN-first makes the DISPLAY step
    // unreachable. MAIN also has a BOOTSEL button and is therefore the CPU
    // that is always recoverable, which is why it is the safe one to leave
    // until last. See cpuRank() in fwFlashPlan.cpp.
    auto plan = buildFlashPlan(legacy());
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].cpu == TargetCpu::Display);
    CHECK(plan[0].image.embeddedId == "FreeWiliDisplay");
    CHECK(plan[1].cpu == TargetCpu::Main);
    CHECK(plan[1].image.embeddedId == "FreeWiliMain");
}

TEST_CASE("an entry with no assets produces no plan") {
    CatalogEntry e;
    e.scheme = FlashScheme::OgApp;
    CHECK(buildFlashPlan(e).empty());
}

TEST_CASE("assets whose CPU contradicts the scheme are dropped") {
    // An OgApp entry that somehow carries a display asset must not silently
    // flash the display CPU.
    CatalogEntry e = ogApp();
    e.uf2.push_back(asset(TargetCpu::Display, "sneaky", 1));
    auto plan = buildFlashPlan(e);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].cpu == TargetCpu::Main);
}

TEST_CASE("steps carry the asset's sha256 forward") {
    CatalogEntry e = ogApp();
    e.uf2[0].sha256 = "abc123";
    auto plan = buildFlashPlan(e);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].sha256 == "abc123");
}

TEST_CASE("LegacyDirect warns that it destroys the display bootloader") {
    auto w = planWarnings(legacy(), CpuIdentity{});
    REQUIRE(w.size() >= 1);
    bool found = false;
    for (const auto& s : w)
        if (s.find("display bootloader") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("OgApp warns that it expects an installed bootloader") {
    auto w = planWarnings(ogApp(), CpuIdentity{});
    REQUIRE(w.size() >= 1);
    bool found = false;
    for (const auto& s : w)
        if (s.find("bootloader") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("DisplayBootloader carries no warning") {
    CHECK(planWarnings(bootloader(), CpuIdentity{}).empty());
}

// ---------------------------------------------------------------------------
// The erase-MAIN entry (Task 22): OgApp scheme, one MAIN step, but its plan
// description and warning must read as destructive, not as an ordinary app
// install -- see kEraseMainCpuSlug's comment in fwFlashPlan.h for why this
// needs its own coverage distinct from "OgApp produces one step targeting
// the main CPU" above.
// ---------------------------------------------------------------------------

TEST_CASE("the erase-MAIN entry produces exactly one step targeting MAIN") {
    auto plan = buildFlashPlan(eraseMain());
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].cpu == TargetCpu::Main);
    CHECK(plan[0].image.embeddedId == "flash_nuke");
}

TEST_CASE("the erase-MAIN entry's step description says it erases, not installs") {
    auto plan = buildFlashPlan(eraseMain());
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].description.find("Erase") != std::string::npos);
    // Must NOT read like the ordinary OgApp step description -- a user
    // skimming the plan must not mistake this for a normal app install.
    CHECK(plan[0].description.find("Write the application image") == std::string::npos);
}

TEST_CASE("the erase-MAIN entry warns that it destroys MAIN, not the generic OgApp warning") {
    auto w = planWarnings(eraseMain(), CpuIdentity{});
    REQUIRE(w.size() == 1);
    CHECK(w[0].find("destroys the MAIN CPU's firmware") != std::string::npos);
    // The generic OgApp warning ("expects the wiliOGBsp display bootloader")
    // must be replaced, not appended -- it would be actively misleading here.
    CHECK(w[0].find("expects the wiliOGBsp display bootloader") == std::string::npos);
}

// ---------------------------------------------------------------------------
// The three cases below pinned MAIN-before-DISPLAY against catalog-authored
// `order` values. They now pin DISPLAY-before-MAIN against exactly the same
// attack: the point of them was never WHICH order, it was that the CODE fixes
// the order and a catalog entry cannot invert it. Each has been rewritten to
// the equivalent assertion rather than deleted, so that property stays pinned.
// ---------------------------------------------------------------------------

TEST_CASE("LegacyDirect where main has the lower order still flashes display first") {
    // `order` is authored from JSON array position, so a catalog entry can
    // list main first and give it order 0. CPU rank must dominate `order`:
    // a main-first sequence makes the DISPLAY step unreachable on real
    // hardware (see "LegacyDirect is display first, then main" above), and
    // that must not be reachable from catalog data.
    CatalogEntry e;
    e.slug = "inverted-order";
    e.scheme = FlashScheme::LegacyDirect;
    e.uf2 = { asset(TargetCpu::Main,    "FreeWiliMain",    0),
              asset(TargetCpu::Display, "FreeWiliDisplay", 1) };
    auto plan = buildFlashPlan(e);
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].cpu == TargetCpu::Display);
    CHECK(plan[0].image.embeddedId == "FreeWiliDisplay");
    CHECK(plan[1].cpu == TargetCpu::Main);
    CHECK(plan[1].image.embeddedId == "FreeWiliMain");
}

TEST_CASE("LegacyDirect with equal order values still flashes display first") {
    // When both assets share the same order value and main appears first in
    // the array, stable_sort would otherwise preserve the array order.
    CatalogEntry e;
    e.slug = "equal-order";
    e.scheme = FlashScheme::LegacyDirect;
    e.uf2 = { asset(TargetCpu::Main,    "FreeWiliMain",    0),
              asset(TargetCpu::Display, "FreeWiliDisplay", 0) };
    auto plan = buildFlashPlan(e);
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].cpu == TargetCpu::Display);
    CHECK(plan[0].image.embeddedId == "FreeWiliDisplay");
    CHECK(plan[1].cpu == TargetCpu::Main);
    CHECK(plan[1].image.embeddedId == "FreeWiliMain");
}

TEST_CASE("order still sequences assets targeting the same CPU") {
    // `order` must still work as the tiebreaker that sequences multiple
    // assets targeting one CPU -- the structural ranks above it are not
    // allowed to have swallowed it.
    CatalogEntry e;
    e.slug = "same-cpu-order";
    e.scheme = FlashScheme::LegacyDirect;
    e.uf2 = { asset(TargetCpu::Display, "display-second", 1),
              asset(TargetCpu::Display, "display-first",  0),
              asset(TargetCpu::Main,    "main",           0) };
    auto plan = buildFlashPlan(e);
    REQUIRE(plan.size() == 3);
    // Display assets in `order`, then main.
    CHECK(plan[0].cpu == TargetCpu::Display);
    CHECK(plan[0].image.embeddedId == "display-first");
    CHECK(plan[1].cpu == TargetCpu::Display);
    CHECK(plan[1].image.embeddedId == "display-second");
    CHECK(plan[2].cpu == TargetCpu::Main);
    CHECK(plan[2].image.embeddedId == "main");
}

// ---------------------------------------------------------------------------
// The DISPLAY erase step, and the exact boundaries of the exception that lets
// the flash-erase image reach the DISPLAY CPU at all.
// ---------------------------------------------------------------------------

TEST_CASE("the real embedded deprecated-firmware entry plans erase DISPLAY, write DISPLAY, write MAIN") {
    // Deliberately the REAL compiled-in entry rather than a fixture: every
    // ordering test above proves buildFlashPlan behaves correctly GIVEN a
    // three-asset LegacyDirect entry, and none of them would notice if
    // firmware/manifest.json had never gained the flash_nuke asset, or had
    // gained it on the wrong CPU. This is the one that ties the shipped
    // catalog to the sequence the owner asked for.
    const CatalogEntry entry = realLegacyEntry();
    REQUIRE_FALSE(entry.slug.empty());
    REQUIRE(entry.uf2.size() == 3);

    const auto plan = buildFlashPlan(entry);
    REQUIRE(plan.size() == 3);

    CHECK(plan[0].cpu == TargetCpu::Display);
    CHECK(plan[0].image.embeddedId == kFlashNukeImageId);
    CHECK(plan[0].action == StepAction::Erase);

    CHECK(plan[1].cpu == TargetCpu::Display);
    CHECK(plan[1].image.embeddedId == "FreeWiliDisplay");
    CHECK(plan[1].action == StepAction::Write);

    CHECK(plan[2].cpu == TargetCpu::Main);
    CHECK(plan[2].image.embeddedId == "FreeWiliMain");
    CHECK(plan[2].action == StepAction::Write);
}

TEST_CASE("the real deprecated-firmware plan describes its erase step as an erase, naming DISPLAY") {
    // The plan list is what the user reads before consenting. describe() used
    // to tell the erase case apart by slug, which cannot work now that one
    // entry holds an erase AND a write for the SAME CPU -- so this pins that
    // the two same-CPU steps do not read alike.
    const CatalogEntry entry = realLegacyEntry();
    REQUIRE_FALSE(entry.slug.empty());
    const auto plan = buildFlashPlan(entry);
    REQUIRE(plan.size() == 3);

    CHECK(plan[0].description.find("Erase the DISPLAY CPU") != std::string::npos);
    CHECK(plan[0].description.find("flash-erase image") != std::string::npos);
    // The write step for the same CPU must not borrow the erase's wording,
    // nor the erase the write's.
    CHECK(plan[1].description.find("Erase") == std::string::npos);
    CHECK(plan[1].description.find("Write the display image") != std::string::npos);
    CHECK(plan[0].description != plan[1].description);
}

TEST_CASE("a catalog entry cannot reorder the real plan by rewriting its order values") {
    // The same structural guarantee as the fixture tests, but applied to the
    // real entry with its `order` values inverted -- the shape a hostile or
    // careless remote/local override of this entry would take.
    CatalogEntry entry = realLegacyEntry();
    REQUIRE_FALSE(entry.slug.empty());
    REQUIRE(entry.uf2.size() == 3);
    // Main earliest, erase last: the worst order someone could ask for.
    for (auto& a : entry.uf2) {
        if (a.cpu == TargetCpu::Main)        a.order = 0;
        else if (isEraseAsset(a))            a.order = 9;
        else                                 a.order = 5;
    }

    const auto plan = buildFlashPlan(entry);
    REQUIRE(plan.size() == 3);
    CHECK(plan[0].action == StepAction::Erase);
    CHECK(plan[0].cpu == TargetCpu::Display);
    CHECK(plan[1].cpu == TargetCpu::Display);
    CHECK(plan[1].action == StepAction::Write);
    CHECK(plan[2].cpu == TargetCpu::Main);
}

TEST_CASE("an erase asset never precedes a write to the same CPU by way of order") {
    // The erase-before-write rank, isolated from the CPU rank.
    CatalogEntry e;
    e.slug = "erase-after-write";
    e.scheme = FlashScheme::LegacyDirect;
    e.uf2 = { asset(TargetCpu::Display, "FreeWiliDisplay", 0),
              asset(TargetCpu::Display, kFlashNukeImageId, 1) };
    auto plan = buildFlashPlan(e);
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].action == StepAction::Erase);   // despite the higher `order`
    CHECK(plan[1].action == StepAction::Write);
}

TEST_CASE("the flash-erase image on DISPLAY is dropped for every scheme but LegacyDirect") {
    // The narrowness of the exception, stated as a test. Erasing the DISPLAY
    // CPU is permitted only as the opening step of a plan that goes on to
    // write a display image; it is not a general "erase images may target any
    // CPU" rule, and this is what stops one from appearing by accident.
    CatalogEntry bl;
    bl.slug = "bootloader-with-an-erase";
    bl.scheme = FlashScheme::DisplayBootloader;
    bl.uf2 = { asset(TargetCpu::Display, kFlashNukeImageId, 0),
               asset(TargetCpu::Display, "bl_display",      1) };
    auto blPlan = buildFlashPlan(bl);
    REQUIRE(blPlan.size() == 1);
    CHECK(blPlan[0].image.embeddedId == "bl_display");

    // And OgApp, which cannot reach DISPLAY at all, erase image or not.
    CatalogEntry app;
    app.slug = "app-with-a-display-erase";
    app.scheme = FlashScheme::OgApp;
    app.uf2 = { asset(TargetCpu::Main,    "blinky",         0),
                asset(TargetCpu::Display, kFlashNukeImageId, 1) };
    auto appPlan = buildFlashPlan(app);
    REQUIRE(appPlan.size() == 1);
    CHECK(appPlan[0].cpu == TargetCpu::Main);
}

TEST_CASE("OgApp still cannot reach the DISPLAY CPU with any image") {
    // The layer that keeps ordinary app images -- which drive GPIO 29 against
    // the PDM microphone if they run on the DISPLAY CPU -- off that CPU. The
    // DISPLAY-first change must not have widened it.
    CatalogEntry e = ogApp();
    e.uf2.push_back(asset(TargetCpu::Display, "an-app-image",    1));
    e.uf2.push_back(asset(TargetCpu::Display, kFlashNukeImageId, 2));
    auto plan = buildFlashPlan(e);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].cpu == TargetCpu::Main);
    CHECK(plan[0].image.embeddedId == "blinky");
}

TEST_CASE("the real erase-main-cpu entry is MAIN-only and stays MAIN-only if retargeted") {
    // Two independent things, both pinned against the real compiled-in entry:
    // that the shipped entry targets MAIN, and that rewriting its asset to
    // DISPLAY -- the shape a retarget would take -- produces an EMPTY plan
    // rather than an erase of the CPU with no BOOTSEL button. It stays an
    // OgApp entry, and OgApp permits only Main.
    const std::vector<CatalogEntry> entries = embeddedEntries();
    const CatalogEntry* found = nullptr;
    for (const auto& e : entries)
        if (e.slug == kEraseMainCpuSlug) found = &e;
    REQUIRE(found != nullptr);

    auto plan = buildFlashPlan(*found);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].cpu == TargetCpu::Main);
    CHECK(plan[0].action == StepAction::Erase);
    CHECK(plan[0].description.find("Erase the MAIN CPU") != std::string::npos);

    CatalogEntry retargeted = *found;
    for (auto& a : retargeted.uf2) a.cpu = TargetCpu::Display;
    CHECK(buildFlashPlan(retargeted).empty());
}

TEST_CASE("the real erase-display-cpu entry is DISPLAY-only and stays DISPLAY-only if retargeted") {
    // The exact mirror of the erase-MAIN test above, and pinned for the same
    // two reasons: that the shipped entry targets DISPLAY, and that rewriting
    // its asset to MAIN -- the shape a retarget would take -- produces an EMPTY
    // plan rather than an erase of the wrong CPU. It stays a DisplayBootloader
    // entry, and DisplayBootloader permits only Display.
    const std::vector<CatalogEntry> entries = embeddedEntries();
    const CatalogEntry* found = nullptr;
    for (const auto& e : entries)
        if (e.slug == kEraseDisplayCpuSlug) found = &e;
    REQUIRE(found != nullptr);

    auto plan = buildFlashPlan(*found);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].cpu == TargetCpu::Display);
    CHECK(plan[0].action == StepAction::Erase);
    // And it says ERASE, not "Install the display serial bootloader" -- which
    // is what FlashScheme::DisplayBootloader's own description would have made
    // this step read as before describe() tested for an erase first.
    CHECK(plan[0].description.find("Erase the DISPLAY CPU") != std::string::npos);

    CatalogEntry retargeted = *found;
    for (auto& a : retargeted.uf2) a.cpu = TargetCpu::Main;
    CHECK(buildFlashPlan(retargeted).empty());
}

TEST_CASE("only the real bootloader entry may erase MAIN under DisplayBootloader") {
    // eraseOnlyException() is what lets the bootloader install erase MAIN before
    // it writes the DISPLAY bootloader. It is keyed on the SLUG, and this is the
    // test for that: an imposter with the same scheme and the same erase asset
    // -- a remote catalog entry, say -- gets an empty plan, not an erase of the
    // user's MAIN CPU.
    const std::vector<CatalogEntry> entries = embeddedEntries();
    const CatalogEntry* real = nullptr;
    for (const auto& e : entries)
        if (e.slug == kOgBootloaderSlug) real = &e;
    REQUIRE(real != nullptr);

    // The real one: erase MAIN first, then write the DISPLAY bootloader.
    const auto plan = buildFlashPlan(*real);
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].cpu == TargetCpu::Main);
    CHECK(plan[0].action == StepAction::Erase);
    CHECK(plan[1].cpu == TargetCpu::Display);
    CHECK(plan[1].action != StepAction::Erase);

    // The imposter: identical in every way except its slug.
    CatalogEntry imposter = *real;
    imposter.slug = "totally-legit-bootloader";
    const auto imposterPlan = buildFlashPlan(imposter);
    for (const auto& step : imposterPlan)
        CHECK(step.cpu != TargetCpu::Main);
}

TEST_CASE("the DISPLAY-erase exception is keyed on the slug, not on the scheme") {
    // eraseAllowsCpu() lets a DISPLAY erase through for LegacyDirect and for
    // the erase-display-cpu slug, and for nothing else. Without the slug being
    // load-bearing, ANY DisplayBootloader entry -- including one from a remote
    // catalog -- could acquire an erase step merely by listing the erase image.
    //
    // Same entry shape, same scheme, same asset; only the slug differs.
    CatalogEntry sanctioned;
    sanctioned.slug   = kEraseDisplayCpuSlug;
    sanctioned.scheme = FlashScheme::DisplayBootloader;
    sanctioned.uf2    = { asset(TargetCpu::Display, kFlashNukeImageId, 0) };
    REQUIRE(buildFlashPlan(sanctioned).size() == 1);

    CatalogEntry impostor = sanctioned;
    impostor.slug = "some-other-display-entry";
    CHECK(buildFlashPlan(impostor).empty());

    // And the bootloader INSTALL entry cannot pick one up either: its firmware
    // asset survives, the erase beside it does not.
    CatalogEntry withErase = bootloader();
    withErase.uf2.push_back(asset(TargetCpu::Display, kFlashNukeImageId, 1));
    const auto plan = buildFlashPlan(withErase);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].image.embeddedId == "bl_display");
    CHECK(plan[0].action == StepAction::Write);
}

TEST_CASE("erase-display-cpu warns that it is destructive AND that it is recoverable") {
    // Both halves matter. FlashScheme::DisplayBootloader's own warning list is
    // EMPTY, which is right for installing a bootloader and silent about
    // destroying one -- so without the slug branch this action would be the
    // only destructive thing in the app that warned about nothing. And the
    // recoverability is not padding: the DISPLAY CPU having no BOOTSEL button
    // is the premise of every other refusal here, so a user who knows that will
    // read this as unrecoverable unless told otherwise.
    const std::vector<CatalogEntry> entries = embeddedEntries();
    const CatalogEntry* found = nullptr;
    for (const auto& e : entries)
        if (e.slug == kEraseDisplayCpuSlug) found = &e;
    REQUIRE(found != nullptr);

    const auto w = planWarnings(*found, CpuIdentity{});
    REQUIRE_FALSE(w.empty());
    bool saysDestroys = false, saysRecoverable = false;
    for (const auto& s : w) {
        if (s.find("destroys the DISPLAY CPU") != std::string::npos) saysDestroys = true;
        if (s.find("recoverable") != std::string::npos &&
            s.find("RPI-RP2") != std::string::npos) saysRecoverable = true;
    }
    CHECK(saysDestroys);
    CHECK(saysRecoverable);

    // NOT the generic OgApp "this image expects the wiliOGBsp display
    // bootloader" line, and not silence either -- both would be wrong here.
    for (const auto& s : w)
        CHECK(s.find("expects the wiliOGBsp") == std::string::npos);
}

TEST_CASE("erase-display-cpu is never dropped, however thoroughly DISPLAY is identified") {
    // dropRedundantErases()'s second half, applied to the new entry: an erase
    // is only redundant when a LATER step writes the same CPU. Nothing follows
    // this one, so a probed DISPLAY volume -- a DISPLAY CPU sitting in its own
    // bootloader with its firmware still in flash -- must not silently turn
    // "erase it" into a no-op.
    const std::vector<CatalogEntry> entries = embeddedEntries();
    const CatalogEntry* found = nullptr;
    for (const auto& e : entries)
        if (e.slug == kEraseDisplayCpuSlug) found = &e;
    REQUIRE(found != nullptr);

    const auto full = buildFlashPlan(*found);
    REQUIRE(full.size() == 1);
    REQUIRE(full[0].action == StepAction::Erase);

    CpuIdentity id;
    id.displayVolume = "G:/";
    id.displaySource = IdentitySource::VerifiedProbe;
    const auto kept = dropRedundantErases(full, id);
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].action == StepAction::Erase);
    CHECK(kept[0].cpu == TargetCpu::Display);
    CHECK(droppedEraseNote(full, id).empty());
}

TEST_CASE("LegacyDirect warns that the DISPLAY CPU is erased before it is written") {
    // The erase is new, destructive, and lands on the CPU with no BOOTSEL
    // button. It must be said before the user confirms, not left to be
    // inferred from the step list.
    auto w = planWarnings(realLegacyEntry(), CpuIdentity{});
    bool saysErased = false, saysBootloader = false;
    for (const auto& s : w) {
        if (s.find("ERASED") != std::string::npos) saysErased = true;
        if (s.find("display bootloader") != std::string::npos) saysBootloader = true;
    }
    CHECK(saysErased);
    CHECK(saysBootloader);   // the original warning is kept, not replaced
}

// ---------------------------------------------------------------------------
// dropRedundantErases: the erase a verified CPU probe has already made
// pointless.
//
// The board owner's question was "trying to flash the deprecated firmware will
// only flash display if its not mounted?" -- and it exposed exactly this. The
// LegacyDirect plan leads with ERASE DISPLAY solely to get the DISPLAY CPU into
// its own bootloader, because it has no BOOTSEL button. If a probe has already
// measured that CPU's volume as mounted, it is observably in that bootloader
// already, and running the erase would spend a reboot cycle reaching a state it
// is being watched in -- a reboot that changes the drive letters the mapping
// depends on, which would then void the mapping and block the rest of the plan.
// ---------------------------------------------------------------------------

namespace {

/// A CpuIdentity carrying a verified probe result, built the same way the app
/// builds it. Written out here rather than pulling in fwDeviceModel.h, so this
/// file stays about plans.
CpuIdentity probed(const char* volume, TargetCpu cpu)
{
    CpuIdentity id;
    if (cpu == TargetCpu::Main) { id.mainVolume = volume; id.mainSource = IdentitySource::VerifiedProbe; }
    else { id.displayVolume = volume; id.displaySource = IdentitySource::VerifiedProbe; }
    return id;
}

/// The real deprecated-firmware plan: ERASE DISPLAY, WRITE DISPLAY, WRITE MAIN.
std::vector<FlashStep> realLegacyPlan() { return buildFlashPlan(realLegacyEntry()); }

} // namespace

TEST_CASE("the real LegacyDirect plan still leads with an erase when nothing is known") {
    // The premise every case below depends on. Asserted rather than assumed, so
    // that a manifest change turning this into a two-step plan fails HERE
    // instead of silently making the drop tests pass over nothing.
    REQUIRE_FALSE(realLegacyEntry().slug.empty());
    const auto full = realLegacyPlan();
    REQUIRE(full.size() == 3);
    CHECK(full[0].action == StepAction::Erase);
    CHECK(full[0].cpu == TargetCpu::Display);
    CHECK(full[1].cpu == TargetCpu::Display);
    CHECK(full[2].cpu == TargetCpu::Main);

    // With no probe result, nothing is dropped -- which is every call this app
    // made before probe mappings existed.
    CHECK(dropRedundantErases(full, CpuIdentity{}).size() == 3);
    CHECK(droppedEraseNote(full, CpuIdentity{}).empty());
}

TEST_CASE("a probed DISPLAY volume drops the LegacyDirect erase") {
    const auto full = realLegacyPlan();
    const CpuIdentity id = probed("G:/", TargetCpu::Display);

    const auto kept = dropRedundantErases(full, id);
    REQUIRE(kept.size() == 2);
    // The erase is gone and the two writes are untouched, in their original
    // order -- DISPLAY still before MAIN.
    CHECK(kept[0].action == StepAction::Write);
    CHECK(kept[0].cpu == TargetCpu::Display);
    CHECK(kept[1].action == StepAction::Write);
    CHECK(kept[1].cpu == TargetCpu::Main);

    // And it says so, naming the drive and the CPU.
    const std::string note = droppedEraseNote(full, id);
    CHECK(note.find("G:/") != std::string::npos);
    CHECK(note.find("DISPLAY") != std::string::npos);
}

TEST_CASE("a probed MAIN volume does not drop the DISPLAY erase") {
    // The mapping has to be about the CPU the erase targets. Knowing that the
    // mounted drive is MAIN says nothing at all about where DISPLAY is.
    const auto full = realLegacyPlan();
    const CpuIdentity id = probed("G:/", TargetCpu::Main);
    CHECK(dropRedundantErases(full, id).size() == 3);
    CHECK(droppedEraseNote(full, id).empty());
}

TEST_CASE("erase-main-cpu is never dropped, however thoroughly MAIN is identified") {
    // The rule's second half, and the one that stops it eating a step whose
    // erasing IS the point. A MAIN CPU held in BOOTSEL by its button still has
    // its firmware; erasing it is exactly what the user asked for. Nothing
    // later in this plan writes MAIN, so there is no following write for the
    // erase to be preparing.
    const std::vector<CatalogEntry> entries = embeddedEntries();
    const CatalogEntry* found = nullptr;
    for (const auto& e : entries)
        if (e.slug == kEraseMainCpuSlug) found = &e;
    REQUIRE(found != nullptr);

    const auto full = buildFlashPlan(*found);
    REQUIRE(full.size() == 1);
    REQUIRE(full[0].action == StepAction::Erase);

    const CpuIdentity id = probed("G:/", TargetCpu::Main);
    const auto kept = dropRedundantErases(full, id);
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].action == StepAction::Erase);
    CHECK(kept[0].cpu == TargetCpu::Main);
    CHECK(droppedEraseNote(full, id).empty());
}

TEST_CASE("the erase warning is replaced, not merely kept, when the erase is dropped") {
    // A warning describing an action this run will not take is worse than no
    // warning: it is the app misdescribing itself on the one screen where the
    // user is deciding whether to consent.
    const CatalogEntry entry = realLegacyEntry();
    REQUIRE_FALSE(entry.slug.empty());

    const auto w = planWarnings(entry, probed("G:/", TargetCpu::Display));
    bool claimsErase = false, saysNotErased = false, saysBootloader = false;
    for (const auto& s : w) {
        if (s.find("is ERASED first") != std::string::npos) claimsErase = true;
        if (s.find("NOT erased") != std::string::npos)      saysNotErased = true;
        if (s.find("display bootloader") != std::string::npos) saysBootloader = true;
    }
    CHECK_FALSE(claimsErase);
    CHECK(saysNotErased);
    // The bootloader-destruction warning is unconditional and stays: the
    // display image still lands on top of it either way.
    CHECK(saysBootloader);
}
