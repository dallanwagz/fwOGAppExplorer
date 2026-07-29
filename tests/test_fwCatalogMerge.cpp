#include <doctest/doctest.h>
#include "catalog/fwCatalogMerge.h"

using namespace fwog;

namespace {
CatalogEntry make(std::string slug, CatalogSource src, std::string name = "") {
    CatalogEntry e;
    e.slug = std::move(slug);
    e.source = src;
    e.name = name.empty() ? e.slug : std::move(name);
    return e;
}
} // namespace

TEST_CASE("disjoint sources all appear") {
    auto out = mergeCatalogs({ make("bl", CatalogSource::Embedded) },
                             { make("local-app", CatalogSource::Local) },
                             { make("remote-app", CatalogSource::Remote) });
    CHECK(out.size() == 3);
}

TEST_CASE("local shadows remote by slug") {
    auto out = mergeCatalogs({},
                             { make("dup", CatalogSource::Local,  "local version") },
                             { make("dup", CatalogSource::Remote, "remote version") });
    REQUIRE(out.size() == 1);
    CHECK(out[0].name == "local version");
    CHECK(out[0].source == CatalogSource::Local);
}

TEST_CASE("embedded shadows both") {
    // The embedded images are the recovery path. Nothing downloaded may
    // silently replace them.
    auto out = mergeCatalogs({ make("dup", CatalogSource::Embedded, "embedded") },
                             { make("dup", CatalogSource::Local,    "local") },
                             { make("dup", CatalogSource::Remote,   "remote") });
    REQUIRE(out.size() == 1);
    CHECK(out[0].source == CatalogSource::Embedded);
}

TEST_CASE("embedded shadows local") {
    auto out = mergeCatalogs({ make("dup", CatalogSource::Embedded, "embedded") },
                             { make("dup", CatalogSource::Local,    "local") },
                             {});
    REQUIRE(out.size() == 1);
    CHECK(out[0].source == CatalogSource::Embedded);
}

TEST_CASE("embedded shadows remote") {
    auto out = mergeCatalogs({ make("dup", CatalogSource::Embedded, "embedded") },
                             {},
                             { make("dup", CatalogSource::Remote,   "remote") });
    REQUIRE(out.size() == 1);
    CHECK(out[0].source == CatalogSource::Embedded);
}

TEST_CASE("duplicate slugs within one source keep the first") {
    auto out = mergeCatalogs({}, {},
                             { make("dup", CatalogSource::Remote, "first"),
                               make("dup", CatalogSource::Remote, "second") });
    REQUIRE(out.size() == 1);
    CHECK(out[0].name == "first");
}

TEST_CASE("duplicate slugs within embedded keep the first") {
    auto out = mergeCatalogs({ make("dup", CatalogSource::Embedded, "first"),
                               make("dup", CatalogSource::Embedded, "second") },
                             {},
                             {});
    REQUIRE(out.size() == 1);
    CHECK(out[0].name == "first");
}

TEST_CASE("duplicate slugs within local keep the first") {
    auto out = mergeCatalogs({},
                             { make("dup", CatalogSource::Local, "first"),
                               make("dup", CatalogSource::Local, "second") },
                             {});
    REQUIRE(out.size() == 1);
    CHECK(out[0].name == "first");
}

TEST_CASE("result is sorted by name, case-insensitively") {
    auto out = mergeCatalogs({}, {},
                             { make("c", CatalogSource::Remote, "zebra"),
                               make("a", CatalogSource::Remote, "Apple"),
                               make("b", CatalogSource::Remote, "banana") });
    REQUIRE(out.size() == 3);
    CHECK(out[0].name == "Apple");
    CHECK(out[1].name == "banana");
    CHECK(out[2].name == "zebra");
}

TEST_CASE("merging nothing yields nothing") {
    CHECK(mergeCatalogs({}, {}, {}).empty());
}

TEST_CASE("an offline remote leaves embedded and local intact") {
    auto out = mergeCatalogs({ make("bl", CatalogSource::Embedded) },
                             { make("local-app", CatalogSource::Local) },
                             {});
    CHECK(out.size() == 2);
}

TEST_CASE("entries sharing a name are ordered by source precedence") {
    // Dedup is by slug, so these both survive. Without a stable sort their
    // relative order would be unspecified.
    auto out = mergeCatalogs({ make("bl-embedded", CatalogSource::Embedded, "Display Bootloader") },
                             {},
                             { make("bl-remote",   CatalogSource::Remote,   "Display Bootloader") });
    REQUIRE(out.size() == 2);
    CHECK(out[0].source == CatalogSource::Embedded);
    CHECK(out[1].source == CatalogSource::Remote);
}

TEST_CASE("entries whose names differ only by case are ordered by source precedence") {
    auto out = mergeCatalogs({ make("a-embedded", CatalogSource::Embedded, "blinky") },
                             {},
                             { make("a-remote",   CatalogSource::Remote,   "BLINKY") });
    REQUIRE(out.size() == 2);
    CHECK(out[0].source == CatalogSource::Embedded);
}
