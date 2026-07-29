#include "ui/fwApp.h"

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
