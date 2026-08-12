#include "ui/fwWindowIcon.h"

#include "ui/fwIcoImage.h"

#include <miniz.h>
#include <SDL3/SDL.h>

#include <cstddef>
#include <span>
#include <vector>

namespace fwog {

// Defined by the generated fwEmbeddedResource_appIcon.cpp -- see
// tools/embed_resource.py and the CMake rule that runs it. Same arrangement
// the Recovery tab's board diagram uses (fwBoardImage.cpp), and for the same
// reason: this app ships as one executable, and an icon that lived beside it
// as a loose file would be missing from every packaging of it that forgot to
// copy it.
std::span<const unsigned char> appIconDeflated();
std::size_t                    appIconRawSize();

void applyWindowIcon(SDL_Window* window)
{
    if (!window) return;

    const auto        deflated = appIconDeflated();
    const std::size_t rawSize  = appIconRawSize();

    std::vector<unsigned char> ico(rawSize);
    mz_ulong actual = static_cast<mz_ulong>(rawSize);
    if (mz_uncompress(ico.data(), &actual,
                      deflated.data(), static_cast<mz_ulong>(deflated.size())) != MZ_OK) {
        SDL_Log("window icon: inflate failed");
        return;
    }
    if (actual != rawSize) {
        // The generated table and the bytes disagree: a stale build artifact.
        // Say so rather than decode a partial buffer, exactly as
        // fwBoardImage.cpp does for the board diagram.
        SDL_Log("window icon: inflated %lu bytes, expected %zu", (unsigned long)actual, rawSize);
        return;
    }

    const auto decoded = decodeLargestIcoImage(ico);
    if (!decoded) {
        SDL_Log("window icon: %s", decoded.error().c_str());
        return;
    }

    // SDL_CreateSurfaceFrom does not copy the pixels, so `decoded->bgra` has
    // to outlive the surface -- it does: the surface is destroyed at the
    // bottom of this function, well inside decoded's scope. ARGB8888 is the
    // packed-integer name for the B,G,R,A memory order an .ICO's 32-bpp DIB
    // already has on a little-endian host, so no conversion happens here;
    // SDL_SetWindowIcon takes its own converted copy.
    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        decoded->w, decoded->h, SDL_PIXELFORMAT_ARGB8888,
        const_cast<unsigned char*>(decoded->bgra.data()), decoded->w * 4);
    if (!surface) {
        SDL_Log("window icon: SDL_CreateSurfaceFrom failed: %s", SDL_GetError());
        return;
    }

    // A false return is normal and not worth more than a log line: SDL's
    // Wayland backend reports failure when the compositor does not implement
    // xdg_toplevel_icon_v1, which is a property of the user's desktop and not
    // something this app can or should do anything about. See fwWindowIcon.h.
    if (!SDL_SetWindowIcon(window, surface))
        SDL_Log("window icon: SDL_SetWindowIcon failed: %s", SDL_GetError());

    SDL_DestroySurface(surface);
}

} // namespace fwog
