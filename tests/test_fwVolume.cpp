#include <doctest/doctest.h>
#include "platform/fwVolume.h"

#include <filesystem>
#include <system_error>

using namespace fwog;

TEST_CASE("unescapeMount leaves a path with no escapes unchanged") {
    CHECK(detail::unescapeMount("/media/usb/RPI-RP2") == "/media/usb/RPI-RP2");
}

TEST_CASE("unescapeMount decodes an octal space escape") {
    CHECK(detail::unescapeMount("/media/john\\040doe/RPI-RP2") == "/media/john doe/RPI-RP2");
}

TEST_CASE("unescapeMount leaves a trailing backslash with too few digits alone") {
    // Only two digits follow the backslash before the string ends, so there
    // are not enough characters to form a 3-digit octal escape.
    CHECK(detail::unescapeMount("/media/foo\\04") == "/media/foo\\04");
}

TEST_CASE("unescapeMount leaves a non-octal digit after the backslash alone") {
    // '8' and '9' are not valid octal digits, so this is not a real escape
    // and must be passed through verbatim rather than guessed at.
    CHECK(detail::unescapeMount("/media/foo\\089bar") == "/media/foo\\089bar");
    CHECK(detail::unescapeMount("/media/foo\\099bar") == "/media/foo\\099bar");
}

TEST_CASE("std::filesystem::file_size on a missing path maps to std::errc::no_such_file_or_directory") {
    // copyToVolume's "missing destination after copy means success" branch
    // depends on this mapping holding for the standard library actually in
    // use, so it is verified here directly rather than assumed.
    std::error_code ec;
    const auto sz = std::filesystem::file_size(
        "this-path-should-not-exist-9f8a7c2e/also-missing.uf2", ec);
    (void)sz;
    CHECK(ec);
    CHECK(ec == std::errc::no_such_file_or_directory);
}
