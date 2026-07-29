#include <doctest/doctest.h>
#include "catalog/fwCatalogStub.h"
#include "catalog/fwCatalogJson.h"
#include "flash/fwFlashPlan.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace fwog;

namespace {

std::vector<OgAppInfo> mainRecord(const std::string& name, const std::string& description,
                                  uint32_t version = 1)
{
    OgAppInfo r;
    r.cpu = TargetCpu::Main;
    r.name = name;
    r.description = description;
    r.version = version;
    return { r };
}

/// A unique scratch directory per test, removed on destruction.
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* tag)
        : path(std::filesystem::temp_directory_path() / ("fwog_stub_test_" + std::string(tag)))
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

std::string read(const std::filesystem::path& p)
{
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("a stub seeds what the image declares and leaves the rest blank") {
    const auto json = blankCatalogEntryJson("wilidoro_main.uf2",
                                            mainRecord("wilidoro", "Pomodoro timer"));
    // Seeded from the image's own record: an image that already says what it is
    // should not make the user retype it.
    CHECK(json.find("\"name\": \"wilidoro\"") != std::string::npos);
    CHECK(json.find("\"description\": \"Pomodoro timer\"") != std::string::npos);
    CHECK(json.find("\"version\": \"001\"") != std::string::npos);
    // The point of the stub: the fields the image cannot know, ready to edit.
    CHECK(json.find("\"author\": \"\"") != std::string::npos);
    CHECK(json.find("\"category\": \"\"") != std::string::npos);
    // Relative, so the folder stays portable -- and under the key the catalog
    // parser actually reads.
    CHECK(json.find("\"path\": \"wilidoro_main.uf2\"") != std::string::npos);
    CHECK(json.find("\"flashScheme\": \"OgApp\"") != std::string::npos);
}

TEST_CASE("a stub for an image with no record is still complete") {
    const auto json = blankCatalogEntryJson("mystery_main.uf2", {});
    CHECK(json.find("\"slug\": \"mystery_main\"") != std::string::npos);
    CHECK(json.find("\"name\": \"\"") != std::string::npos);
    CHECK(json.find("\"path\": \"mystery_main.uf2\"") != std::string::npos);
}

TEST_CASE("the stub's slug matches the Unlisted entry's, so it replaces rather than duplicates") {
    // unlistedEntryFor() slugs a loose file by its stem; matching that is what
    // makes the described entry take the Unlisted one's place in the list.
    CHECK(blankCatalogEntryJson("ogvegas_main.uf2", {}).find("\"slug\": \"ogvegas_main\"")
          != std::string::npos);
}

TEST_CASE("writing a stub creates catalog.json and the app can read it back") {
    TempDir dir("create");
    const auto written = addBlankCatalogEntry(dir.path, "wilidoro_main.uf2",
                                              mainRecord("wilidoro", "Pomodoro timer"));
    REQUIRE(written.has_value());
    CHECK(written->filename() == "catalog.json");

    // The round trip that matters: what was written parses as a catalog.
    const auto parsed = parseCatalogJson(read(*written), CatalogSource::Local);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 1);
    CHECK((*parsed)[0].slug == "wilidoro_main");
    CHECK((*parsed)[0].name == "wilidoro");

    // THE assertion this file was missing, and the reason a stub that listed
    // fine could not be flashed: the asset has to come back with an actual
    // image reference on it. Checking slug and name proved the entry existed,
    // not that it pointed at anything.
    REQUIRE((*parsed)[0].uf2.size() == 1);
    CHECK((*parsed)[0].uf2[0].ref.localPath == "wilidoro_main.uf2");
    CHECK((*parsed)[0].uf2[0].cpu == TargetCpu::Main);

    // ...and that it produces a real, runnable step rather than an empty plan.
    const auto plan = buildFlashPlan((*parsed)[0]);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].cpu == TargetCpu::Main);
    CHECK(plan[0].image.localPath == "wilidoro_main.uf2");
}

TEST_CASE("a second stub is appended, not overwritten") {
    TempDir dir("append");
    REQUIRE(addBlankCatalogEntry(dir.path, "one_main.uf2", {}).has_value());
    REQUIRE(addBlankCatalogEntry(dir.path, "two_main.uf2", {}).has_value());

    const auto parsed = parseCatalogJson(read(dir.path / "catalog.json"), CatalogSource::Local);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 2);
    CHECK((*parsed)[0].slug == "one_main");
    CHECK((*parsed)[1].slug == "two_main");
}

TEST_CASE("a file that already has an entry is refused, not duplicated") {
    // Two entries for one file makes it ambiguous which one wins.
    TempDir dir("dupe");
    REQUIRE(addBlankCatalogEntry(dir.path, "one_main.uf2", {}).has_value());
    const auto again = addBlankCatalogEntry(dir.path, "one_main.uf2", {});
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().find("already has a catalog entry") != std::string::npos);
}

TEST_CASE("an unparseable catalog.json is left alone rather than replaced") {
    // A user's hand-written catalog is not this function's to rewrite. The
    // original bytes must still be on disk afterwards.
    TempDir dir("broken");
    const auto path = dir.path / "catalog.json";
    { std::ofstream out(path); out << "{ this is not json"; }

    const auto result = addBlankCatalogEntry(dir.path, "one_main.uf2", {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("not valid JSON") != std::string::npos);
    CHECK(read(path) == "{ this is not json");
}

TEST_CASE("a catalog.json with no apps array is left alone") {
    TempDir dir("noapps");
    const auto path = dir.path / "catalog.json";
    { std::ofstream out(path); out << R"({"something": 1})"; }

    const auto result = addBlankCatalogEntry(dir.path, "one_main.uf2", {});
    REQUIRE_FALSE(result.has_value());
    CHECK(read(path) == R"({"something": 1})");
}

TEST_CASE("an existing hand-written entry survives the append") {
    // The whole file is round-tripped as a DOCUMENT, so fields this app does
    // not model are not silently dropped.
    TempDir dir("preserve");
    const auto path = dir.path / "catalog.json";
    {
        std::ofstream out(path);
        out << R"({"apps":[{"slug":"mine","name":"Mine","someFutureField":42,
                            "uf2":[{"cpu":"main","file":"mine.uf2"}]}]})";
    }
    REQUIRE(addBlankCatalogEntry(dir.path, "other_main.uf2", {}).has_value());

    const auto text = read(path);
    CHECK(text.find("someFutureField") != std::string::npos);
    CHECK(text.find("\"slug\": \"mine\"") != std::string::npos);
    CHECK(text.find("\"slug\": \"other_main\"") != std::string::npos);
}
