#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace fwog {

/// One decoded image out of a Windows .ICO container.
///
/// `bgra` is w*h*4 bytes, top-down, one pixel as B,G,R,A in memory -- which is
/// byte-for-byte what SDL calls SDL_PIXELFORMAT_ARGB8888 on a little-endian
/// host, so a surface can be wrapped around it with no swizzle. Named for the
/// MEMORY order rather than SDL's packed-integer name because this header
/// knows nothing about SDL and must stay that way: it is compiled into
/// fwog_core so the test suite can exercise it without linking SDL or ImGui,
/// the same arrangement fwTabDefaultFirmwareLogic uses.
struct IcoImage {
    int                        w = 0;
    int                        h = 0;
    std::vector<unsigned char> bgra;
};

/// The largest uncompressed 32-bpp image in an .ICO, or a reason it could not
/// be read.
///
/// LARGEST, not first: an .ICO is a container of the same drawing at several
/// sizes, and every consumer this project has downscales -- X11 hands
/// _NET_WM_ICON to a window manager that picks its own size, and Wayland's
/// xdg_toplevel_icon_v1 does the same. Starting from the most detailed image
/// available is the only choice that cannot be improved by picking differently
/// later.
///
/// PNG-compressed entries (the 256x256 one in resources/product.ico is one)
/// are SKIPPED rather than treated as an error: nothing in this app can decode
/// PNG -- SDL3's core ships a BMP loader and no PNG loader -- so an .ICO that
/// holds a PNG alongside ordinary DIBs is a perfectly good input and only the
/// DIBs are candidates. An .ICO that holds NOTHING BUT PNGs is the error case,
/// and it reports that rather than returning a blank image.
///
/// Every offset and length in the file is bounds-checked against the actual
/// buffer before it is used. The input is compiled into the executable today,
/// so nothing hostile can reach it -- the checks are here because a truncated
/// or half-generated resource is a build accident, and the one outcome it must
/// never have is a read past the end of a static array.
///
/// Note what it does NOT promise. A truncated file is not reliably reported AS
/// truncation: an entry whose length runs off the end is skipped like any other
/// unusable entry, so the usual result is either a smaller icon than the file
/// meant to contain or a message about whatever the surviving entries happen to
/// be. Distinguishing "this file is cut short" from "this file has nothing I can
/// use" would mean tracking why each entry was rejected, and the caller already
/// has a better answer: fwWindowIcon.cpp compares the inflated size against the
/// recorded one and refuses before a truncated buffer ever reaches this
/// function. Memory safety is the guarantee here; diagnosis is the caller's.
std::expected<IcoImage, std::string> decodeLargestIcoImage(std::span<const unsigned char> ico);

} // namespace fwog
