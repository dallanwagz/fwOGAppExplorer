#pragma once

// The ONE statement of where freewili-finder exists, shared by every guard
// that used to test __EMSCRIPTEN__ directly. fwfinder is absent on the web
// (no USB tree at all) and on iOS/iPadOS (its Apple backend is IOKit, which
// is not public API there, and iPadOS offers no USB enumeration to replace
// it). Everywhere else the real library is fetched and linked.
#if defined(__APPLE__)
  #include <TargetConditionals.h>
#endif

#if !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && !TARGET_OS_OSX)
  #define FWOG_HAVE_FWFINDER 1
#endif
