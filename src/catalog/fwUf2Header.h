#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace fwog {

enum class Uf2Error {
    Empty,
    BadSize,             ///< not a whole number of 512-byte blocks
    BadMagic,
    WrongFamily,         ///< a family ID is present and it is not RP2040
    BadBlockOrder,       ///< blockNo not sequential from 0
    BlockCountMismatch,  ///< numBlocks disagrees with the file size
    BadPayloadSize,
};

std::string uf2ErrorMessage(Uf2Error e);

struct Uf2Info {
    uint32_t familyId      = 0;
    bool     familyPresent = false;
    uint32_t numBlocks     = 0;
    uint32_t targetAddr    = 0;   ///< of the first block
    uint64_t payloadBytes  = 0;   ///< sum of every block's payloadSize
};

/// Parse and validate a UF2 image. Rejects anything that must not reach a
/// board; see Uf2Error for the reasons.
std::expected<Uf2Info, Uf2Error> parseUf2(std::span<const uint8_t> data);

} // namespace fwog
