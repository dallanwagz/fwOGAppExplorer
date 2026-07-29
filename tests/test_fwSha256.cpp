#include <doctest/doctest.h>
#include "core/fwSha256.h"

#include <string>
#include <vector>

using namespace fwog;

static std::vector<uint8_t> bytes(std::string_view s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

TEST_CASE("sha256 of the empty input") {
    // NIST vector.
    CHECK(sha256Hex({}) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("sha256 of \"abc\"") {
    // NIST vector.
    auto in = bytes("abc");
    CHECK(sha256Hex(in) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256 spanning multiple blocks") {
    // 1,000,000 'a' -- NIST vector. Exercises the buffering path, which is
    // where a hand-rolled SHA-256 most often goes wrong.
    std::vector<uint8_t> in(1000000, 'a');
    CHECK(sha256Hex(in) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256 is lowercase hex and always 64 chars") {
    auto in = bytes("FreeWili");
    auto hex = sha256Hex(in);
    CHECK(hex.size() == 64);
    CHECK(hex.find_first_not_of("0123456789abcdef") == std::string::npos);
}
