#include "catalog/fwCatalogJson.h"

#include <nlohmann/json.hpp>

#include <optional>

namespace fwog {
namespace {

using json = nlohmann::json;

std::string str(const json& j, const char* key)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

bool boolean(const json& j, const char* key)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) && it->get<bool>();
}

std::optional<FlashScheme> parseScheme(std::string_view s)
{
    if (s == "OgApp")             return FlashScheme::OgApp;
    if (s == "DisplayBootloader") return FlashScheme::DisplayBootloader;
    if (s == "LegacyDirect")      return FlashScheme::LegacyDirect;
    return std::nullopt;
}

std::optional<TargetCpu> parseCpu(std::string_view s)
{
    if (s == "main")    return TargetCpu::Main;
    if (s == "display") return TargetCpu::Display;
    return std::nullopt;   // never guess; "fpga" and typos drop the asset
}

void readAssets(const json& j, CatalogEntry& e)
{
    auto it = j.find("uf2");
    if (it == j.end() || !it->is_array()) return;

    int order = 0;
    for (const auto& a : *it) {
        if (!a.is_object()) continue;
        auto cpu = parseCpu(str(a, "cpu"));
        if (!cpu) continue;

        Uf2Asset asset;
        asset.cpu    = *cpu;
        asset.sha256 = str(a, "sha256");
        // `order` is a post-filter sequence number, NOT the JSON array index:
        // dropped assets (unknown cpu) do not reserve a slot. Safe because
        // buildFlashPlan sorts primarily by CPU rank and uses `order` only as a
        // same-CPU tiebreaker, where compacted and positional numbering compare
        // identically. Do not "fix" this to array position without checking that.
        asset.order  = order++;
        asset.ref.url        = str(a, "url");
        asset.ref.localPath  = str(a, "path");
        asset.ref.embeddedId = str(a, "embeddedId");

        if (auto s = a.find("size"); s != a.end() && s->is_number_unsigned())
            asset.size = s->get<uint64_t>();

        e.uf2.push_back(std::move(asset));
    }
}

} // namespace

std::expected<std::vector<CatalogEntry>, std::string>
parseCatalogJson(std::string_view text, CatalogSource source)
{
    json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) return std::unexpected("the catalog is not valid JSON");

    auto apps = doc.find("apps");
    if (apps == doc.end() || !apps->is_array())
        return std::unexpected("the catalog has no \"apps\" array");

    std::vector<CatalogEntry> out;
    for (const auto& j : *apps) {
        if (!j.is_object()) continue;

        CatalogEntry e;
        e.slug = str(j, "slug");
        if (e.slug.empty()) continue;   // skip, do not fail the whole document

        e.name        = str(j, "name");
        e.tagline     = str(j, "tagline");
        e.description = str(j, "description");
        e.author      = str(j, "author");
        e.github      = str(j, "github");
        e.category    = str(j, "category");
        e.version     = str(j, "version");
        e.updated     = str(j, "updated");
        e.defaultFirmware = boolean(j, "defaultFirmware");
        e.source      = source;

        if (auto t = j.find("tags"); t != j.end() && t->is_array())
            for (const auto& tag : *t)
                if (tag.is_string()) e.tags.push_back(tag.get<std::string>());

        // An absent or unrecognised scheme defaults to OgApp -- the safe one,
        // since it targets only the main CPU -- and is marked inferred so the
        // UI can say so.
        if (auto s = parseScheme(str(j, "flashScheme"))) {
            e.scheme = *s;
            e.schemeInferred = false;
        } else {
            e.scheme = FlashScheme::OgApp;
            e.schemeInferred = true;
        }

        readAssets(j, e);
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace fwog
