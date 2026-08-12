#include "ui/fwIcoImage.h"

#include <cstdint>
#include <cstring>

namespace fwog {
namespace {

// The .ICO container. Six-byte header, then `count` 16-byte directory entries,
// then the image payloads at the byte offsets those entries name. Payload
// offsets are absolute from the start of the file and are NOT required to be
// in directory order or contiguous, which is why each one is bounds-checked on
// its own rather than by walking forward.
constexpr std::size_t kHeaderSize = 6;
constexpr std::size_t kDirEntrySize = 16;
// BITMAPINFOHEADER. The only DIB header this reads: a .ICO may in principle
// carry the larger V4/V5 headers, and one that did would be rejected below by
// the biSize check rather than silently misread with the pixel data at the
// wrong offset.
constexpr std::uint32_t kBitmapInfoHeaderSize = 40;

std::uint16_t readU16(std::span<const unsigned char> b, std::size_t at)
{
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

std::uint32_t readU32(std::span<const unsigned char> b, std::size_t at)
{
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

bool looksLikePng(std::span<const unsigned char> b, std::size_t at, std::size_t len)
{
    static const unsigned char kPngSignature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    return len >= sizeof(kPngSignature) &&
           std::memcmp(b.data() + at, kPngSignature, sizeof(kPngSignature)) == 0;
}

} // namespace

std::expected<IcoImage, std::string> decodeLargestIcoImage(std::span<const unsigned char> ico)
{
    if (ico.size() < kHeaderSize)
        return std::unexpected("not an .ICO: shorter than its own header");
    // reserved must be 0 and type must be 1 (icon; 2 would be a cursor, whose
    // directory entries reuse these two bytes as a hotspot and so cannot be
    // read as planes/bpp). Checking both is what makes "this is not the file
    // you think it is" a message instead of a nonsensical size.
    if (readU16(ico, 0) != 0 || readU16(ico, 2) != 1)
        return std::unexpected("not an .ICO: bad signature");

    const std::size_t count = readU16(ico, 4);
    if (count == 0)
        return std::unexpected(".ICO contains no images");
    if (ico.size() < kHeaderSize + count * kDirEntrySize)
        return std::unexpected(".ICO directory is truncated");

    std::size_t bestOffset = 0, bestPixels = 0;
    int bestW = 0, bestH = 0;
    bool sawPngOnly = true;

    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t e = kHeaderSize + i * kDirEntrySize;
        // A width or height byte of 0 means 256 -- the field is one byte and
        // 256 does not fit in it. Every .ICO writer uses this encoding, so
        // reading it literally would report a zero-sized image rather than the
        // largest one in the file.
        const int         w   = ico[e + 0] ? int(ico[e + 0]) : 256;
        const int         h   = ico[e + 1] ? int(ico[e + 1]) : 256;
        const std::uint16_t bpp = readU16(ico, e + 6);
        const std::uint32_t len = readU32(ico, e + 8);
        const std::uint32_t off = readU32(ico, e + 12);

        // Bounds first, and as one unsigned addition promoted wide enough that
        // it cannot wrap: a directory entry naming a length that runs past the
        // end of the buffer is exactly the truncated-resource case this guards.
        if (std::uint64_t(off) + len > ico.size()) continue;
        if (looksLikePng(ico, off, len)) continue;
        sawPngOnly = false;

        if (bpp != 32) continue;                      // 24/8/4/1-bpp entries carry a separate AND mask
        if (len < kBitmapInfoHeaderSize) continue;
        if (readU32(ico, off) != kBitmapInfoHeaderSize) continue;   // not a plain BITMAPINFOHEADER
        if (readU16(ico, off + 14) != 32) continue;                 // biBitCount must agree with the directory
        if (readU32(ico, off + 16) != 0) continue;                  // biCompression: BI_RGB only

        // biWidth/biHeight are the authority on the pixel layout; the
        // directory's bytes are a summary. biHeight is conventionally DOUBLE
        // the icon height because the DIB holds the colour image and a 1-bpp
        // AND mask stacked. Accept either, and take the width/height from the
        // DIB rather than from the directory so the copy below cannot be
        // driven by a directory that disagrees with the pixels.
        const std::int32_t dibW = static_cast<std::int32_t>(readU32(ico, off + 4));
        const std::int32_t dibH = static_cast<std::int32_t>(readU32(ico, off + 8));
        if (dibW <= 0 || dibH <= 0) continue;         // top-down (negative height) DIBs are not produced for icons
        const std::int32_t imageH = (dibH == 2 * h) ? h : dibH;
        if (dibW != w || imageH != h) continue;       // directory and DIB disagree: skip rather than guess

        const std::uint64_t needed = std::uint64_t(kBitmapInfoHeaderSize) + std::uint64_t(w) * h * 4u;
        if (needed > len) continue;                   // colour plane does not fit in the payload

        const std::size_t pixels = std::size_t(w) * std::size_t(h);
        if (pixels > bestPixels) {
            bestPixels = pixels;
            bestOffset = off + kBitmapInfoHeaderSize;
            bestW = w;
            bestH = h;
        }
    }

    if (bestPixels == 0) {
        return std::unexpected(sawPngOnly
            ? ".ICO holds only PNG-compressed images, which this build cannot decode"
            : ".ICO holds no uncompressed 32-bpp image");
    }

    IcoImage out;
    out.w = bestW;
    out.h = bestH;
    out.bgra.resize(bestPixels * 4);

    // Bottom-up to top-down. A DIB stores its last row first; every consumer
    // here wants the first row first, and flipping while copying is cheaper
    // and less error-prone than copying and then flipping in place.
    const std::size_t stride = std::size_t(bestW) * 4;
    for (int y = 0; y < bestH; ++y) {
        const std::size_t src = bestOffset + std::size_t(bestH - 1 - y) * stride;
        std::memcpy(out.bgra.data() + std::size_t(y) * stride, ico.data() + src, stride);
    }
    return out;
}

} // namespace fwog
