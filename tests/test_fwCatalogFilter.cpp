#include <doctest/doctest.h>
#include "catalog/fwCatalogFilter.h"
#include "flash/fwFlashPlan.h"

using namespace fwog;

namespace {
CatalogEntry make(std::string name, std::string cat,
                  std::vector<std::string> tags = {}, std::string author = "") {
    CatalogEntry e;
    e.slug = name; e.name = std::move(name); e.category = std::move(cat);
    e.tags = std::move(tags); e.author = std::move(author);
    return e;
}
std::vector<CatalogEntry> sample() {
    return { make("WiLi Blinky", "Examples", {"beginner","led"}, "Intrepid"),
             make("CAN Bus Sniffer", "Diagnostics", {"can"}, "Jane"),
             make("I2C Scanner", "Diagnostics", {"i2c"}, "Hobbyist") };
}
} // namespace

TEST_CASE("an empty query returns everything") {
    auto s = sample();
    CHECK(filterEntries(s, "", "").size() == 3);
}

TEST_CASE("the query matches names case-insensitively") {
    auto s = sample();
    auto r = filterEntries(s, "blinky", "");
    REQUIRE(r.size() == 1);
    CHECK(r[0]->name == "WiLi Blinky");
}

TEST_CASE("the query also matches tags and authors") {
    auto s = sample();
    CHECK(filterEntries(s, "i2c", "").size() == 1);
    CHECK(filterEntries(s, "jane", "").size() == 1);
}

TEST_CASE("a category filter narrows the list") {
    auto s = sample();
    CHECK(filterEntries(s, "", "Diagnostics").size() == 2);
}

TEST_CASE("query and category combine") {
    auto s = sample();
    CHECK(filterEntries(s, "scanner", "Diagnostics").size() == 1);
    CHECK(filterEntries(s, "blinky",  "Diagnostics").empty());
}

TEST_CASE("a query matching nothing returns nothing") {
    auto s = sample();
    CHECK(filterEntries(s, "zzzz", "").empty());
}

TEST_CASE("categories are unique and sorted") {
    auto s = sample();
    auto c = categoriesOf(s);
    REQUIRE(c.size() == 2);
    CHECK(c[0] == "Diagnostics");
    CHECK(c[1] == "Examples");
}

TEST_CASE("flashing is disabled when the entry has no assets") {
    CatalogEntry e = make("No assets", "Apps");
    DeviceView d; d.isOg = true;
    auto why = flashDisabledReason(e, &d);
    CHECK_FALSE(why.empty());
    CHECK(why.find("no firmware") != std::string::npos);
}

TEST_CASE("flashing is disabled when no device is connected") {
    CatalogEntry e = make("Has assets", "Apps");
    Uf2Asset a; a.ref.url = "u"; e.uf2.push_back(a);
    auto why = flashDisabledReason(e, nullptr);
    CHECK_FALSE(why.empty());
    CHECK(why.find("No FreeWili") != std::string::npos);
}

TEST_CASE("flashing is disabled for a device that is not a FreeWili OG") {
    // A FreeWili 2 has a different hub map and different flash paths. Offering
    // to flash it would be offering something this app cannot do correctly.
    CatalogEntry e = make("Has assets", "Apps");
    Uf2Asset a; a.ref.url = "u"; e.uf2.push_back(a);
    DeviceView d; d.isOg = false; d.name = "FreeWili 2";
    auto why = flashDisabledReason(e, &d);
    CHECK_FALSE(why.empty());
}

TEST_CASE("flashing is allowed with assets and a connected OG") {
    CatalogEntry e = make("Has assets", "Apps");
    Uf2Asset a; a.ref.url = "u"; e.uf2.push_back(a);
    DeviceView d; d.isOg = true;
    CHECK(flashDisabledReason(e, &d).empty());
}

// ---------------------------------------------------------------------------
// flashDisabledReason priority, pinned: every case above triggers exactly
// one condition, which does not prove the FIRST-applicable-reason ordering
// the header mandates. These construct entries where two blockers apply at
// once and assert the higher-priority message wins -- and that the
// lower-priority one is absent, so a future reordering of the early returns
// fails loudly rather than merely changing which substring a lenient check
// happens to still find.
// ---------------------------------------------------------------------------

TEST_CASE("no device beats an assetless entry -- the no-device reason wins") {
    CatalogEntry e = make("No assets", "Apps");   // also has no uf2 assets
    auto why = flashDisabledReason(e, nullptr);
    CHECK(why.find("No FreeWili") != std::string::npos);
    CHECK(why.find("no firmware") == std::string::npos);
}

TEST_CASE("a non-OG device beats an assetless entry -- the not-an-OG reason wins") {
    CatalogEntry e = make("No assets", "Apps");   // also has no uf2 assets
    DeviceView d; d.isOg = false; d.name = "FreeWili 2";
    auto why = flashDisabledReason(e, &d);
    CHECK(why.find("not a FreeWili OG") != std::string::npos);
    CHECK(why.find("no firmware") == std::string::npos);
}

// ---------------------------------------------------------------------------
// applyDisplayRetarget / displayRetargetConfirmed: the gated Unlisted-entry
// retarget is the one control in the App Explorer tab that can point a
// main-CPU image at the DISPLAY CPU. The transform and its arming
// precondition are pure and extracted specifically so they have direct
// regression coverage rather than being reachable only through ImGui widget
// interaction (which nothing here can drive).
// ---------------------------------------------------------------------------

namespace {
/// Same shape unlistedEntryFor() (fwCatalogLocal.cpp) produces: Unlisted,
/// schemeInferred, OgApp scheme, one MAIN asset.
CatalogEntry unlistedLike() {
    CatalogEntry e = make("mystery", "Unlisted");
    e.source = CatalogSource::Unlisted;
    e.schemeInferred = true;
    e.scheme = FlashScheme::OgApp;
    Uf2Asset a; a.cpu = TargetCpu::Main; a.ref.localPath = "mystery.uf2";
    e.uf2.push_back(a);
    return e;
}
} // namespace

TEST_CASE("applyDisplayRetarget points an Unlisted entry's asset at DISPLAY") {
    const auto retargeted = applyDisplayRetarget(unlistedLike());
    CHECK(retargeted.scheme == FlashScheme::DisplayBootloader);
    REQUIRE(retargeted.uf2.size() == 1);
    CHECK(retargeted.uf2[0].cpu == TargetCpu::Display);

    const auto plan = buildFlashPlan(retargeted);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].cpu == TargetCpu::Display);
}

TEST_CASE("applyDisplayRetarget does not mutate the original entry") {
    const CatalogEntry original = unlistedLike();
    const auto retargeted = applyDisplayRetarget(original);
    (void)retargeted;

    CHECK(original.scheme == FlashScheme::OgApp);
    REQUIRE(original.uf2.size() == 1);
    CHECK(original.uf2[0].cpu == TargetCpu::Main);
}

TEST_CASE("applyDisplayRetarget on an assetless entry still produces no plan") {
    CatalogEntry e = make("empty", "Unlisted");
    e.source = CatalogSource::Unlisted;
    e.schemeInferred = true;
    e.scheme = FlashScheme::OgApp;
    // no uf2 assets at all

    const auto retargeted = applyDisplayRetarget(e);
    CHECK(retargeted.scheme == FlashScheme::DisplayBootloader);
    CHECK(retargeted.uf2.empty());
    CHECK(buildFlashPlan(retargeted).empty());
}

TEST_CASE("displayRetargetConfirmed requires the exact DISPLAY confirmation") {
    CHECK(displayRetargetConfirmed("DISPLAY"));
    CHECK(displayRetargetConfirmed("  display  "));   // case/whitespace-insensitive, same as confirmationMatches
    CHECK_FALSE(displayRetargetConfirmed("MAIN"));
    CHECK_FALSE(displayRetargetConfirmed(""));
    CHECK_FALSE(displayRetargetConfirmed("DISPLAYS"));
}

// ---------------------------------------------------------------------------
// flashDisabledReason's identityConfirmed parameter (Task 22): a regression
// fix. A board that DeviceModel::selected() declines to return -- today, one
// whose fingerprint contradicts the recorded selection (see BoardFingerprint)
// -- used to be collapsed to `device = nullptr` by callers, producing "No
// FreeWili is connected." even while that board's row was still visible in the
// device bar -- worse than the refusal it replaced, because it is simply false.
// These pin the fix: the message must differ from the true no-device case,
// name the actual problem, and the refusal itself must still hold (empty
// string is never returned here).
// ---------------------------------------------------------------------------

TEST_CASE("an unconfirmed identity is refused with a message distinct from 'no device'") {
    CatalogEntry e = make("Has assets", "Apps");
    Uf2Asset a; a.ref.url = "u"; e.uf2.push_back(a);
    DeviceView d; d.isOg = true; d.serial = "Unknown";

    const auto noDevice = flashDisabledReason(e, nullptr);
    const auto unconfirmed = flashDisabledReason(e, &d, /*identityConfirmed=*/false);

    CHECK_FALSE(unconfirmed.empty());               // still refused
    CHECK(unconfirmed != noDevice);                  // not the same false statement
    CHECK(noDevice.find("No FreeWili") != std::string::npos);
    CHECK(unconfirmed.find("No FreeWili") == std::string::npos); // must not claim nothing is connected
    CHECK(unconfirmed.find("connected") != std::string::npos);   // ...but must say a board IS present
}

TEST_CASE("identityConfirmed defaults to true, matching every pre-Task-22 call site") {
    CatalogEntry e = make("Has assets", "Apps");
    Uf2Asset a; a.ref.url = "u"; e.uf2.push_back(a);
    DeviceView d; d.isOg = true;
    CHECK(flashDisabledReason(e, &d).empty()); // 2-arg call: identical to flashDisabledReason(e, &d, true)
}

TEST_CASE("a non-OG device beats an unconfirmed identity -- the not-an-OG reason still wins") {
    // isOg is still knowable from the raw candidate even when its serial did
    // not resolve, so that higher-priority reason must still take precedence.
    CatalogEntry e = make("Has assets", "Apps");
    Uf2Asset a; a.ref.url = "u"; e.uf2.push_back(a);
    DeviceView d; d.isOg = false; d.serial = "Unknown";
    auto why = flashDisabledReason(e, &d, /*identityConfirmed=*/false);
    CHECK(why.find("not a FreeWili OG") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Platform capability (Task 21). kDeviceSupportAvailable is a compile-time
// constant, so a test written only against flashDisabledReason() could assert
// nothing at all about the web build's behaviour when run on a desktop build
// -- the branch would simply be dead. flashDisabledReasonFor() takes the
// capability as a parameter precisely so BOTH orderings are exercised by
// every build's test run; the guarded case at the end then pins that
// flashDisabledReason() really does feed this build's own constant in.
// ---------------------------------------------------------------------------

TEST_CASE("a platform without device support names the platform, not the missing device") {
    CatalogEntry e = make("Has assets", "Apps");
    Uf2Asset a; a.ref.url = "u"; e.uf2.push_back(a);

    const auto why = flashDisabledReasonFor(/*deviceSupportAvailable=*/false, e, nullptr);
    CHECK_FALSE(why.empty());
    CHECK(why == std::string(platformLimitationNotice()));
    // The whole point of the check's position: a browser can never find a
    // board, so "No FreeWili is connected." would send the user hunting for
    // a cable fault that does not exist.
    CHECK(why.find("No FreeWili") == std::string::npos);
    CHECK(why.find("desktop app") != std::string::npos);
}

TEST_CASE("the platform reason wins over every other reason, no-device included") {
    // Every other blocker applies at once: no assets, no device / a non-OG
    // device whose identity never resolved. The platform reason must still be
    // the one returned, or the ordering has regressed.
    CatalogEntry e = make("No assets", "Apps");
    DeviceView d; d.isOg = false; d.serial = "Unknown";

    CHECK(flashDisabledReasonFor(false, e, nullptr)
          == std::string(platformLimitationNotice()));
    CHECK(flashDisabledReasonFor(false, e, &d, /*identityConfirmed=*/false)
          == std::string(platformLimitationNotice()));
}

TEST_CASE("with device support present the pre-existing ordering is untouched") {
    CatalogEntry withAssets = make("Has assets", "Apps");
    Uf2Asset a; a.ref.url = "u"; withAssets.uf2.push_back(a);
    CatalogEntry noAssets = make("No assets", "Apps");
    DeviceView og;    og.isOg = true;
    DeviceView notOg; notOg.isOg = false; notOg.name = "FreeWili 2";

    CHECK(flashDisabledReasonFor(true, withAssets, &og).empty());
    CHECK(flashDisabledReasonFor(true, withAssets, nullptr).find("No FreeWili") != std::string::npos);
    CHECK(flashDisabledReasonFor(true, withAssets, &notOg).find("not a FreeWili OG") != std::string::npos);
    CHECK(flashDisabledReasonFor(true, noAssets, &og).find("no firmware") != std::string::npos);
    CHECK(flashDisabledReasonFor(true, withAssets, &og, /*identityConfirmed=*/false)
              .find("contradicts the selection") != std::string::npos);
    // ...and none of them is ever the platform notice on a capable platform.
    CHECK(flashDisabledReasonFor(true, noAssets, nullptr) != std::string(platformLimitationNotice()));
}

TEST_CASE("flashDisabledReason feeds this build's own platform capability in") {
    CatalogEntry e = make("Has assets", "Apps");
    Uf2Asset a; a.ref.url = "u"; e.uf2.push_back(a);
    DeviceView d; d.isOg = true;

    if constexpr (!kDeviceSupportAvailable) {
        // Web build: nothing can be detected, so every answer collapses to
        // the platform notice -- including the case that would otherwise be
        // "flashing allowed".
        CHECK(flashDisabledReason(e, &d) == std::string(platformLimitationNotice()));
        CHECK(flashDisabledReason(e, nullptr) == std::string(platformLimitationNotice()));
    } else {
        // Desktop build: the platform check never fires and the reasons are
        // exactly what every case above this block already asserts.
        CHECK(flashDisabledReason(e, &d).empty());
        CHECK(flashDisabledReason(e, nullptr).find("No FreeWili") != std::string::npos);
    }
    // True either way, and the actual bridge being pinned: the two-argument
    // entry point must agree with the parameterised rule fed this build's
    // constant, not with some independently-drifting copy of it.
    CHECK(flashDisabledReason(e, &d)
          == flashDisabledReasonFor(kDeviceSupportAvailable, e, &d));
    CHECK(flashDisabledReason(e, nullptr)
          == flashDisabledReasonFor(kDeviceSupportAvailable, e, nullptr));
}

// ---------------------------------------------------------------------------
// excludeSlug: pure filter that keeps the destructive erase-MAIN catalog
// entry out of App Explorer's general browse-and-flash list. See its header
// comment (fwCatalogFilter.h) for why.
// ---------------------------------------------------------------------------

TEST_CASE("excludeSlug removes only the matching entry") {
    CatalogEntry a = make("A", "Apps"); a.slug = "keep-a";
    CatalogEntry b = make("B", "Apps"); b.slug = "drop-me";
    CatalogEntry c = make("C", "Apps"); c.slug = "keep-c";

    auto out = excludeSlug({ a, b, c }, "drop-me");
    REQUIRE(out.size() == 2);
    CHECK(out[0].slug == "keep-a");
    CHECK(out[1].slug == "keep-c");
}

TEST_CASE("excludeSlug is a no-op when nothing matches") {
    CatalogEntry a = make("A", "Apps"); a.slug = "keep-a";
    auto out = excludeSlug({ a }, "nonexistent-slug");
    REQUIRE(out.size() == 1);
    CHECK(out[0].slug == "keep-a");
}

TEST_CASE("excludeSlug on an empty list returns an empty list") {
    CHECK(excludeSlug({}, "anything").empty());
}

// ---------------------------------------------------------------------------
// onlyOgApps: the wiliOGbsp contract as a filter. An app is one UF2 written to
// the MAIN CPU; everything else is a board-level operation, not an app.

TEST_CASE("onlyOgApps keeps app images and drops board-level operations") {
    CatalogEntry app        = make("App", "Apps");   app.slug = "an-app";
    CatalogEntry bootloader = make("BL", "Apps");    bootloader.slug = "bootloader";
    CatalogEntry legacy     = make("Old", "Apps");   legacy.slug = "legacy";
    app.scheme        = FlashScheme::OgApp;
    bootloader.scheme = FlashScheme::DisplayBootloader;
    legacy.scheme     = FlashScheme::LegacyDirect;

    auto out = onlyOgApps({ bootloader, app, legacy });
    REQUIRE(out.size() == 1);
    CHECK(out[0].slug == "an-app");
}

TEST_CASE("onlyOgApps preserves the relative order of what it keeps") {
    CatalogEntry a = make("A", "Apps"); a.slug = "a"; a.scheme = FlashScheme::OgApp;
    CatalogEntry b = make("B", "Apps"); b.slug = "b"; b.scheme = FlashScheme::DisplayBootloader;
    CatalogEntry c = make("C", "Apps"); c.slug = "c"; c.scheme = FlashScheme::OgApp;

    auto out = onlyOgApps({ a, b, c });
    REQUIRE(out.size() == 2);
    CHECK(out[0].slug == "a");
    CHECK(out[1].slug == "c");
}

TEST_CASE("onlyOgApps does NOT remove the erase-MAIN entry") {
    // The erase-MAIN image is written to the MAIN CPU, so it is an OgApp by
    // scheme and survives this filter. It is destructive and typed-confirmation
    // gated, and it is excludeSlug()'s job -- not this one's -- to keep it out
    // of the browse list. A future refactor that "simplified" the two calls into
    // one would put an erase behind an ordinary Flash button.
    CatalogEntry erase = make("Erase MAIN", "Apps");
    erase.slug   = kEraseMainCpuSlug;
    erase.scheme = FlashScheme::OgApp;

    auto out = onlyOgApps({ erase });
    REQUIRE(out.size() == 1);
    CHECK(out[0].slug == kEraseMainCpuSlug);
    CHECK(excludeSlug(std::move(out), kEraseMainCpuSlug).empty());
}

TEST_CASE("onlyOgApps on an empty list returns an empty list") {
    CHECK(onlyOgApps({}).empty());
}

// ---------------------------------------------------------------------------
// mappedToTheWrongCpu: the refusal a verified CPU probe makes possible.
//
// This one REFUSES something that used to be permitted, and the case it
// refuses was dangerous: one foreign mounted volume plus a DISPLAY-only entry
// used to reach the engine's typed-confirmation prompt, and a user who
// correctly typed DISPLAY got a wrong-CPU write if that drive was in fact the
// MAIN CPU.
// ---------------------------------------------------------------------------

namespace {

CatalogEntry displayOnlyEntry() {
    CatalogEntry e = make("Display bootloader", "Firmware");
    e.scheme = FlashScheme::DisplayBootloader;
    Uf2Asset a; a.cpu = TargetCpu::Display; a.ref.embeddedId = "bl_display";
    e.uf2.push_back(a);
    return e;
}

CatalogEntry mainOnlyEntry() {
    CatalogEntry e = make("An app", "Apps");
    e.scheme = FlashScheme::OgApp;
    Uf2Asset a; a.cpu = TargetCpu::Main; a.ref.embeddedId = "blinky";
    e.uf2.push_back(a);
    return e;
}

CatalogEntry bothCpusEntry() {
    CatalogEntry e = make("Original FreeWili", "Firmware");
    e.scheme = FlashScheme::LegacyDirect;
    Uf2Asset m; m.cpu = TargetCpu::Main;    m.ref.embeddedId = "main";    m.order = 1;
    Uf2Asset d; d.cpu = TargetCpu::Display; d.ref.embeddedId = "display"; d.order = 0;
    e.uf2 = { m, d };
    return e;
}

CpuIdentity probedAs(const char* volume, TargetCpu cpu) {
    CpuIdentity id;
    if (cpu == TargetCpu::Main) { id.mainVolume = volume; id.mainSource = IdentitySource::VerifiedProbe; }
    else { id.displayVolume = volume; id.displaySource = IdentitySource::VerifiedProbe; }
    return id;
}

} // namespace

TEST_CASE("with no probe result this rule has no opinion whatsoever") {
    // Which is every call this app made before probe mappings existed, and
    // every call it makes until somebody runs the Recovery tab's action.
    CHECK(mappedToTheWrongCpu(displayOnlyEntry(), CpuIdentity{}).empty());
    CHECK(mappedToTheWrongCpu(mainOnlyEntry(), CpuIdentity{}).empty());
    CHECK(mappedToTheWrongCpu(bothCpusEntry(), CpuIdentity{}).empty());
}

TEST_CASE("a DISPLAY-only entry is refused while the mounted drive is measurably MAIN") {
    const std::string why = mappedToTheWrongCpu(displayOnlyEntry(), probedAs("G:/", TargetCpu::Main));
    REQUIRE_FALSE(why.empty());
    CHECK(why.find("G:/") != std::string::npos);
    CHECK(why.find("MAIN") != std::string::npos);
    CHECK(why.find("DISPLAY") != std::string::npos);

    // ...and it reaches the Flash button, not just this helper.
    CatalogEntry e = displayOnlyEntry();
    DeviceView d; d.isOg = true; d.serial = "FW6548";
    d.identity = probedAs("G:/", TargetCpu::Main);
    CHECK(flashDisabledReason(e, &d) == why);
}

TEST_CASE("the same entry is allowed when the mounted drive is the CPU it writes") {
    CHECK(mappedToTheWrongCpu(displayOnlyEntry(), probedAs("G:/", TargetCpu::Display)).empty());
    CHECK(mappedToTheWrongCpu(mainOnlyEntry(), probedAs("G:/", TargetCpu::Main)).empty());

    CatalogEntry e = mainOnlyEntry();
    DeviceView d; d.isOg = true; d.serial = "FW6548";
    d.identity = probedAs("G:/", TargetCpu::Main);
    CHECK(flashDisabledReason(e, &d).empty());   // this is the unblocking, at the button
}

TEST_CASE("an entry writing BOTH CPUs is never refused on one step's account") {
    // The deprecated-firmware install writes DISPLAY and then MAIN. Whichever
    // CPU the mounted drive turns out to be, one of its steps goes there, and
    // the others have ordinary paths the engine judges when it reaches them.
    // Refusing the whole entry here would block the install this recovery flow
    // exists to make possible.
    CHECK(mappedToTheWrongCpu(bothCpusEntry(), probedAs("G:/", TargetCpu::Main)).empty());
    CHECK(mappedToTheWrongCpu(bothCpusEntry(), probedAs("G:/", TargetCpu::Display)).empty());
}

TEST_CASE("the scheme decides which CPUs count, not the raw asset list") {
    // An OgApp entry carrying a stray DISPLAY asset writes MAIN only --
    // schemeAllows() drops the other -- so it must be judged on the plan, and
    // refused while the mounted drive is measurably DISPLAY.
    CatalogEntry e = mainOnlyEntry();
    Uf2Asset sneaky; sneaky.cpu = TargetCpu::Display; sneaky.ref.embeddedId = "sneaky";
    e.uf2.push_back(sneaky);
    REQUIRE(buildFlashPlan(e).size() == 1);   // the stray asset really is dropped

    CHECK_FALSE(mappedToTheWrongCpu(e, probedAs("G:/", TargetCpu::Display)).empty());
    CHECK(mappedToTheWrongCpu(e, probedAs("G:/", TargetCpu::Main)).empty());
}

TEST_CASE("an entry with no plan is left to the earlier, better 'no firmware' answer") {
    CatalogEntry e = make("Empty", "Apps");
    CHECK(mappedToTheWrongCpu(e, probedAs("G:/", TargetCpu::Main)).empty());

    DeviceView d; d.isOg = true; d.serial = "FW6548";
    d.identity = probedAs("G:/", TargetCpu::Main);
    CHECK(flashDisabledReason(e, &d).find("no firmware") != std::string::npos);
}
