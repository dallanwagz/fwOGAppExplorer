#pragma once

#include <expected>
#include <string>

namespace fwog {

/// Whether an HTTPS transport could be obtained.
///
/// Windows: always true, WinHTTP ships with the OS.
/// Linux:   whether libcurl could be dlopen'd. False is not an error -- the
///          remote catalog reports unavailable and everything else works.
/// Web:     always true, emscripten_fetch is part of the runtime.
bool httpAvailable();

/// Blocking HTTPS GET. Call from a worker thread, never from the UI thread.
///
/// TLS validation is left to the platform stack, so the trust store is
/// whatever the OS keeps current. Nothing is bundled and nothing goes stale.
std::expected<std::string, std::string> httpGet(const std::string& url);

} // namespace fwog
