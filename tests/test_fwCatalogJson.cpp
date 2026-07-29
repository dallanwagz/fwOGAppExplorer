#include <doctest/doctest.h>
#include "catalog/fwCatalogJson.h"

using namespace fwog;

static constexpr const char* kValid = R"({
  "apps": [
    {
      "slug": "wili-blinky",
      "name": "WiLi Blinky",
      "tagline": "The classic LED blink demo.",
      "description": "A minimal getting-started example.",
      "author": "Intrepid Control Systems",
      "github": "https://github.com/intrepidcs/freewili-blinky",
      "category": "Examples",
      "tags": ["beginner", "led"],
      "version": "1.0.0",
      "updated": "2026-04-12",
      "flashScheme": "OgApp",
      "uf2": [
        { "cpu": "main", "url": "https://example.com/blinky.uf2",
          "sha256": "aa", "size": 262144 }
      ]
    }
  ]
})";

TEST_CASE("a valid catalog parses") {
    auto r = parseCatalogJson(kValid, CatalogSource::Remote);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    const auto& e = (*r)[0];
    CHECK(e.slug == "wili-blinky");
    CHECK(e.name == "WiLi Blinky");
    CHECK(e.category == "Examples");
    CHECK(e.tags.size() == 2);
    CHECK(e.scheme == FlashScheme::OgApp);
    CHECK(e.schemeInferred == false);
    CHECK(e.source == CatalogSource::Remote);
    REQUIRE(e.uf2.size() == 1);
    CHECK(e.uf2[0].cpu == TargetCpu::Main);
    CHECK(e.uf2[0].ref.url == "https://example.com/blinky.uf2");
    CHECK(e.uf2[0].size == 262144);
}

TEST_CASE("an entry with no uf2 block still parses, with no assets") {
    // The website's existing apps.json has no uf2 anywhere. Those entries must
    // still appear in the App Explorer -- only the Flash button is disabled.
    auto r = parseCatalogJson(R"({"apps":[{"slug":"a","name":"A"}]})",
                              CatalogSource::Remote);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].uf2.empty());
    CHECK((*r)[0].schemeInferred == true);
}

TEST_CASE("all three flash schemes are recognised") {
    auto one = [](const char* s) {
        std::string j = R"({"apps":[{"slug":"a","name":"A","flashScheme":")";
        j += s; j += R"("}]})";
        auto r = parseCatalogJson(j, CatalogSource::Remote);
        REQUIRE(r.has_value());
        return (*r)[0].scheme;
    };
    CHECK(one("OgApp")             == FlashScheme::OgApp);
    CHECK(one("DisplayBootloader") == FlashScheme::DisplayBootloader);
    CHECK(one("LegacyDirect")      == FlashScheme::LegacyDirect);
}

TEST_CASE("an unknown flashScheme falls back to OgApp and marks it inferred") {
    // OgApp is the safe default: it targets only the main CPU.
    auto r = parseCatalogJson(
        R"({"apps":[{"slug":"a","name":"A","flashScheme":"Nonsense"}]})",
        CatalogSource::Remote);
    REQUIRE(r.has_value());
    CHECK((*r)[0].scheme == FlashScheme::OgApp);
    CHECK((*r)[0].schemeInferred == true);
}

TEST_CASE("an unknown cpu value drops that asset rather than guessing") {
    auto r = parseCatalogJson(
        R"({"apps":[{"slug":"a","name":"A","uf2":[{"cpu":"fpga","url":"u"}]}]})",
        CatalogSource::Remote);
    REQUIRE(r.has_value());
    CHECK((*r)[0].uf2.empty());
}

TEST_CASE("assets receive ascending order values with no gaps") {
    auto r = parseCatalogJson(R"({"apps":[{"slug":"a","name":"A",
        "flashScheme":"LegacyDirect",
        "uf2":[{"cpu":"main","url":"m"},{"cpu":"display","url":"d"}]}]})",
        CatalogSource::Remote);
    REQUIRE(r.has_value());
    REQUIRE((*r)[0].uf2.size() == 2);
    CHECK((*r)[0].uf2[0].order == 0);
    CHECK((*r)[0].uf2[1].order == 1);
}

TEST_CASE("malformed JSON returns an error rather than throwing") {
    auto r = parseCatalogJson("{ this is not json", CatalogSource::Remote);
    REQUIRE_FALSE(r.has_value());
    CHECK_FALSE(r.error().empty());
}

TEST_CASE("a missing apps array returns an error") {
    auto r = parseCatalogJson(R"({"nope":[]})", CatalogSource::Remote);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("an entry with no slug is skipped, not fatal") {
    // One bad entry must not cost the user the whole catalog.
    auto r = parseCatalogJson(
        R"({"apps":[{"name":"no slug"},{"slug":"ok","name":"OK"}]})",
        CatalogSource::Remote);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].slug == "ok");
}

TEST_CASE("an empty apps array is valid and yields nothing") {
    auto r = parseCatalogJson(R"({"apps":[]})", CatalogSource::Remote);
    REQUIRE(r.has_value());
    CHECK(r->empty());
}

TEST_CASE("defaultFirmware is read when present") {
    auto r = parseCatalogJson(
        R"({"apps":[{"slug":"a","name":"A","defaultFirmware":true}]})",
        CatalogSource::Local);
    REQUIRE(r.has_value());
    CHECK((*r)[0].defaultFirmware == true);
    CHECK((*r)[0].source == CatalogSource::Local);
}

TEST_CASE("asset order compacts when entries are dropped between survivors") {
    // Dropped assets (unknown cpu) do not reserve order slots; surviving
    // assets get consecutive 0, 1, 2... order values, not array positions.
    // This is safe because order is only a same-CPU tiebreaker in buildFlashPlan.
    auto r = parseCatalogJson(R"({"apps":[{"slug":"a","name":"A",
        "uf2":[{"cpu":"main","url":"m1"},{"cpu":"fpga","url":"f"},{"cpu":"main","url":"m2"}]}]})",
        CatalogSource::Remote);
    REQUIRE(r.has_value());
    REQUIRE((*r)[0].uf2.size() == 2);
    CHECK((*r)[0].uf2[0].cpu == TargetCpu::Main);
    CHECK((*r)[0].uf2[0].order == 0);
    CHECK((*r)[0].uf2[1].cpu == TargetCpu::Main);
    CHECK((*r)[0].uf2[1].order == 1);  // NOT 2, despite JSON position
}

TEST_CASE("wrong-typed fields are ignored rather than throwing") {
    // The never-throw guarantee is what keeps a malformed catalog a status
    // line instead of a crash. These are the shapes a hand-edited
    // catalog.json most plausibly produces.
    auto r = parseCatalogJson(R"({"apps":[{
        "slug": "a",
        "name": 123,
        "tags": "not-an-array",
        "uf2": {},
        "defaultFirmware": "yes"
    }]})", CatalogSource::Remote);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].slug == "a");
    CHECK((*r)[0].name.empty());          // wrong type -> treated as absent
    CHECK((*r)[0].tags.empty());
    CHECK((*r)[0].uf2.empty());           // object, not array -> no assets
    CHECK((*r)[0].defaultFirmware == false);
}

TEST_CASE("a wrong-typed asset size is ignored rather than throwing") {
    auto r = parseCatalogJson(R"({"apps":[{"slug":"a","name":"A",
        "uf2":[{"cpu":"main","url":"u","size":"big"}]}]})",
        CatalogSource::Remote);
    REQUIRE(r.has_value());
    REQUIRE((*r)[0].uf2.size() == 1);
    CHECK((*r)[0].uf2[0].size == 0);
}

TEST_CASE("a non-object entry in the apps array is skipped") {
    auto r = parseCatalogJson(R"({"apps":[42,"nope",{"slug":"ok","name":"OK"}]})",
                              CatalogSource::Remote);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].slug == "ok");
}
