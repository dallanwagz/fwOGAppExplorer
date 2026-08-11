#include <doctest/doctest.h>
#include "ui/fwIcoImage.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace fwog;

namespace {

void putU16(std::vector<unsigned char>& b, std::size_t at, std::uint16_t v)
{
    b[at]     = static_cast<unsigned char>(v & 0xff);
    b[at + 1] = static_cast<unsigned char>(v >> 8);
}

void putU32(std::vector<unsigned char>& b, std::size_t at, std::uint32_t v)
{
    b[at]     = static_cast<unsigned char>(v & 0xff);
    b[at + 1] = static_cast<unsigned char>((v >> 8) & 0xff);
    b[at + 2] = static_cast<unsigned char>((v >> 16) & 0xff);
    b[at + 3] = static_cast<unsigned char>((v >> 24) & 0xff);
}

/// A minimal but structurally real .ICO holding one uncompressed 32-bpp image
/// of `size`x`size`, every pixel a distinct value derived from its coordinates
/// so the bottom-up-to-top-down flip can be checked rather than assumed.
///
/// `doubleHeight` writes biHeight = 2*size, which is what real icon writers
/// emit (colour image plus a 1-bpp AND mask stacked); passing false exercises
/// the maskless variant the decoder also accepts.
std::vector<unsigned char> makeIco(int size, bool doubleHeight = true)
{
    const std::size_t pixelBytes = std::size_t(size) * size * 4;
    const std::size_t maskBytes  = doubleHeight ? (std::size_t(size) * size) / 8 : 0;
    const std::size_t imageAt    = 6 + 16;
    const std::size_t payload    = 40 + pixelBytes + maskBytes;

    std::vector<unsigned char> b(imageAt + payload, 0);
    putU16(b, 0, 0);            // reserved
    putU16(b, 2, 1);            // type: icon
    putU16(b, 4, 1);            // one directory entry

    b[6] = static_cast<unsigned char>(size == 256 ? 0 : size);   // width  (0 means 256)
    b[7] = static_cast<unsigned char>(size == 256 ? 0 : size);   // height
    putU16(b, 6 + 6, 32);                                        // bpp
    putU32(b, 6 + 8, static_cast<std::uint32_t>(payload));       // bytes in resource
    putU32(b, 6 + 12, static_cast<std::uint32_t>(imageAt));      // offset

    putU32(b, imageAt + 0, 40);                                          // biSize
    putU32(b, imageAt + 4, static_cast<std::uint32_t>(size));            // biWidth
    putU32(b, imageAt + 8, static_cast<std::uint32_t>(doubleHeight ? size * 2 : size));
    putU16(b, imageAt + 12, 1);                                          // biPlanes
    putU16(b, imageAt + 14, 32);                                         // biBitCount
    putU32(b, imageAt + 16, 0);                                          // biCompression: BI_RGB

    // Bottom-up rows: DIB row 0 is the BOTTOM of the picture.
    for (int row = 0; row < size; ++row) {
        for (int x = 0; x < size; ++x) {
            const std::size_t at = imageAt + 40 + (std::size_t(row) * size + x) * 4;
            b[at + 0] = static_cast<unsigned char>(row);   // B carries the DIB row index
            b[at + 1] = static_cast<unsigned char>(x);     // G carries the column
            b[at + 2] = 0x11;
            b[at + 3] = 0xff;
        }
    }
    return b;
}

} // namespace

TEST_CASE("decodes a 32-bpp icon and flips it top-down") {
    const auto ico = makeIco(8);
    const auto img = decodeLargestIcoImage(ico);
    REQUIRE(img.has_value());
    CHECK(img->w == 8);
    CHECK(img->h == 8);
    REQUIRE(img->bgra.size() == 8u * 8u * 4u);

    // Output row 0 must be the LAST DIB row (7), and output row 7 the first.
    CHECK(img->bgra[0] == 7);                        // B of output (0,0)
    CHECK(img->bgra[(7u * 8u) * 4u] == 0);           // B of output (0,7)
    CHECK(img->bgra[1] == 0);                        // G of output (0,0): column 0
    CHECK(img->bgra[(0u * 8u + 3u) * 4u + 1] == 3);  // G of output (3,0): column 3
    CHECK(img->bgra[3] == 0xff);                     // alpha survives
}

TEST_CASE("a DIB without a stacked AND mask decodes too") {
    const auto ico = makeIco(8, /*doubleHeight=*/false);
    const auto img = decodeLargestIcoImage(ico);
    REQUIRE(img.has_value());
    CHECK(img->w == 8);
    CHECK(img->h == 8);
}

TEST_CASE("a width byte of 0 means 256, not an empty image") {
    const auto ico = makeIco(256);
    const auto img = decodeLargestIcoImage(ico);
    REQUIRE(img.has_value());
    CHECK(img->w == 256);
    CHECK(img->h == 256);
}

TEST_CASE("the LARGEST image wins, whatever order the directory is in") {
    // Two entries: a 16x16 first and a 32x32 second, each with its own payload.
    const auto small = makeIco(16);
    const auto large = makeIco(32);
    const std::size_t smallPayload = small.size() - (6 + 16);
    const std::size_t largePayload = large.size() - (6 + 16);
    const std::size_t dirBytes = 6 + 16 * 2;

    std::vector<unsigned char> b(dirBytes + smallPayload + largePayload, 0);
    putU16(b, 0, 0);
    putU16(b, 2, 1);
    putU16(b, 4, 2);

    b[6] = 16; b[7] = 16;
    putU16(b, 12, 32);
    putU32(b, 14, static_cast<std::uint32_t>(smallPayload));
    putU32(b, 18, static_cast<std::uint32_t>(dirBytes));

    b[22] = 32; b[23] = 32;
    putU16(b, 28, 32);
    putU32(b, 30, static_cast<std::uint32_t>(largePayload));
    putU32(b, 34, static_cast<std::uint32_t>(dirBytes + smallPayload));

    std::copy(small.begin() + 22, small.end(), b.begin() + dirBytes);
    std::copy(large.begin() + 22, large.end(), b.begin() + dirBytes + smallPayload);

    const auto img = decodeLargestIcoImage(b);
    REQUIRE(img.has_value());
    CHECK(img->w == 32);
}

TEST_CASE("a PNG-compressed entry is skipped rather than misread") {
    auto ico = makeIco(8);
    // Overwrite the payload's first bytes with a PNG signature, leaving the
    // directory entry claiming a perfectly ordinary 8x8 32-bpp image. Without
    // the signature check the BITMAPINFOHEADER validation would reject it
    // anyway, so the value here is the ERROR TEXT: it must say what is
    // actually wrong.
    const unsigned char png[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    for (int i = 0; i < 8; ++i) ico[6 + 16 + i] = png[i];

    const auto img = decodeLargestIcoImage(ico);
    REQUIRE_FALSE(img.has_value());
    CHECK(img.error().find("PNG") != std::string::npos);
}

TEST_CASE("malformed containers are refused, not read out of bounds") {
    SUBCASE("empty") {
        CHECK_FALSE(decodeLargestIcoImage({}).has_value());
    }
    SUBCASE("bad signature") {
        auto ico = makeIco(8);
        ico[2] = 2;   // type 2 is a CURSOR; its directory entries mean something else
        CHECK_FALSE(decodeLargestIcoImage(ico).has_value());
    }
    SUBCASE("no images") {
        auto ico = makeIco(8);
        putU16(ico, 4, 0);
        CHECK_FALSE(decodeLargestIcoImage(ico).has_value());
    }
    SUBCASE("directory claims more entries than the file holds") {
        auto ico = makeIco(8);
        putU16(ico, 4, 500);
        const auto img = decodeLargestIcoImage(ico);
        REQUIRE_FALSE(img.has_value());
        CHECK(img.error().find("truncated") != std::string::npos);
    }
    SUBCASE("payload runs past the end of the buffer") {
        auto ico = makeIco(8);
        putU32(ico, 14, 0xffffff00u);   // bytesInRes far beyond the file
        CHECK_FALSE(decodeLargestIcoImage(ico).has_value());
    }
    SUBCASE("payload offset runs past the end of the buffer") {
        auto ico = makeIco(8);
        putU32(ico, 18, 0xfffffff0u);   // imageOffset beyond the file; must not wrap
        CHECK_FALSE(decodeLargestIcoImage(ico).has_value());
    }
    SUBCASE("a DIB header this parser does not know is refused") {
        auto ico = makeIco(8);
        putU32(ico, 6 + 16, 124);   // BITMAPV5HEADER: pixels would be at a different offset
        CHECK_FALSE(decodeLargestIcoImage(ico).has_value());
    }
    SUBCASE("a compressed DIB is refused") {
        auto ico = makeIco(8);
        putU32(ico, 6 + 16 + 16, 3);   // BI_BITFIELDS
        CHECK_FALSE(decodeLargestIcoImage(ico).has_value());
    }
    SUBCASE("directory and DIB disagreeing on the size is refused") {
        auto ico = makeIco(8);
        putU32(ico, 6 + 16 + 4, 9);   // biWidth 9 against a directory that says 8
        CHECK_FALSE(decodeLargestIcoImage(ico).has_value());
    }
    SUBCASE("a payload too short for its own pixels is refused") {
        auto ico = makeIco(8);
        putU32(ico, 14, 40 + 8 * 8 * 4 - 1);   // one byte short of the colour plane
        CHECK_FALSE(decodeLargestIcoImage(ico).has_value());
    }
}

// A directory entry whose length is inside the buffer but shorter than the 40
// bytes of a BITMAPINFOHEADER.
//
// READ THIS BEFORE DELETING THE GUARD IT IS ABOUT. Mutation testing found that
// removing `if (len < kBitmapInfoHeaderSize) continue;` from
// decodeLargestIcoImage() leaves this entire suite green, including this test.
// That is not a gap someone should close by writing a cleverer test, because it
// cannot be closed from here, and the reason is worth stating exactly:
//
//   * Removing the guard does NOT change the RESULT. The later
//     `needed > len` check -- 40 + w*h*4 against the payload length -- rejects
//     any short entry independently, so the function still returns an error.
//   * What removing it DOES change is that biSize, biBitCount and
//     biCompression are read BEFORE that later check runs. With a 16-byte
//     payload at offset 22 in a 38-byte buffer, biCompression at off+16 lands
//     on bytes 38..41 -- past the end. Measured: the mutant, built with
//     -fsanitize=address,undefined against an exact-sized allocation, aborts
//     with libstdc++'s span precondition failing (`span::operator[]`,
//     `Assertion '__idx < size()' failed`); the shipped code returns its error
//     normally. A release build has that assertion compiled out and simply
//     reads whatever is adjacent, reaching the same answer by luck.
//
// So the difference is undefined behaviour, not observable behaviour, and no
// assertion in a plain build can see it. I tried to force it to be observable
// by passing a span shorter than its backing buffer, with a valid header
// planted just past the span's end so a length-ignoring decoder would succeed
// where a correct one fails. It does not work: `needed > len` rejects the entry
// before the planted bytes can affect the outcome.
//
// What this test therefore does and does not do: it pins the BEHAVIOUR (a
// too-short entry is rejected, and rejected for that reason rather than by
// accident of some other property of the file). It does NOT protect the guard.
// The guard is protected by this comment, and by the fact that the overread is
// caught the moment anything builds this code with libstdc++ assertions or a
// sanitizer enabled. This project has no such preset today; adding one would
// turn a whole class of comment-protected invariants into test-protected ones,
// and is worth more than any single test written here.
TEST_CASE("an entry too short to hold a BITMAPINFOHEADER is rejected") {
    constexpr std::size_t kShort  = 16;      // < 40, entirely inside the buffer
    const std::size_t     imageAt = 6 + 16;

    std::vector<unsigned char> b(imageAt + kShort, 0);
    putU16(b, 0, 0);
    putU16(b, 2, 1);
    putU16(b, 4, 1);
    b[6] = 1;
    b[7] = 1;
    putU16(b, 6 + 6, 32);                                    // 32-bpp: passes the bpp check
    putU32(b, 6 + 8, static_cast<std::uint32_t>(kShort));    // the short length under test
    putU32(b, 6 + 12, static_cast<std::uint32_t>(imageAt));

    const auto out = decodeLargestIcoImage(b);
    REQUIRE_FALSE(out.has_value());
    // Specifically "nothing usable", not "only PNGs" -- the entry was reached
    // and rejected, rather than skipped earlier as a compressed image.
    CHECK(out.error().find("no uncompressed 32-bpp image") != std::string::npos);

    // Attribution: the same construction at full length IS accepted, so the
    // rejection above is the short payload and not some other defect in this
    // hand-built file.
    const auto good = decodeLargestIcoImage(makeIco(128));
    REQUIRE(good.has_value());
}
