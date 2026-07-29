#pragma once

#include "core/fwTypes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fwog {

/// One `fwog_uf2_info_t` record as carried inside a FreeWili OG image.
///
/// Under the wiliOGbsp contract every OG image carries one of these, and a MAIN
/// UF2 carries TWO: its own (`cpu == Main`) and one describing the display
/// image embedded inside it (`cpu == Display`). That is what lets this app show
/// a user what a single `<name>_main.uf2` will put on BOTH CPUs, from the file
/// alone, with nothing downloaded and no catalog entry required.
struct OgAppInfo {
    TargetCpu   cpu = TargetCpu::Main;
    uint32_t    formatVersion = 0;   ///< record layout version; 1 is all that exists
    uint32_t    version = 0;         ///< app version; rendered zero-padded to 3 digits
    std::string name;
    std::string description;
    /// Git describe of the build, e.g. "6c49d12-dirty". EMPTY on a display
    /// record by design: the bootloader decides whether to transfer the display
    /// image by comparing crc32, so a display image carries no build identity of
    /// its own to disagree with.
    std::string build;
    uint32_t    buildTimestamp = 0;  ///< unix seconds; 0 on a display record
    uint32_t    crc32 = 0;

    bool operator==(const OgAppInfo&) const = default;
};

/// The record's on-image size and the field offsets within it.
///
/// DERIVED FROM REAL IMAGES, not from a published header. The layout below was
/// read out of two independently built apps (ogvegas_main.uf2 and
/// wilidoro_main.uf2), whose records agree field for field, and it is
/// corroborated by the contract's own statements -- two records per main image,
/// one per CPU, and display records carrying no build/build_ts (both hold
/// exactly). It is nonetheless an INFERENCE, so every field is bounds-checked
/// and a record that does not fit is rejected rather than read past.
///
///     +0x00  char     magic[8]        "FWGOINFO"
///     +0x08  uint16   formatVersion   1
///     +0x0a  uint16   cpu             0 = display, 1 = main
///     +0x0c  uint32   version         1 -> "001"
///     +0x10  char     name[32]        NUL-padded
///     +0x30  char     description[128] NUL-padded
///     +0xb0  char     build[32]       NUL-padded; empty on display
///     +0xd0  uint32   buildTimestamp  0 on display
///     +0xd4  uint32   crc32
inline constexpr std::size_t kOgAppInfoSize = 0xd8;
inline constexpr char        kOgAppInfoMagic[8] = { 'F','W','G','O','I','N','F','O' };

/// Every record found in a flattened image, in the order they appear.
///
/// `image` is the CONCATENATED PAYLOAD of a UF2 (what the blocks actually write
/// to flash), not the .uf2 file itself -- a record can straddle a block
/// boundary in the file, and searching the file's raw bytes would both miss
/// those and match on header fields. See flattenUf2Payload().
///
/// Returns empty for an image that carries no records: an ordinary Pico UF2
/// that is not an OG app is not an error, it simply has nothing to say here.
std::vector<OgAppInfo> findOgAppInfo(std::span<const uint8_t> image);

/// Concatenate a UF2's block payloads into the flat image they describe.
///
/// Returns empty when `uf2` is not a whole number of 512-byte blocks or no
/// block carries the UF2 magic. Deliberately does NOT re-validate the image the
/// way parseUf2() does -- this exists to read metadata OUT of a file, including
/// one the flash guard would refuse, and refusing to describe a bad image is
/// how a user ends up with no idea what they are holding.
std::vector<uint8_t> flattenUf2Payload(std::span<const uint8_t> uf2);

/// "001" for 1. The contract renders a version as exactly three digits; a
/// larger number is printed in full rather than truncated.
std::string formatOgAppVersion(uint32_t version);

/// The record for `cpu`, or nullopt when the image carries none for it.
std::optional<OgAppInfo> ogAppInfoFor(std::span<const OgAppInfo> records, TargetCpu cpu);

} // namespace fwog
