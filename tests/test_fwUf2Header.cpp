#include <doctest/doctest.h>
#include "catalog/fwUf2Header.h"

#include <cstring>
#include <vector>

using namespace fwog;

namespace {

constexpr uint32_t kMagic0   = 0x0A324655;
constexpr uint32_t kMagic1   = 0x9E5D5157;
constexpr uint32_t kMagicEnd = 0x0AB16F30;
constexpr uint32_t kRp2040   = 0xE48BFF56;
constexpr uint32_t kFamilyPresent = 0x00002000;

void put32(std::vector<uint8_t>& v, size_t off, uint32_t val) {
    v[off + 0] = uint8_t(val & 0xFF);
    v[off + 1] = uint8_t((val >> 8) & 0xFF);
    v[off + 2] = uint8_t((val >> 16) & 0xFF);
    v[off + 3] = uint8_t((val >> 24) & 0xFF);
}

/// Build a syntactically valid UF2 of `blocks` blocks.
std::vector<uint8_t> makeUf2(uint32_t blocks,
                             uint32_t familyId = kRp2040,
                             uint32_t flags = kFamilyPresent)
{
    std::vector<uint8_t> v(size_t(blocks) * 512, 0);
    for (uint32_t i = 0; i < blocks; ++i) {
        size_t b = size_t(i) * 512;
        put32(v, b + 0,  kMagic0);
        put32(v, b + 4,  kMagic1);
        put32(v, b + 8,  flags);
        put32(v, b + 12, 0x10000000 + i * 256);  // targetAddr
        put32(v, b + 16, 256);                   // payloadSize
        put32(v, b + 20, i);                     // blockNo
        put32(v, b + 24, blocks);                // numBlocks
        put32(v, b + 28, familyId);
        put32(v, b + 508, kMagicEnd);
    }
    return v;
}

} // namespace

TEST_CASE("a valid RP2040 UF2 parses") {
    auto v = makeUf2(4);
    auto r = parseUf2(v);
    REQUIRE(r.has_value());
    CHECK(r->familyPresent == true);
    CHECK(r->familyId == kRp2040);
    CHECK(r->numBlocks == 4);
    CHECK(r->targetAddr == 0x10000000);
    CHECK(r->payloadBytes == 4 * 256);
}

TEST_CASE("empty input is rejected") {
    auto r = parseUf2({});
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == Uf2Error::Empty);
}

TEST_CASE("a size that is not a multiple of 512 is rejected") {
    auto v = makeUf2(2);
    v.resize(v.size() - 8);          // truncated mid-block
    auto r = parseUf2(v);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == Uf2Error::BadSize);
}

TEST_CASE("wrong magic is rejected") {
    auto v = makeUf2(2);
    put32(v, 0, 0xDEADBEEF);
    auto r = parseUf2(v);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == Uf2Error::BadMagic);
}

TEST_CASE("a missing end magic is rejected") {
    auto v = makeUf2(2);
    put32(v, 508, 0);
    auto r = parseUf2(v);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == Uf2Error::BadMagic);
}

TEST_CASE("a non-RP2040 family is rejected") {
    // 0xE48BFF59 is RP2350 ARM-S. Writing it to an OG would be a mistake, so
    // this is the check that matters most in this file.
    auto v = makeUf2(2, 0xE48BFF59);
    auto r = parseUf2(v);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == Uf2Error::WrongFamily);
}

TEST_CASE("no family ID present is accepted with familyPresent false") {
    // The deprecated original images predate universal family tagging.
    // Refusing them would break the LegacyDirect path, so they are allowed
    // through and the UI warns instead.
    auto v = makeUf2(2, 0, /*flags=*/0);
    auto r = parseUf2(v);
    REQUIRE(r.has_value());
    CHECK(r->familyPresent == false);
    CHECK(r->familyId == 0);
}

TEST_CASE("a spliced file with a different family later is rejected") {
    // Block 0 is a legitimate RP2040 block; block 2 carries RP2350 ARM-S
    // (0xE48BFF59). Only checking block 0 would let this through -- this is
    // the exact splice the gate exists to catch.
    auto v = makeUf2(3);
    put32(v, 2 * 512 + 28, 0xE48BFF59);
    auto r = parseUf2(v);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == Uf2Error::WrongFamily);
}

TEST_CASE("a family ID appearing only in a later block is rejected") {
    // Block 0 has no family tag (as the deprecated originals do), but block 1
    // suddenly claims one. Block 0 set the expectation of "no family ID";
    // block 1 must honor it too.
    auto v = makeUf2(3, 0, /*flags=*/0);
    put32(v, 512 + 8,  kFamilyPresent);
    put32(v, 512 + 28, kRp2040);
    auto r = parseUf2(v);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == Uf2Error::WrongFamily);
}

TEST_CASE("no family ID in any block is still accepted") {
    // Regression guard for the LegacyDirect path: an untagged image must
    // stay accepted end to end, not just at block 0.
    auto v = makeUf2(3, 0, /*flags=*/0);
    auto r = parseUf2(v);
    REQUIRE(r.has_value());
    CHECK(r->familyPresent == false);
    CHECK(r->familyId == 0);
}

TEST_CASE("a single-block UF2 parses") {
    // The smallest legal input -- 512 bytes -- and a natural off-by-one
    // boundary; every other test uses 2-4 blocks.
    auto v = makeUf2(1);
    auto r = parseUf2(v);
    REQUIRE(r.has_value());
    CHECK(r->numBlocks == 1);
    CHECK(r->familyPresent == true);
    CHECK(r->payloadBytes == 256);
}

TEST_CASE("uf2ErrorMessage returns non-empty, distinct messages") {
    auto empty       = uf2ErrorMessage(Uf2Error::Empty);
    auto wrongFamily = uf2ErrorMessage(Uf2Error::WrongFamily);
    CHECK_FALSE(empty.empty());
    CHECK_FALSE(wrongFamily.empty());
    CHECK(empty != wrongFamily);
}

TEST_CASE("non-sequential block numbers are rejected") {
    auto v = makeUf2(3);
    put32(v, 512 + 20, 7);           // block 1 claims to be block 7
    auto r = parseUf2(v);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == Uf2Error::BadBlockOrder);
}

TEST_CASE("numBlocks disagreeing with the file size is rejected") {
    auto v = makeUf2(3);
    for (uint32_t i = 0; i < 3; ++i) put32(v, size_t(i) * 512 + 24, 9);
    auto r = parseUf2(v);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == Uf2Error::BlockCountMismatch);
}

TEST_CASE("a payloadSize larger than the block is rejected") {
    auto v = makeUf2(2);
    put32(v, 16, 999);
    auto r = parseUf2(v);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == Uf2Error::BadPayloadSize);
}
