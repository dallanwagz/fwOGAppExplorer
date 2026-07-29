#pragma once

#include "core/fwTypes.h"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace fwog {

/// The firmware compiled into this executable. Always available, network or
/// not -- which is the point, since a board being recovered is often on a bad
/// network.
std::vector<CatalogEntry> embeddedEntries();

/// Inflate an embedded image by id.
std::expected<std::vector<uint8_t>, std::string> loadEmbeddedImage(const std::string& id);

/// The version recorded for an embedded image at build time. Empty when the id
/// is unknown.
std::string embeddedVersion(const std::string& id);

namespace detail {

/// The decompress-and-verify-size step behind loadEmbeddedImage, factored
/// out and exposed so the two failure paths that matter most for this
/// task's safety story -- a corrupted/truncated deflate stream, and a
/// recorded size that does not match what actually came out -- are
/// unit-testable against synthetic input. The compiled-in embedded table is
/// always valid by construction, so those branches are otherwise
/// unreachable in a normal build. Production code calls loadEmbeddedImage(id);
/// this is a testing seam, not a second public entry point.
std::expected<std::vector<uint8_t>, std::string>
inflateAndVerifySize(std::span<const uint8_t> deflated, std::size_t rawSize);

} // namespace detail

} // namespace fwog
