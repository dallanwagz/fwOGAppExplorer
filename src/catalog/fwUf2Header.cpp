#include "catalog/fwUf2Header.h"

namespace fwog {
namespace {

constexpr uint32_t kMagic0        = 0x0A324655;
constexpr uint32_t kMagic1        = 0x9E5D5157;
constexpr uint32_t kMagicEnd      = 0x0AB16F30;
constexpr uint32_t kFamilyRp2040  = 0xE48BFF56;
constexpr uint32_t kFlagFamilyPresent = 0x00002000;
constexpr size_t   kBlockSize     = 512;
constexpr size_t   kMaxPayload    = 476;

uint32_t read32(std::span<const uint8_t> d, size_t off)
{
    return uint32_t(d[off]) | (uint32_t(d[off + 1]) << 8) |
           (uint32_t(d[off + 2]) << 16) | (uint32_t(d[off + 3]) << 24);
}

} // namespace

std::string uf2ErrorMessage(Uf2Error e)
{
    switch (e) {
    case Uf2Error::Empty:              return "the file is empty";
    case Uf2Error::BadSize:            return "the file is not a whole number of 512-byte UF2 blocks; it is probably truncated";
    case Uf2Error::BadMagic:           return "this is not a UF2 file (block magic missing)";
    case Uf2Error::WrongFamily:        return "this UF2 is for a different chip family; the FreeWili OG needs RP2040 images";
    case Uf2Error::BadBlockOrder:      return "the UF2 blocks are not in order; the file is corrupt";
    case Uf2Error::BlockCountMismatch: return "the UF2 block count disagrees with the file size; the file is corrupt or incomplete";
    case Uf2Error::BadPayloadSize:     return "a UF2 block declares more payload than a block can hold";
    }
    return "unknown UF2 error";
}

std::expected<Uf2Info, Uf2Error> parseUf2(std::span<const uint8_t> data)
{
    if (data.empty())                  return std::unexpected(Uf2Error::Empty);
    if (data.size() % kBlockSize != 0) return std::unexpected(Uf2Error::BadSize);

    const uint32_t actualBlocks = uint32_t(data.size() / kBlockSize);

    Uf2Info info;
    info.numBlocks = actualBlocks;

    for (uint32_t i = 0; i < actualBlocks; ++i) {
        const size_t b = size_t(i) * kBlockSize;

        if (read32(data, b + 0) != kMagic0 ||
            read32(data, b + 4) != kMagic1 ||
            read32(data, b + 508) != kMagicEnd) {
            return std::unexpected(Uf2Error::BadMagic);
        }

        const uint32_t flags       = read32(data, b + 8);
        const uint32_t payloadSize = read32(data, b + 16);
        const uint32_t blockNo     = read32(data, b + 20);
        const uint32_t numBlocks   = read32(data, b + 24);

        if (payloadSize > kMaxPayload) return std::unexpected(Uf2Error::BadPayloadSize);
        if (blockNo != i)              return std::unexpected(Uf2Error::BadBlockOrder);
        if (numBlocks != actualBlocks) return std::unexpected(Uf2Error::BlockCountMismatch);

        const bool     blockFamilyPresent = (flags & kFlagFamilyPresent) != 0;
        const uint32_t blockFamilyId      = blockFamilyPresent ? read32(data, b + 28) : 0;

        if (i == 0) {
            info.targetAddr    = read32(data, b + 12);
            info.familyPresent = blockFamilyPresent;
            info.familyId      = blockFamilyId;

            // The check that matters: an RP2350 or ESP32 image must never
            // reach an OG. Images with no family ID at all are allowed --
            // the deprecated originals predate the tag, and refusing them
            // would break LegacyDirect.
            if (info.familyPresent && info.familyId != kFamilyRp2040) {
                return std::unexpected(Uf2Error::WrongFamily);
            }
        } else {
            // Block 0 set the expectation; every later block must match it
            // exactly, in both presence and value. A spliced file -- a
            // legitimate block 0 followed by blocks from a different image --
            // must not slip through just because only the first block was
            // checked.
            if (blockFamilyPresent != info.familyPresent ||
                (info.familyPresent && blockFamilyId != info.familyId)) {
                return std::unexpected(Uf2Error::WrongFamily);
            }
        }

        info.payloadBytes += payloadSize;
    }

    return info;
}

} // namespace fwog
