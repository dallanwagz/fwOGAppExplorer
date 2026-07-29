#include "platform/fwHttp.h"

#include "core/fwSettingsIo.h"   // hasHttpScheme -- the ONE scheme predicate

#include <cstring>

namespace fwog {
namespace {

/// Returns an error when the URL is unusable, or an empty value when it is
/// fine. Callers write
/// `if (auto bad = checkUrl(url); !bad.has_value()) return std::unexpected(bad.error());`.
///
/// The scheme test is fwog::hasHttpScheme() (fwog_core), not a local one.
/// This file used to carry its own `rfind("https://", 0) == 0`, which is
/// case-SENSITIVE, while the settings layer that validates what the user types
/// compares the scheme case-INsensitively as RFC 3986 requires. The two
/// disagreed: pasting "HTTPS://example.com/apps.json" was accepted by the App
/// Explorer tab's control, written to settings.ini, and then failed every
/// fetch -- this session and every future launch -- with "only http and https
/// URLs are supported", for a URL that plainly is https. Both layers now share
/// one predicate so they cannot drift apart again.
std::expected<std::string, std::string> checkUrl(const std::string& url)
{
    if (url.empty())          return std::unexpected("the URL is empty");
    if (!hasHttpScheme(url))  return std::unexpected("only http and https URLs are supported");
    return std::string{};
}

} // namespace
} // namespace fwog

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace fwog {

bool httpAvailable() { return true; }

std::expected<std::string, std::string> httpGet(const std::string& url)
{
    if (auto bad = checkUrl(url); !bad.has_value()) return std::unexpected(bad.error());

    const std::wstring wurl(url.begin(), url.end());

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = DWORD(std::size(host));
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = DWORD(std::size(path));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
        return std::unexpected("the URL could not be parsed");

    struct Handle {
        HINTERNET h = nullptr;
        ~Handle() { if (h) WinHttpCloseHandle(h); }
        Handle() = default;
        explicit Handle(HINTERNET handle) : h(handle) {}
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };

    Handle session{ WinHttpOpen(L"fwOGAppExplorer/1.0",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!session.h) return std::unexpected("the HTTP session could not be opened");

    Handle conn{ WinHttpConnect(session.h, host, uc.nPort, 0) };
    if (!conn.h) return std::unexpected("cannot reach the host");

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    Handle req{ WinHttpOpenRequest(conn.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, flags) };
    if (!req.h) return std::unexpected("the request could not be created");

    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        return std::unexpected("the request failed; check the network connection");

    DWORD status = 0, statusLen = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                        WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300)
        return std::unexpected("the server returned HTTP " + std::to_string(status));

    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req.h, chunk.data(), avail, &read)) break;
        body.append(chunk, 0, read);
    }
    return body;
}

} // namespace fwog

#elif defined(__EMSCRIPTEN__)
#include <emscripten/fetch.h>

namespace fwog {

bool httpAvailable() { return true; }

std::expected<std::string, std::string> httpGet(const std::string& url)
{
    if (auto bad = checkUrl(url); !bad.has_value()) return std::unexpected(bad.error());

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strcpy(attr.requestMethod, "GET");
    // Synchronous because this already runs on a worker (fwCatalogRemote), and
    // blocking there keeps one code shape across all three platforms.
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;

    emscripten_fetch_t* f = emscripten_fetch(&attr, url.c_str());
    if (!f) return std::unexpected("the request could not be created");

    std::expected<std::string, std::string> result =
        (f->status >= 200 && f->status < 300)
            ? std::expected<std::string, std::string>(
                  std::string(f->data, size_t(f->numBytes)))
            : std::unexpected("the server returned HTTP " + std::to_string(f->status));

    emscripten_fetch_close(f);
    return result;
}

} // namespace fwog

#else   // Linux and other POSIX
#include <dlfcn.h>

namespace fwog {
namespace {

// libcurl is dlopen'd, never linked. Linking would make libcurl.so a hard
// runtime dependency of a binary that is supposed to have none. When it is
// absent the failure is contained: the remote catalog reports unavailable and
// embedded firmware, the local catalog, device detection, flashing and
// Recovery all keep working.
struct Curl {
    void* lib = nullptr;
    void*       (*easy_init)()                  = nullptr;
    int         (*easy_setopt)(void*, int, ...) = nullptr;
    int         (*easy_perform)(void*)          = nullptr;
    int         (*easy_getinfo)(void*, int, ...) = nullptr;
    void        (*easy_cleanup)(void*)          = nullptr;
    const char* (*easy_strerror)(int)           = nullptr;

    bool ok() const {
        return lib && easy_init && easy_setopt && easy_perform && easy_getinfo
               && easy_cleanup && easy_strerror;
    }
};

const Curl& curl()
{
    static const Curl c = [] {
        Curl x;
        for (const char* name : { "libcurl.so.4", "libcurl.so", "libcurl.so.3" }) {
            x.lib = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
            if (x.lib) break;
        }
        if (!x.lib) return x;
        x.easy_init     = (decltype(x.easy_init))     dlsym(x.lib, "curl_easy_init");
        x.easy_setopt   = (decltype(x.easy_setopt))   dlsym(x.lib, "curl_easy_setopt");
        x.easy_perform  = (decltype(x.easy_perform))  dlsym(x.lib, "curl_easy_perform");
        x.easy_getinfo  = (decltype(x.easy_getinfo))  dlsym(x.lib, "curl_easy_getinfo");
        x.easy_cleanup  = (decltype(x.easy_cleanup))  dlsym(x.lib, "curl_easy_cleanup");
        x.easy_strerror = (decltype(x.easy_strerror)) dlsym(x.lib, "curl_easy_strerror");
        return x;
    }();
    return c;
}

// Option values from curl.h, hard-coded because the header is not included.
// These are ABI-stable and have not changed since libcurl 7.x.
constexpr int CURLOPT_URL            = 10002;
constexpr int CURLOPT_WRITEDATA      = 10001;
constexpr int CURLOPT_USERAGENT      = 10018;
constexpr int CURLOPT_WRITEFUNCTION  = 20011;
constexpr int CURLOPT_FOLLOWLOCATION = 52;
constexpr int CURLOPT_TIMEOUT        = 13;
constexpr int CURLINFO_RESPONSE_CODE = 2097154; // CURLINFO_LONG + 2

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

bool httpAvailable() { return curl().ok(); }

std::expected<std::string, std::string> httpGet(const std::string& url)
{
    if (auto bad = checkUrl(url); !bad.has_value()) return std::unexpected(bad.error());

    const Curl& c = curl();
    if (!c.ok())
        return std::unexpected("libcurl is not installed, so the remote catalog is "
                               "unavailable; embedded and local firmware still work");

    void* h = c.easy_init();
    if (!h) return std::unexpected("libcurl could not be initialised");

    std::string body;
    c.easy_setopt(h, CURLOPT_URL, url.c_str());
    c.easy_setopt(h, CURLOPT_WRITEFUNCTION, writeCb);
    c.easy_setopt(h, CURLOPT_WRITEDATA, &body);
    c.easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    c.easy_setopt(h, CURLOPT_TIMEOUT, 30L);
    c.easy_setopt(h, CURLOPT_USERAGENT, "fwOGAppExplorer/1.0");

    const int rc = c.easy_perform(h);
    long status = 0;
    if (rc == 0) c.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    c.easy_cleanup(h);

    if (rc != 0)
        return std::unexpected(std::string("the request failed: ") + c.easy_strerror(rc));
    if (status < 200 || status >= 300)
        return std::unexpected("the server returned HTTP " + std::to_string(status));
    return body;
}

} // namespace fwog

#endif
