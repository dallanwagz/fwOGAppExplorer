#include <doctest/doctest.h>
#include "ui/fwTabDefaultFirmwareLogic.h"

#include "catalog/fwCatalogEmbedded.h"
#include "flash/fwFlashPlan.h"   // isEraseAsset

#include <string>

using namespace fwog;

namespace {

// The real embedded LegacyDirect entry -- one Main asset (version "v92"), one
// Display asset (version "v67"), and since the DISPLAY-first reordering a
// third asset, the flash-erase image on DISPLAY, per firmware/manifest.json.
// There is no way to fake embeddedVersion() (it reads a compiled-in table),
// so these tests slice up the real entry's uf2 vector rather than construct
// ids embeddedVersion() would not recognize.
CatalogEntry findLegacyEntry()
{
    for (const auto& e : embeddedEntries())
        if (e.scheme == FlashScheme::LegacyDirect) return e;
    return {};
}

} // namespace

TEST_CASE("legacyVersion: both CPUs present composes 'Main <v> / Display <v>'") {
    const CatalogEntry entry = findLegacyEntry();
    REQUIRE(entry.uf2.size() == 3);
    CHECK(detail::legacyVersion(entry) == "Main v92 / Display v67");
}

TEST_CASE("legacyVersion: the flash-erase asset never reaches the version line") {
    // The erase image has a "version" of its own ("pico-flash-nuke") and
    // targets DISPLAY, so a naive last-display-asset-wins read of the entry
    // would print "Display pico-flash-nuke" -- naming the wrong thing, and
    // depending on manifest array order to decide which wrong thing.
    const CatalogEntry entry = findLegacyEntry();
    REQUIRE(entry.uf2.size() == 3);
    bool hasErase = false;
    for (const auto& a : entry.uf2)
        if (isEraseAsset(a)) hasErase = true;
    REQUIRE(hasErase);   // otherwise this passes vacuously

    const std::string v = detail::legacyVersion(entry);
    CHECK(v.find("nuke") == std::string::npos);
    CHECK(v == "Main v92 / Display v67");
}

TEST_CASE("legacyVersion: main-only composes 'Main <v>' with no Display suffix") {
    CatalogEntry entry = findLegacyEntry();
    REQUIRE(entry.uf2.size() == 3);
    std::vector<Uf2Asset> mainOnly;
    for (const auto& a : entry.uf2)
        if (a.cpu == TargetCpu::Main) mainOnly.push_back(a);
    entry.uf2 = mainOnly;

    CHECK(detail::legacyVersion(entry) == "Main v92");
}

TEST_CASE("legacyVersion: display-only composes 'Display <v>' with no Main prefix") {
    CatalogEntry entry = findLegacyEntry();
    REQUIRE(entry.uf2.size() == 3);
    std::vector<Uf2Asset> displayOnly;
    for (const auto& a : entry.uf2)
        if (a.cpu == TargetCpu::Display && !isEraseAsset(a)) displayOnly.push_back(a);
    entry.uf2 = displayOnly;

    CHECK(detail::legacyVersion(entry) == "Display v67");
}

TEST_CASE("legacyVersion: neither asset present returns an empty string") {
    CatalogEntry entry; // default-constructed: uf2 is empty
    CHECK(detail::legacyVersion(entry).empty());
}

// ---------------------------------------------------------------------------
// eraseConfirmationMatches: gates the Default Firmware tab's two destructive
// erase buttons. Deliberately a different phrase from confirmationMatches()'s
// "MAIN"/"DISPLAY" (the flash engine's own foreign-volume confirmation) --
// these tests pin both the accepted phrases and that the OTHER app's
// confirmation words ("MAIN"/"DISPLAY" alone) do NOT arm either button. That
// anti-habituation is the whole reason the phrases differ, so it is pinned in
// both directions rather than assumed.
// ---------------------------------------------------------------------------

TEST_CASE("erasePhrase names the two-word phrase each CPU's button requires") {
    // The prompt the user reads and the string that is compared come from this
    // one function; a test that hardcoded the phrases separately would not
    // notice them drifting apart.
    CHECK(std::string(detail::erasePhrase(TargetCpu::Main)) == "ERASE MAIN");
    CHECK(std::string(detail::erasePhrase(TargetCpu::Display)) == "ERASE DISPLAY");
}

TEST_CASE("eraseConfirmationMatches accepts the exact phrase, trimmed and case-insensitive") {
    CHECK(detail::eraseConfirmationMatches(TargetCpu::Main, "ERASE MAIN"));
    CHECK(detail::eraseConfirmationMatches(TargetCpu::Main, "erase main"));
    CHECK(detail::eraseConfirmationMatches(TargetCpu::Main, "  Erase Main  "));

    CHECK(detail::eraseConfirmationMatches(TargetCpu::Display, "ERASE DISPLAY"));
    CHECK(detail::eraseConfirmationMatches(TargetCpu::Display, "erase display"));
    CHECK(detail::eraseConfirmationMatches(TargetCpu::Display, "  Erase Display  "));
}

TEST_CASE("eraseConfirmationMatches rejects everything else, including the CPU-name confirmation alone") {
    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Main, ""));
    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Main, "   "));
    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Main, "MAIN"));      // the OTHER confirmation phrase
    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Main, "ERASE"));     // partial
    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Main, "ERASE  MAIN")); // internal whitespace not collapsed
    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Main, "ERASE MAINN"));

    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Display, ""));
    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Display, "   "));
    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Display, "DISPLAY"));  // the OTHER confirmation phrase
    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Display, "ERASE"));
    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Display, "ERASE  DISPLAY"));
    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Display, "ERASE DISPLAYY"));
}

TEST_CASE("each erase phrase arms only its own CPU's button") {
    // The two cards sit next to each other in the same Danger zone. Typing one
    // card's phrase must never arm the other's -- otherwise a user who meant to
    // erase MAIN, and typed so, could arm a wipe of the CPU with no BOOTSEL
    // button.
    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Display, "ERASE MAIN"));
    CHECK_FALSE(detail::eraseConfirmationMatches(TargetCpu::Main, "ERASE DISPLAY"));
}

// ---------------------------------------------------------------------------
// eraseButtonArmed (Task 22 fix round): the pure half of the Critical
// fix. The stateful half -- clearing the confirmation buffer when the
// Danger zone header collapses or the tab loses focus, so a STALE phrase
// typed on a past visit can never reach here with headerOpen still
// true -- lives in fwTabDefaultFirmware.cpp/fwApp.cpp and is ImGui-coupled,
// verified by running the app instead (see task-22 fix-round report). This
// pins the piece that IS pure: regardless of what has been typed, the
// button must never read as armed while the header itself is not open.
// ---------------------------------------------------------------------------

TEST_CASE("eraseButtonArmed requires both the header open and the exact phrase") {
    CHECK(detail::eraseButtonArmed(true, TargetCpu::Main, "ERASE MAIN"));
    CHECK_FALSE(detail::eraseButtonArmed(true, TargetCpu::Main, "MAIN"));
    CHECK_FALSE(detail::eraseButtonArmed(true, TargetCpu::Main, ""));

    CHECK(detail::eraseButtonArmed(true, TargetCpu::Display, "ERASE DISPLAY"));
    CHECK_FALSE(detail::eraseButtonArmed(true, TargetCpu::Display, "DISPLAY"));
    CHECK_FALSE(detail::eraseButtonArmed(true, TargetCpu::Display, ""));
}

TEST_CASE("eraseButtonArmed is false while the header is closed, no matter what was typed") {
    // The exact shape of the bug this fix round closed: a correctly-typed
    // phrase must not arm the button on its own -- see the class doc
    // comment (fwTabDefaultFirmware.h) for why headerOpen alone is not
    // enough to trust either, but this at least pins that a closed header
    // can never produce an armed button.
    CHECK_FALSE(detail::eraseButtonArmed(false, TargetCpu::Main, "ERASE MAIN"));
    CHECK_FALSE(detail::eraseButtonArmed(false, TargetCpu::Main, ""));
    CHECK_FALSE(detail::eraseButtonArmed(false, TargetCpu::Display, "ERASE DISPLAY"));
    CHECK_FALSE(detail::eraseButtonArmed(false, TargetCpu::Display, ""));
}

// ---------------------------------------------------------------------------
// autoIdentifyDecision: whether clicking an install button must run the CPU
// probe itself first.
//
// This is the pure half of "behind the scenes the app should just figure it
// out". The state it exists for is both CPUs sitting in the RP2040 bootrom,
// two indistinguishable RPI-RP2 volumes mounted, and every write refusing as
// ambiguous -- which used to mean a hand trip to the Recovery tab and back.
// ---------------------------------------------------------------------------

TEST_CASE("autoIdentifyDecision runs the probe for exactly two volumes on one board") {
    CHECK(detail::autoIdentifyDecision(2, 1) == detail::AutoIdentify::Run);
    // Nothing selected yet is still one board's worth of volumes: the mounted
    // pair is what makes the write ambiguous, and it is the reason to probe.
    CHECK(detail::autoIdentifyDecision(2, 0) == detail::AutoIdentify::Run);
}

TEST_CASE("autoIdentifyDecision leaves every other volume count alone") {
    // Fewer than two volumes: the engine's own paths already apply -- a
    // touch-and-wait with none mounted, a typed confirmation or a fresh probe
    // mapping with one. Probing there would write the prober to a board that
    // did not need it.
    CHECK(detail::autoIdentifyDecision(0, 1) == detail::AutoIdentify::NotNeeded);
    CHECK(detail::autoIdentifyDecision(1, 1) == detail::AutoIdentify::NotNeeded);
    // Three or more cannot come from one two-CPU board, so "the one that is
    // left" would be a guess rather than a conclusion. identifyCpus() refuses
    // this outright (NotExactlyTwoVolumes); the button must not start a run
    // that is going to refuse on arrival.
    CHECK(detail::autoIdentifyDecision(3, 1) == detail::AutoIdentify::NotNeeded);
}

TEST_CASE("autoIdentifyDecision refuses to probe with more than one board connected") {
    // Two boards with one erased CPU each also produce two RPI-RP2 volumes,
    // and the prober's answer would then say nothing about the other board's
    // drive. identifyCpus() cannot see boards at all, so this is the only
    // layer that can catch it.
    CHECK(detail::autoIdentifyDecision(2, 2) == detail::AutoIdentify::Blocked);
    CHECK(detail::autoIdentifyDecision(2, 7) == detail::AutoIdentify::Blocked);
}

// ---------------------------------------------------------------------------
// locatedWithoutProbe: asked BEFORE autoIdentifyDecision, and the reason two
// mounted drives no longer mean "write a prober image and find out".

TEST_CASE("locatedWithoutProbe accepts a CPU answering on a port") {
    // The legacy entry writes to BOTH CPUs, so both have to be located.
    const CatalogEntry entry = findLegacyEntry();
    CpuIdentity id;
    id.mainPort    = "COM60";
    id.displayPort = "COM65";
    CHECK(detail::locatedWithoutProbe(entry, id));
}

TEST_CASE("locatedWithoutProbe accepts a bootrom drive named by hub position") {
    // Both CPUs in BOOTSEL. Nothing is answering on a port, nothing has been
    // probed, and yet both are located -- which is the whole point: clicking
    // install here must go straight to the flash dialog.
    const CatalogEntry entry = findLegacyEntry();
    CpuIdentity id;
    id.mainVolume    = "G:/";
    id.mainSource    = IdentitySource::HubLocation;
    id.displayVolume = "H:/";
    id.displaySource = IdentitySource::HubLocation;
    CHECK(detail::locatedWithoutProbe(entry, id));
}

TEST_CASE("locatedWithoutProbe rejects a CPU that is neither answering nor located") {
    const CatalogEntry entry = findLegacyEntry();
    CpuIdentity id;
    id.mainPort = "COM60";   // DISPLAY is nowhere: still needs establishing
    CHECK_FALSE(detail::locatedWithoutProbe(entry, id));
}

TEST_CASE("locatedWithoutProbe does not count a PROBED volume as sufficient") {
    // A probe result is the thing this function decides whether to go and get.
    // Treating a previous one as sufficient here would silently turn this into
    // a freshness check, which belongs to verifiedVolumeFrom() and its clock.
    const CatalogEntry entry = findLegacyEntry();
    CpuIdentity id;
    id.mainPort      = "COM60";
    id.displayVolume = "H:/";
    id.displaySource = IdentitySource::VerifiedProbe;
    CHECK_FALSE(detail::locatedWithoutProbe(entry, id));
}

TEST_CASE("locatedWithoutProbe says no for an entry with nothing to flash") {
    // No steps means no located CPUs, not "vacuously all of them" -- a click
    // must not sail past into a dialog for a plan that cannot run.
    CatalogEntry entry;   // default-constructed: uf2 is empty
    CpuIdentity id;
    id.mainPort    = "COM60";
    id.displayPort = "COM65";
    CHECK_FALSE(detail::locatedWithoutProbe(entry, id));
}

TEST_CASE("autoIdentifyBlockedReason says what is wrong and what to do, and only when blocked") {
    const std::string reason = detail::autoIdentifyBlockedReason(2, 2);
    REQUIRE_FALSE(reason.empty());
    CHECK(reason.find("more than one FreeWili") != std::string::npos);
    CHECK(reason.find("Unplug") != std::string::npos);

    // Every non-blocked case reports nothing: a refusal message shown beside a
    // button that is not refusing would be worse than none.
    CHECK(detail::autoIdentifyBlockedReason(2, 1).empty());
    CHECK(detail::autoIdentifyBlockedReason(1, 5).empty());
    CHECK(detail::autoIdentifyBlockedReason(0, 0).empty());
}
