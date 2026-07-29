#include <doctest/doctest.h>
#include "catalog/fwCatalogEmbedded.h"
#include "catalog/fwCatalogFilter.h"
#include "catalog/fwUf2Header.h"
#include "core/fwSha256.h"
#include "flash/fwFlashPlan.h"
#include "fwEmbeddedFirmware.h"   // generated; kImages exposes deflated bytes for the corruption tests below

#include <span>
#include <string>

using namespace fwog;

TEST_CASE("every embedded entry declares a scheme and one asset") {
    for (const auto& e : embeddedEntries()) {
        CAPTURE(e.slug);
        CHECK(e.source == CatalogSource::Embedded);
        CHECK_FALSE(e.slug.empty());
        CHECK_FALSE(e.name.empty());
        CHECK(e.schemeInferred == false);   // embedded schemes are declared
        CHECK_FALSE(e.uf2.empty());
        for (const auto& a : e.uf2)
            CHECK_FALSE(a.ref.embeddedId.empty());
    }
}

TEST_CASE("every embedded image inflates, and matches its recorded size and hash") {
    // This runs in CI and catches a corrupted embedding before a board does.
    for (const auto& e : embeddedEntries()) {
        for (const auto& a : e.uf2) {
            CAPTURE(a.ref.embeddedId);
            auto bytes = loadEmbeddedImage(a.ref.embeddedId);
            REQUIRE(bytes.has_value());
            CHECK(bytes->size() == a.size);
            CHECK(sha256Hex(*bytes) == a.sha256);
        }
    }
}

TEST_CASE("every embedded image is a valid RP2040 UF2") {
    for (const auto& e : embeddedEntries()) {
        for (const auto& a : e.uf2) {
            CAPTURE(a.ref.embeddedId);
            auto bytes = loadEmbeddedImage(a.ref.embeddedId);
            REQUIRE(bytes.has_value());
            auto info = parseUf2(*bytes);
            CHECK(info.has_value());
        }
    }
}

TEST_CASE("an unknown embedded id is an error, not a crash") {
    auto r = loadEmbeddedImage("no-such-image");
    REQUIRE_FALSE(r.has_value());
    CHECK_FALSE(r.error().empty());
}

TEST_CASE("the display bootloader reports a version") {
    // "make sure to show version" -- the Default Firmware tab renders this.
    //
    // Scoped to the INSTALL entry, not to every DisplayBootloader entry: the
    // standalone erase-DISPLAY action carries that scheme too (it is what pins
    // it to the DISPLAY CPU), and its "version" is the erase image's
    // "pico-flash-nuke", which is not a firmware version and is not what this
    // is about. Matched the same way the tab itself picks the card to draw --
    // by having a non-erase asset -- so the two cannot disagree.
    bool foundBootloader = false;
    for (const auto& e : embeddedEntries()) {
        if (e.scheme != FlashScheme::DisplayBootloader) continue;
        bool installsFirmware = false;
        for (const auto& a : e.uf2)
            if (!isEraseAsset(a)) installsFirmware = true;
        if (!installsFirmware) continue;
        foundBootloader = true;
        CHECK_FALSE(e.version.empty());
        CHECK_FALSE(embeddedVersion(e.uf2[0].ref.embeddedId).empty());
        CHECK(e.slug != kEraseDisplayCpuSlug);
    }
    CHECK(foundBootloader);
}

// The three tests above prove the compiled-in table round-trips correctly,
// but the table is always valid by construction -- they never exercise the
// two failure branches that matter most for a *corrupted* embedding:
// decompression itself failing, and decompression succeeding but producing
// the wrong number of bytes. loadEmbeddedImage(id) only ever looks up into
// the always-valid table, so there is no way to hand it bad bytes through
// its public, id-based signature. detail::inflateAndVerifySize (declared in
// fwCatalogEmbedded.h) is the pure decompress step loadEmbeddedImage
// delegates to; driving it directly with corrupted/truncated copies of a
// real embedded image's deflated bytes is how those two branches get
// covered without contorting loadEmbeddedImage's contract.

TEST_CASE("a corrupted deflate stream fails to decompress rather than returning partial data") {
    REQUIRE(generated::kImageCount > 0);
    const auto& img = generated::kImages[0];
    REQUIRE(img.deflatedSize > 8);

    std::vector<uint8_t> corrupted(img.deflated, img.deflated + img.deflatedSize);
    // Flip the zlib header and a run of bytes early in the stream -- deep
    // enough that mz_uncompress cannot just resync and decode anyway.
    for (size_t i = 0; i < 8; ++i) corrupted[i] ^= 0xFF;

    auto r = detail::inflateAndVerifySize(corrupted, img.rawSize);
    REQUIRE_FALSE(r.has_value());
    CHECK_FALSE(r.error().empty());
}

TEST_CASE("a truncated deflate stream fails to decompress rather than returning partial data") {
    REQUIRE(generated::kImageCount > 0);
    const auto& img = generated::kImages[0];
    REQUIRE(img.deflatedSize > 16);

    // Cut it off mid-stream: valid prefix, no end-of-stream marker.
    std::vector<uint8_t> truncated(img.deflated, img.deflated + img.deflatedSize / 2);

    auto r = detail::inflateAndVerifySize(truncated, img.rawSize);
    REQUIRE_FALSE(r.has_value());
    CHECK_FALSE(r.error().empty());
}

TEST_CASE("a recorded size that does not match the decompressed output is rejected") {
    // Exercises the branch decompression-failure tests above cannot reach:
    // rc == MZ_OK (the bytes are genuinely valid and decode cleanly), but
    // the caller-supplied size disagrees with what actually came out --
    // exactly what a hand-edited or bit-rotted manifest entry would produce.
    REQUIRE(generated::kImageCount > 0);
    const auto& img = generated::kImages[0];

    std::span<const uint8_t> deflated(img.deflated, img.deflatedSize);
    auto r = detail::inflateAndVerifySize(deflated, img.rawSize + 1024);
    REQUIRE_FALSE(r.has_value());
    CHECK_FALSE(r.error().empty());
}

// ---------------------------------------------------------------------------
// The erase-MAIN entry's REAL manifest data (Task 22 fix round, Important):
// every other erase-MAIN test in this codebase (test_fwFlashPlan.cpp,
// test_fwCatalogFilter.cpp, test_fwTabDefaultFirmwareLogic.cpp) builds a
// synthetic CatalogEntry from kEraseMainCpuSlug itself, which proves those
// functions behave correctly GIVEN such an entry but proves nothing about
// firmware/manifest.json actually containing one -- a typo'd or renamed
// "slug" there would silently make the Danger zone card vanish ("No
// firmware was embedded in this build") on the Default Firmware tab AND let
// the real destructive entry leak into App Explorer's ordinary Flash-button
// list, and neither of the synthetic-entry tests would notice. This is the
// one place that reads the actual compiled-in catalog (embeddedEntries(),
// same source the other tests in this file already exercise) and ties it to
// kEraseMainCpuSlug and excludeSlug() together.
// ---------------------------------------------------------------------------

TEST_CASE("the real embedded catalog contains the erase-MAIN entry, OgApp scheme, one MAIN asset") {
    // embeddedEntries() returns by value: captured into a named vector that
    // outlives the loop below, so `found` (a pointer into it) stays valid
    // for the CHECKs after the loop. Taking `&e` from a range-for over a
    // call to embeddedEntries() directly would dangle -- the range-for's
    // lifetime extension keeps the temporary alive only for the loop itself,
    // not past it.
    const std::vector<CatalogEntry> entries = embeddedEntries();
    const CatalogEntry* found = nullptr;
    for (const auto& e : entries)
        if (e.slug == kEraseMainCpuSlug) found = &e;

    REQUIRE(found != nullptr);
    CHECK(found->scheme == FlashScheme::OgApp);
    REQUIRE(found->uf2.size() == 1);
    CHECK(found->uf2[0].cpu == TargetCpu::Main);
}

TEST_CASE("the real embedded catalog contains the erase-DISPLAY entry, DisplayBootloader scheme, one DISPLAY asset") {
    // The sibling of the test above, and pinned for the same reason: a typo'd
    // or renamed slug here would make the Danger zone's second card vanish
    // ("no firmware was embedded in this build") AND -- worse -- would take
    // eraseAllowsCpu()'s slug exception with it, since that exception is keyed
    // on this exact string. The entry would then produce an EMPTY plan and a
    // button that silently does nothing.
    //
    // The scheme is what pins it to DISPLAY: FlashScheme::DisplayBootloader
    // permits only TargetCpu::Display, exactly as OgApp permits only Main for
    // the erase-MAIN entry. test_fwFlashPlan.cpp pins the consequence.
    const std::vector<CatalogEntry> entries = embeddedEntries();
    const CatalogEntry* found = nullptr;
    for (const auto& e : entries)
        if (e.slug == kEraseDisplayCpuSlug) found = &e;

    REQUIRE(found != nullptr);
    CHECK(found->scheme == FlashScheme::DisplayBootloader);
    REQUIRE(found->uf2.size() == 1);
    CHECK(found->uf2[0].cpu == TargetCpu::Display);
    CHECK(found->uf2[0].ref.embeddedId == kFlashNukeImageId);
}

TEST_CASE("the two erase entries are distinct entries, one per CPU, sharing nothing but the image") {
    // They are two entries rather than one retargetable entry ON PURPOSE: that
    // is what lets each one's SCHEME pin it to a single CPU, which no catalog
    // and no UI control can restate. This asserts they really are separate and
    // really do disagree about the CPU -- a future "simplification" that merged
    // them would fail here rather than on a board.
    const std::vector<CatalogEntry> entries = embeddedEntries();
    const CatalogEntry* main = nullptr;
    const CatalogEntry* disp = nullptr;
    for (const auto& e : entries) {
        if (e.slug == kEraseMainCpuSlug)    main = &e;
        if (e.slug == kEraseDisplayCpuSlug) disp = &e;
    }
    REQUIRE(main != nullptr);
    REQUIRE(disp != nullptr);
    CHECK(main != disp);
    CHECK(main->scheme != disp->scheme);
    REQUIRE(main->uf2.size() == 1);
    REQUIRE(disp->uf2.size() == 1);
    CHECK(main->uf2[0].cpu != disp->uf2[0].cpu);
    // The one thing they DO share, because flash_nuke.uf2 is CPU-agnostic.
    CHECK(main->uf2[0].ref.embeddedId == disp->uf2[0].ref.embeddedId);
}

TEST_CASE("excludeSlug removes the real erase-DISPLAY entry from the real embedded catalog") {
    // App Explorer browses an ordinary Flash button; neither erase entry may
    // ever appear behind one. fwApp.cpp excludes both slugs, and this pins the
    // second half of that pair the way the test below pins the first.
    std::vector<CatalogEntry> entries = embeddedEntries();
    const std::size_t before = entries.size();

    bool presentBefore = false;
    for (const auto& e : entries)
        if (e.slug == kEraseDisplayCpuSlug) presentBefore = true;
    REQUIRE(presentBefore);   // otherwise this test would pass vacuously

    auto filtered = excludeSlug(std::move(entries), kEraseDisplayCpuSlug);
    CHECK(filtered.size() == before - 1);
    for (const auto& e : filtered)
        CHECK(e.slug != kEraseDisplayCpuSlug);
}

TEST_CASE("excludeSlug removes the real erase-MAIN entry from the real embedded catalog") {
    // embeddedEntries() returns by value (a fresh std::vector each call), so
    // it must be captured once -- constructing from
    // embeddedEntries().begin()/embeddedEntries().end() would mix iterators
    // from two different temporary vectors.
    std::vector<CatalogEntry> entries = embeddedEntries();
    const std::size_t before = entries.size();

    bool presentBefore = false;
    for (const auto& e : entries)
        if (e.slug == kEraseMainCpuSlug) presentBefore = true;
    REQUIRE(presentBefore);   // otherwise this test would pass vacuously

    auto filtered = excludeSlug(std::move(entries), kEraseMainCpuSlug);
    CHECK(filtered.size() == before - 1);
    for (const auto& e : filtered)
        CHECK(e.slug != kEraseMainCpuSlug);
}

TEST_CASE("the deprecated firmware entry uses LegacyDirect with both CPUs and a DISPLAY erase") {
    // Three assets since the DISPLAY-first reordering: the flash-erase image
    // for the DISPLAY CPU, the display firmware, and the main firmware.
    // buildFlashPlan() is what fixes their SEQUENCE (test_fwFlashPlan.cpp);
    // this checks the shipped manifest actually carries the assets that
    // sequence is built from.
    bool found = false;
    for (const auto& e : embeddedEntries()) {
        if (e.scheme != FlashScheme::LegacyDirect) continue;
        found = true;
        REQUIRE(e.uf2.size() == 3);
        bool hasMain = false, hasDisplay = false, hasDisplayErase = false;
        for (const auto& a : e.uf2) {
            if (isEraseAsset(a)) {
                // The erase must target DISPLAY here. On MAIN it would silence
                // the CPU this plan needs running last, and it is not what the
                // owner asked for.
                CHECK(a.cpu == TargetCpu::Display);
                hasDisplayErase = true;
                continue;
            }
            if (a.cpu == TargetCpu::Main)    hasMain = true;
            if (a.cpu == TargetCpu::Display) hasDisplay = true;
        }
        CHECK(hasMain);
        CHECK(hasDisplay);
        CHECK(hasDisplayErase);
    }
    CHECK(found);
}

TEST_CASE("the flash-erase image is embedded once and shared by every entry that uses it") {
    // flash_nuke is referenced by erase-main-cpu, erase-display-cpu AND the
    // deprecated firmware entry. tools/embed_firmware.py emits one blob per
    // "images" entry and entries only reference it by id, so a shared asset
    // costs nothing and resolves for all three -- but nothing else proves that,
    // and a duplicated or dropped blob would only show up on a board.
    const std::vector<CatalogEntry> entries = embeddedEntries();
    int referencing = 0;
    for (const auto& e : entries)
        for (const auto& a : e.uf2)
            if (a.ref.embeddedId == kFlashNukeImageId) { ++referencing; break; }
    // FOUR entries now, not three: the display-bootloader install gained a MAIN
    // erase (it silences MAIN before writing the DISPLAY bootloader), joining
    // the legacy entry and the two standalone erase entries. Still ONE blob.
    CHECK(referencing == 4);

    int blobs = 0;
    for (std::size_t k = 0; k < generated::kImageCount; ++k)
        if (std::string(generated::kImages[k].id) == kFlashNukeImageId) ++blobs;
    CHECK(blobs == 1);

    // And it still resolves, from both entries' point of view.
    auto bytes = loadEmbeddedImage(kFlashNukeImageId);
    REQUIRE(bytes.has_value());
    CHECK_FALSE(bytes->empty());
}
