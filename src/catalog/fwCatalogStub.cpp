#include "catalog/fwCatalogStub.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace fwog {
namespace {

/// The slug a stub gets: the file's stem, which is what loadLocalCatalog()
/// already uses to name an Unlisted entry (unlistedEntryFor, fwCatalogLocal.cpp).
/// Keeping them the same means the entry replaces the Unlisted one in place
/// rather than appearing beside it as a duplicate.
std::string slugFor(const std::string& fileName)
{
    const auto dot = fileName.rfind('.');
    return dot == std::string::npos ? fileName : fileName.substr(0, dot);
}

} // namespace

std::string blankCatalogEntryJson(const std::string& fileName,
                                  std::span<const OgAppInfo> records)
{
    const auto main = ogAppInfoFor(records, TargetCpu::Main);

    nlohmann::ordered_json e;
    e["slug"] = slugFor(fileName);
    // Seeded from the image where the image knows, blank where it cannot.
    e["name"]        = main ? main->name : std::string{};
    e["tagline"]     = std::string{};
    e["description"] = main ? main->description : std::string{};
    e["version"]     = main ? formatOgAppVersion(main->version) : std::string{};
    e["author"]      = std::string{};
    e["github"]      = std::string{};
    e["category"]    = std::string{};
    e["tags"]        = nlohmann::ordered_json::array();
    // Always OgApp: everything the App Explorer lists is one image written to
    // MAIN (see onlyOgApps, fwCatalogFilter.h), and a stub that left the scheme
    // open would be inviting the one edit that could point it elsewhere.
    e["flashScheme"] = "OgApp";

    nlohmann::ordered_json asset;
    asset["cpu"] = "main";
    // "path", which is the key parseCatalogJson() reads for a local file
    // (fwCatalogJson.cpp -- the three it knows are "url", "path" and
    // "embeddedId"). This said "file" first, which parsed to an entry with NO
    // image reference at all: it listed, it looked right, and it could not be
    // flashed.
    asset["path"] = fileName;
    e["uf2"] = nlohmann::ordered_json::array({ asset });

    return e.dump(2);
}

std::expected<std::filesystem::path, std::string>
addBlankCatalogEntry(const std::filesystem::path& dir, const std::string& fileName,
                     std::span<const OgAppInfo> records)
{
    const auto path = dir / "catalog.json";

    nlohmann::ordered_json doc;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::ifstream in(path);
        if (!in) return std::unexpected("could not open " + path.string() + " for reading.");
        std::ostringstream ss;
        ss << in.rdbuf();
        // Not parseCatalogJson(): this needs the DOCUMENT back so it can be
        // written out again with one more entry, and that function returns
        // CatalogEntry structs -- round-tripping through those would silently
        // drop every field the app does not model and reformat the user's file.
        doc = nlohmann::ordered_json::parse(ss.str(), nullptr, false);
        if (doc.is_discarded())
            return std::unexpected(path.string() + " is not valid JSON, so it was left alone. "
                                    "Fix or remove it and try again.");
        if (!doc.contains("apps") || !doc["apps"].is_array())
            return std::unexpected(path.string() + " has no \"apps\" array, so it was left alone.");

        // Already described: adding a second entry for the same file would make
        // it ambiguous which one wins.
        for (const auto& app : doc["apps"]) {
            if (!app.contains("uf2") || !app["uf2"].is_array()) continue;
            for (const auto& a : app["uf2"])
                if (a.contains("path") && a["path"].is_string()
                    && a["path"].get<std::string>() == fileName)
                    return std::unexpected(fileName + " already has a catalog entry.");
        }
    } else {
        doc["apps"] = nlohmann::ordered_json::array();
    }

    doc["apps"].push_back(nlohmann::ordered_json::parse(blankCatalogEntryJson(fileName, records)));

    std::ofstream out(path, std::ios::trunc);
    if (!out) return std::unexpected("could not open " + path.string() + " for writing.");
    out << doc.dump(2) << "\n";
    if (!out) return std::unexpected("could not write " + path.string() + ".");
    return path;
}

} // namespace fwog
