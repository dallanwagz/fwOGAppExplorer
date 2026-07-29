#include <doctest/doctest.h>
#include "platform/fwPaths.h"

#include <filesystem>

using namespace fwog;

TEST_CASE("exeDir is an existing absolute directory") {
    auto p = exeDir();
    CHECK(p.is_absolute());
    CHECK(std::filesystem::is_directory(p));
}

TEST_CASE("userDataDir is absolute and ends with the app name") {
    auto p = userDataDir();
    CHECK(p.is_absolute());
    CHECK(p.filename() == "fwOGAppExplorer");
}

TEST_CASE("userDataDir is created on demand") {
    CHECK(std::filesystem::is_directory(userDataDir()));
}

TEST_CASE("tempDir is an existing absolute directory") {
    auto p = tempDir();
    CHECK(p.is_absolute());
    CHECK(std::filesystem::is_directory(p));
}

TEST_CASE("catalogDir sits beside the executable") {
    CHECK(catalogDir() == exeDir() / "catalog");
}
