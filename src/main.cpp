#include "ui/fwApp.h"

// iOS is the one platform where SDL must own the real entry point: UIKit
// requires UIApplicationMain to run the process, and SDL3's SDL_main.h -- on
// platforms where SDL_MAIN_NEEDED is set, which excludes Windows, Linux and
// macOS -- renames main below to SDL_main and supplies a real main() that
// calls UIApplicationMain and then this function. The Windows comment further
// down still holds: this include is scoped so it can never collide with the
// hand-written WinMain there.
#if defined(__APPLE__)
  #include <TargetConditionals.h>
  #if !TARGET_OS_OSX
    #include <SDL3/SDL_main.h>
  #endif
#endif

int main(int, char**)
{
    fwog::App app;
    return app.run();
}

#if defined(_WIN32)
// The executable links WIN32 (/SUBSYSTEM:WINDOWS in Release, so no console
// window flashes up) which means the CRT startup looks for WinMain, not
// main. Forward to the real entry point above rather than duplicating any
// logic here. We deliberately do NOT include <SDL3/SDL_main.h> anywhere in
// this app -- doing so would make SDL3 supply its own WinMain (via the
// separate SDL3main target, which this project does not link) and collide
// with this one.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    return main(0, nullptr);
}
#endif
