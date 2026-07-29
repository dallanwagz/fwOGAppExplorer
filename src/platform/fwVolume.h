#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fwog {

/// Every mounted RP2040 bootrom volume, identified by its `RPI-RP2` label.
///
/// Returning more than one is meaningful and must never be collapsed to the
/// first: two mounted volumes cannot be told apart, and the caller refuses on
/// exactly that basis.
std::vector<std::string> findRpiRp2Volumes();

/// Copy `src` into `volume`, then confirm the result.
std::expected<void, std::string> copyToVolume(const std::filesystem::path& src,
                                              const std::string& volume);

namespace detail {

/// Decode `/proc/mounts`' octal escaping of space, tab, newline and
/// backslash (`\040`, `\011`, `\012`, `\134`). Pure, so it is testable on
/// Windows even though its only caller -- the Linux branch of
/// findRpiRp2Volumes -- is not. A malformed or truncated escape (too few
/// digits, or a non-octal digit) is left untouched rather than guessed at.
std::string unescapeMount(std::string_view s);

} // namespace detail

} // namespace fwog
