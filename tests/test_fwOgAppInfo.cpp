#include <doctest/doctest.h>
#include "catalog/fwOgAppInfo.h"

#include <cstring>
#include <string>
#include <vector>

using namespace fwog;

namespace {

/// Build one fwog_uf2_info_t record at the layout fwOgAppInfo.h documents.
std::vector<uint8_t> makeRecord(uint16_t cpu, const std::string& name,
                                const std::string& description = "",
                                const std::string& build = "",
                                uint32_t version = 1, uint16_t format = 1,
                                uint32_t buildTs = 0, uint32_t crc = 0)
{
    std::vector<uint8_t> r(kOgAppInfoSize, 0);
    std::memcpy(r.data(), kOgAppInfoMagic, sizeof(kOgAppInfoMagic));
    const auto put16 = [&](std::size_t off, uint16_t v) {
        r[off] = uint8_t(v); r[off + 1] = uint8_t(v >> 8);
    };
    const auto put32 = [&](std::size_t off, uint32_t v) {
        r[off] = uint8_t(v);       r[off + 1] = uint8_t(v >> 8);
        r[off + 2] = uint8_t(v >> 16); r[off + 3] = uint8_t(v >> 24);
    };
    const auto putStr = [&](std::size_t off, std::size_t len, const std::string& s) {
        std::memcpy(r.data() + off, s.data(), s.size() < len ? s.size() : len);
    };
    put16(0x08, format);
    put16(0x0a, cpu);
    put32(0x0c, version);
    putStr(0x10, 32, name);
    putStr(0x30, 128, description);
    putStr(0xb0, 32, build);
    put32(0xd0, buildTs);
    put32(0xd4, crc);
    return r;
}

/// Wrap `payload` into UF2 blocks the way a real image does, so the flatten
/// step is exercised rather than bypassed.
std::vector<uint8_t> makeUf2(const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> out;
    const std::size_t chunk = 256;   // deliberately not 476: records straddle blocks
    const std::size_t blocks = (payload.size() + chunk - 1) / chunk;
    for (std::size_t b = 0; b < blocks; ++b) {
        std::vector<uint8_t> blk(512, 0);
        const auto put32 = [&](std::size_t off, uint32_t v) {
            blk[off] = uint8_t(v);       blk[off + 1] = uint8_t(v >> 8);
            blk[off + 2] = uint8_t(v >> 16); blk[off + 3] = uint8_t(v >> 24);
        };
        const std::size_t take = (b + 1) * chunk <= payload.size()
                                   ? chunk : payload.size() - b * chunk;
        put32(0, 0x0A324655);
        put32(4, 0x9E5D5157);
        put32(12, uint32_t(0x10000000 + b * chunk));
        put32(16, uint32_t(take));
        put32(20, uint32_t(b));
        put32(24, uint32_t(blocks));
        std::memcpy(blk.data() + 32, payload.data() + b * chunk, take);
        out.insert(out.end(), blk.begin(), blk.end());
    }
    return out;
}

} // namespace

TEST_CASE("a main image's two records are both found, and told apart by CPU") {
    // The shape the contract mandates: a main UF2 carries its own record AND
    // one for the display image embedded inside it.
    std::vector<uint8_t> payload(4096, 0);
    auto display = makeRecord(0, "ogvegas", "OG Vegas startup image");
    auto main    = makeRecord(1, "ogvegas", "OG Vegas main app", "6c49d12-dirty",
                              1, 1, 0x6a6998ae, 0x970b9e12);
    std::memcpy(payload.data() + 512,  display.data(), display.size());
    std::memcpy(payload.data() + 2048, main.data(),    main.size());

    const auto flat = flattenUf2Payload(makeUf2(payload));
    REQUIRE_FALSE(flat.empty());
    const auto found = findOgAppInfo(flat);
    REQUIRE(found.size() == 2);

    const auto d = ogAppInfoFor(found, TargetCpu::Display);
    const auto m = ogAppInfoFor(found, TargetCpu::Main);
    REQUIRE(d.has_value());
    REQUIRE(m.has_value());

    CHECK(d->name == "ogvegas");
    CHECK(d->description == "OG Vegas startup image");
    // The display half carries no build identity, by design -- the bootloader
    // decides whether to transfer it by comparing crc32.
    CHECK(d->build.empty());
    CHECK(d->buildTimestamp == 0);

    CHECK(m->name == "ogvegas");
    CHECK(m->build == "6c49d12-dirty");
    CHECK(m->buildTimestamp == 0x6a6998ae);
    CHECK(m->crc32 == 0x970b9e12);
}

TEST_CASE("a record straddling a UF2 block boundary is still found") {
    // The reason the search runs on the FLATTENED payload rather than on the
    // .uf2 file's bytes: a 216-byte record does not fit inside one block's
    // payload at an arbitrary offset, and searching the raw file would miss it.
    std::vector<uint8_t> payload(2048, 0);
    auto rec = makeRecord(1, "straddler", "crosses a block edge");
    std::memcpy(payload.data() + 200, rec.data(), rec.size());   // 200..416, over the 256 edge

    const auto found = findOgAppInfo(flattenUf2Payload(makeUf2(payload)));
    REQUIRE(found.size() == 1);
    CHECK(found[0].name == "straddler");
}

TEST_CASE("a record of an unknown layout version is skipped, not guessed at") {
    // Rendering a future record with today's offsets would put confident
    // nonsense on screen next to a Flash button.
    std::vector<uint8_t> payload(1024, 0);
    auto rec = makeRecord(1, "future", "", "", 1, /*format=*/2);
    std::memcpy(payload.data() + 64, rec.data(), rec.size());

    CHECK(findOgAppInfo(flattenUf2Payload(makeUf2(payload))).empty());
}

TEST_CASE("a record naming a CPU the contract does not define is skipped") {
    std::vector<uint8_t> payload(1024, 0);
    auto rec = makeRecord(7, "martian");
    std::memcpy(payload.data() + 64, rec.data(), rec.size());

    CHECK(findOgAppInfo(flattenUf2Payload(makeUf2(payload))).empty());
}

TEST_CASE("a nameless record is not accepted as an identity") {
    // Eight bytes reading FWGOINFO can occur inside ordinary data. A record
    // with no name is far more likely to be that than a real one.
    std::vector<uint8_t> payload(1024, 0);
    auto rec = makeRecord(1, "");
    std::memcpy(payload.data() + 64, rec.data(), rec.size());

    CHECK(findOgAppInfo(flattenUf2Payload(makeUf2(payload))).empty());
}

TEST_CASE("the magic appearing in the last bytes of an image cannot read past the end") {
    // The bounds guarantee: a truncated record at the very end must be ignored,
    // not read off the end of the buffer.
    std::vector<uint8_t> flat(64, 0);
    std::memcpy(flat.data() + 56, kOgAppInfoMagic, sizeof(kOgAppInfoMagic));
    CHECK(findOgAppInfo(flat).empty());
}

TEST_CASE("an unterminated fixed field is truncated at the field's end") {
    std::vector<uint8_t> payload(1024, 0);
    auto rec = makeRecord(1, std::string(40, 'x'));   // longer than name[32]
    std::memcpy(payload.data() + 64, rec.data(), rec.size());

    const auto found = findOgAppInfo(flattenUf2Payload(makeUf2(payload)));
    REQUIRE(found.size() == 1);
    CHECK(found[0].name == std::string(32, 'x'));
}

TEST_CASE("an image carrying no records is not an error") {
    std::vector<uint8_t> payload(1024, 0x5a);
    CHECK(findOgAppInfo(flattenUf2Payload(makeUf2(payload))).empty());
}

TEST_CASE("flattenUf2Payload rejects a file that is not whole blocks") {
    std::vector<uint8_t> junk(700, 0);
    CHECK(flattenUf2Payload(junk).empty());
    CHECK(flattenUf2Payload({}).empty());
}

TEST_CASE("flattenUf2Payload returns nothing when no block carries the UF2 magic") {
    std::vector<uint8_t> blocks(1024, 0);
    CHECK(flattenUf2Payload(blocks).empty());
}

TEST_CASE("a block claiming an oversized payload is skipped rather than read past") {
    std::vector<uint8_t> blk(512, 0);
    const auto put32 = [&](std::size_t off, uint32_t v) {
        blk[off] = uint8_t(v);       blk[off + 1] = uint8_t(v >> 8);
        blk[off + 2] = uint8_t(v >> 16); blk[off + 3] = uint8_t(v >> 24);
    };
    put32(0, 0x0A324655);
    put32(4, 0x9E5D5157);
    put32(16, 9999);   // far more than the 476 a block can hold
    CHECK(flattenUf2Payload(blk).empty());
}

TEST_CASE("a version is rendered as exactly three digits, and never truncated") {
    CHECK(formatOgAppVersion(1)    == "001");
    CHECK(formatOgAppVersion(12)   == "012");
    CHECK(formatOgAppVersion(123)  == "123");
    CHECK(formatOgAppVersion(0)    == "000");
    // Larger than the contract describes: printed in full rather than cut down,
    // because a four-digit version is one this app has not seen before.
    CHECK(formatOgAppVersion(1234) == "1234");
}

TEST_CASE("ogAppInfoFor reports the absence of a CPU's record") {
    std::vector<OgAppInfo> one(1);
    one[0].cpu = TargetCpu::Main;
    one[0].name = "solo";
    CHECK(ogAppInfoFor(one, TargetCpu::Main).has_value());
    CHECK_FALSE(ogAppInfoFor(one, TargetCpu::Display).has_value());
}
