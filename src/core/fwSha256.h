#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace fwog {

/// SHA-256 of `data`, as 64 lowercase hex characters.
std::string sha256Hex(std::span<const uint8_t> data);

} // namespace fwog
