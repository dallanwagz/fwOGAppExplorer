#include "catalog/fwOgAppInfo.h"

#include <algorithm>
#include <cstring>

namespace fwog {
namespace {

constexpr std::size_t kUf2BlockSize   = 512;
constexpr std::size_t kUf2HeaderWords = 8;      // magicStart0..familyId
constexpr uint32_t    kUf2Magic0      = 0x0A324655;
constexpr uint32_t    kUf2Magic1      = 0x9E5D5157;
constexpr std::size_t kUf2PayloadMax  = 476;

// Field offsets -- see kOgAppInfoSize's comment in the header for the whole
// layout and for how it was established.
constexpr std::size_t kOffFormat      = 0x08;
constexpr std::size_t kOffCpu         = 0x0a;
constexpr std::size_t kOffVersion     = 0x0c;
constexpr std::size_t kOffName        = 0x10;
constexpr std::size_t kLenName        = 32;
constexpr std::size_t kOffDescription = 0x30;
constexpr std::size_t kLenDescription = 128;
constexpr std::size_t kOffBuild       = 0xb0;
constexpr std::size_t kLenBuild       = 32;
constexpr std::size_t kOffBuildTs     = 0xd0;
constexpr std::size_t kOffCrc         = 0xd4;

uint16_t readU16(std::span<const uint8_t> s, std::size_t off)
{
    return uint16_t(uint16_t(s[off]) | uint16_t(s[off + 1]) << 8);
}

uint32_t readU32(std::span<const uint8_t> s, std::size_t off)
{
    return uint32_t(s[off]) | uint32_t(s[off + 1]) << 8
         | uint32_t(s[off + 2]) << 16 | uint32_t(s[off + 3]) << 24;
}

/// A NUL-padded fixed field as a string. Stops at the first NUL, and at the
/// field's end if there is none -- an unterminated field is truncated, never
/// read past.
std::string fixedString(std::span<const uint8_t> s, std::size_t off, std::size_t len)
{
    const auto begin = s.begin() + std::ptrdiff_t(off);
    const auto end   = begin + std::ptrdiff_t(len);
    const auto nul   = std::find(begin, end, uint8_t{0});
    return std::string(begin, nul);
}

} // namespace

std::vector<uint8_t> flattenUf2Payload(std::span<const uint8_t> uf2)
{
    if (uf2.empty() || uf2.size() % kUf2BlockSize != 0) return {};

    std::vector<uint8_t> flat;
    flat.reserve(uf2.size() / kUf2BlockSize * kUf2PayloadMax);

    bool sawAnyBlock = false;
    for (std::size_t off = 0; off + kUf2BlockSize <= uf2.size(); off += kUf2BlockSize) {
        const auto block = uf2.subspan(off, kUf2BlockSize);
        if (readU32(block, 0) != kUf2Magic0 || readU32(block, 4) != kUf2Magic1) continue;

        const uint32_t payload = readU32(block, 16);
        if (payload > kUf2PayloadMax) continue;   // malformed block: skip, do not read past it

        sawAnyBlock = true;
        const auto data = block.subspan(kUf2HeaderWords * 4, payload);
        flat.insert(flat.end(), data.begin(), data.end());
    }
    if (!sawAnyBlock) return {};
    return flat;
}

std::vector<OgAppInfo> findOgAppInfo(std::span<const uint8_t> image)
{
    std::vector<OgAppInfo> out;
    if (image.size() < kOgAppInfoSize) return out;

    // Scanned rather than looked up at a fixed address: the record is placed by
    // the linker wherever the app's data lands, and the two records in a main
    // image sit far apart (one inside the embedded display blob). The last
    // start position worth testing is the one where a WHOLE record still fits,
    // which is what keeps every field read below in bounds.
    const std::size_t last = image.size() - kOgAppInfoSize;
    for (std::size_t i = 0; i <= last; ++i) {
        if (std::memcmp(image.data() + i, kOgAppInfoMagic, sizeof(kOgAppInfoMagic)) != 0)
            continue;

        const auto rec = image.subspan(i, kOgAppInfoSize);

        OgAppInfo info;
        info.formatVersion = readU16(rec, kOffFormat);
        // Anything other than the one layout this app knows how to read is
        // skipped, not guessed at: a future record could move every field, and
        // rendering it with these offsets would put confident nonsense on screen
        // beside a Flash button.
        if (info.formatVersion != 1) continue;

        const uint16_t cpu = readU16(rec, kOffCpu);
        if (cpu != 0 && cpu != 1) continue;   // not a CPU this contract defines
        info.cpu = (cpu == 1) ? TargetCpu::Main : TargetCpu::Display;

        info.version        = readU32(rec, kOffVersion);
        info.name           = fixedString(rec, kOffName, kLenName);
        info.description    = fixedString(rec, kOffDescription, kLenDescription);
        info.build          = fixedString(rec, kOffBuild, kLenBuild);
        info.buildTimestamp = readU32(rec, kOffBuildTs);
        info.crc32          = readU32(rec, kOffCrc);

        // A record with no name is not usable as an identity, and is far more
        // likely to be the eight bytes "FWGOINFO" appearing inside some other
        // data than an actual record.
        if (info.name.empty()) continue;

        out.push_back(std::move(info));
        // Skip past the record just accepted: its own body cannot contain
        // another record's magic in a way that means anything.
        i += kOgAppInfoSize - 1;
    }
    return out;
}

std::string formatOgAppVersion(uint32_t version)
{
    std::string s = std::to_string(version);
    // Exactly three digits, per the contract, with no truncation of anything
    // larger -- a four-digit version is a version this app has not seen, not a
    // number to quietly cut down to size.
    while (s.size() < 3) s.insert(s.begin(), '0');
    return s;
}

std::optional<OgAppInfo> ogAppInfoFor(std::span<const OgAppInfo> records, TargetCpu cpu)
{
    for (const auto& r : records)
        if (r.cpu == cpu) return r;
    return std::nullopt;
}

} // namespace fwog
