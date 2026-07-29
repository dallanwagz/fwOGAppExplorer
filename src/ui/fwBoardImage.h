#pragma once

struct SDL_Renderer;
struct SDL_Texture;

namespace fwog {

/// The FreeWili board photo shown on the Recovery tab, with the MAIN BOOTSEL
/// pad, the DISPLAY BOOTSEL pad and a ground point marked on it.
///
/// `texture` is null when the image could not be produced; `w`/`h` are then 0.
/// That is a degraded panel, never a crash and never a blank the user cannot
/// explain -- the Recovery tab prints the two procedures as text regardless,
/// and the picture is what makes "short the DISPLAY pad to ground" findable
/// rather than what makes it possible.
struct BoardImage {
    SDL_Texture* texture = nullptr;
    int          w = 0;
    int          h = 0;
};

/// Inflate the embedded BMP and upload it as a texture on `renderer`.
///
/// Call ONCE, after the renderer exists and before the first frame that draws
/// it; the caller owns the result for the life of the renderer and must call
/// destroyBoardImage() before tearing the renderer down.
///
/// BMP rather than PNG, deflated rather than raw: SDL3's core has an SDL_LoadBMP
/// and no PNG decoder, and miniz is already vendored here to inflate the
/// embedded firmware. So this needs no new dependency in either direction -- the
/// image goes in as bytes this project already knows how to compress, and comes
/// out through a loader SDL already ships.
BoardImage loadBoardRecoveryImage(SDL_Renderer* renderer);

/// Releases the texture. Safe on a default-constructed/failed BoardImage.
void destroyBoardImage(BoardImage& image);

} // namespace fwog
