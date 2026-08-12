#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace fwog {

/// Whether an HTTPS transport could be obtained.
///
/// Windows: always true, WinHTTP ships with the OS.
/// Linux:   whether libcurl could be dlopen'd. False is not an error -- the
///          remote catalog reports unavailable and everything else works.
/// Web:     always true, emscripten_fetch is part of the runtime.
bool httpAvailable();

/// Blocking HTTPS GET. Call from a worker thread, never from the UI thread.
/// Two of them do: RemoteCatalog::run and FlashController's flash worker, and
/// a catalog refresh can overlap a download, so this must be safe to call from
/// two threads at once.
///
/// Redirects are followed, at most 10 deep, and never off https onto plain
/// http (see mayRedirectToPlainHttp). A non-2xx status is an error, not a
/// body. TLS validation is left to the platform stack, so the trust store is
/// whatever the OS keeps current. Nothing is bundled and nothing goes stale.
std::expected<std::string, std::string> httpGet(const std::string& url);

/// Whether a request for `url` may be redirected onto plain http.
///
/// False when `url` is https: dropping TLS mid-redirect would undo the
/// protection the caller asked for, on a document that decides which CPU gets
/// flashed. True when `url` is already plain http, which has no TLS to lose.
///
/// This is the redirect policy itself, pulled out as a pure function so it can
/// be tested without a network, without libcurl and on any platform. Windows
/// gets the same rule from WinHTTP's own default redirect policy and does not
/// call this; the POSIX build has to ask libcurl for it explicitly.
bool mayRedirectToPlainHttp(std::string_view url);

} // namespace fwog
