#include "catalog/fwCatalogEmbedded.h"

#include "catalog/fwCatalogJson.h"
#include "fwEmbeddedFirmware.h"

#include <miniz.h>

#include <cstring>

namespace fwog {
namespace {

const generated::EmbeddedImage* find(const std::string& id)
{
    for (size_t i = 0; i < generated::kImageCount; ++i)
        if (generated::kImages[i].id && id == generated::kImages[i].id)
            return &generated::kImages[i];
    return nullptr;
}

} // namespace

namespace detail {

std::expected<std::vector<uint8_t>, std::string>
inflateAndVerifySize(std::span<const uint8_t> deflated, std::size_t rawSize)
{
    std::vector<uint8_t> out(rawSize);
    mz_ulong outLen = mz_ulong(out.size());
    const int rc = mz_uncompress(out.data(), &outLen,
                                 deflated.data(), mz_ulong(deflated.size()));
    if (rc != MZ_OK)
        return std::unexpected("could not be decompressed");
    if (outLen != rawSize)
        return std::unexpected("is the wrong size");
    return out;
}

} // namespace detail

std::vector<CatalogEntry> embeddedEntries()
{
    auto parsed = parseCatalogJson(generated::kEntriesJson, CatalogSource::Embedded);
    if (!parsed) return {};

    // The generated JSON gives each asset an "embeddedId"; fill in the size
    // and hash the embedder recorded, so the flash engine verifies embedded
    // images exactly as it verifies downloaded ones.
    for (auto& e : *parsed) {
        for (auto& a : e.uf2) {
            if (a.ref.embeddedId.empty()) continue;
            if (const auto* img = find(a.ref.embeddedId)) {
                a.size   = img->rawSize;
                a.sha256 = img->sha256;
            }
        }
    }
    return *parsed;
}

std::expected<std::vector<uint8_t>, std::string> loadEmbeddedImage(const std::string& id)
{
    const auto* img = find(id);
    if (!img) return std::unexpected("no embedded image named \"" + id + "\"");

    auto inflated = detail::inflateAndVerifySize(
        std::span<const uint8_t>(img->deflated, img->deflatedSize), img->rawSize);
    if (!inflated)
        return std::unexpected("the embedded image \"" + id + "\" " + inflated.error());
    return inflated;
}

std::string embeddedVersion(const std::string& id)
{
    const auto* img = find(id);
    return (img && img->version) ? img->version : std::string{};
}

} // namespace fwog
