#include "ui/fwBoardImage.h"

#include <miniz.h>
#include <SDL3/SDL.h>

#include <cstddef>
#include <span>
#include <vector>

namespace fwog {

// Defined by the generated fwEmbeddedResource_boardRecovery.cpp -- see
// tools/embed_resource.py and the CMake rule that runs it.
std::span<const unsigned char> boardRecoveryDeflated();
std::size_t                    boardRecoveryRawSize();

BoardImage loadBoardRecoveryImage(SDL_Renderer* renderer)
{
    BoardImage out;
    if (!renderer) return out;

    const auto deflated = boardRecoveryDeflated();
    const std::size_t rawSize = boardRecoveryRawSize();

    std::vector<unsigned char> bmp(rawSize);
    mz_ulong actual = static_cast<mz_ulong>(rawSize);
    // The SAME call shape the embedded firmware uses (fwCatalogEmbedded.cpp):
    // zlib-wrapped, and the decompressed size is known up front from the
    // generated table rather than being discovered.
    if (mz_uncompress(bmp.data(), &actual,
                      deflated.data(), static_cast<mz_ulong>(deflated.size())) != MZ_OK) {
        SDL_Log("board image: inflate failed");
        return out;
    }
    if (actual != rawSize) {
        // A size mismatch means the generated table and the bytes disagree --
        // a stale build artifact. Refuse rather than hand SDL a partial buffer.
        SDL_Log("board image: inflated %lu bytes, expected %zu", (unsigned long)actual, rawSize);
        return out;
    }

    // SDL_IOFromConstMem does not copy, so `bmp` must outlive the load -- it
    // does; SDL_LoadBMP_IO has produced the surface by the time this returns.
    SDL_IOStream* io = SDL_IOFromConstMem(bmp.data(), bmp.size());
    if (!io) {
        SDL_Log("board image: SDL_IOFromConstMem failed: %s", SDL_GetError());
        return out;
    }
    // closeio = true: SDL closes the stream for us on both the success and the
    // failure path, which is the one arrangement that cannot leak it.
    SDL_Surface* surface = SDL_LoadBMP_IO(io, true);
    if (!surface) {
        SDL_Log("board image: SDL_LoadBMP_IO failed: %s", SDL_GetError());
        return out;
    }

    out.texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (out.texture) {
        out.w = surface->w;
        out.h = surface->h;
        // Linear filtering: this is a photograph that is almost always drawn
        // SMALLER than its native 794px to fit the pane, and nearest-neighbour
        // downscaling of a dense PCB turns the silkscreen into aliased noise --
        // on an image whose entire job is to let someone find one pad.
        SDL_SetTextureScaleMode(out.texture, SDL_SCALEMODE_LINEAR);
    } else {
        SDL_Log("board image: SDL_CreateTextureFromSurface failed: %s", SDL_GetError());
    }
    SDL_DestroySurface(surface);
    return out;
}

void destroyBoardImage(BoardImage& image)
{
    if (image.texture) SDL_DestroyTexture(image.texture);
    image = BoardImage{};
}

} // namespace fwog
