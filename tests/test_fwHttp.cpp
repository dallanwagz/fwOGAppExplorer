#include <doctest/doctest.h>
#include "platform/fwHttp.h"

using namespace fwog;

TEST_CASE("httpAvailable can be called safely and does not throw") {
    // Windows: always true, WinHTTP ships with the OS. Linux: depends on
    // whether libcurl could be dlopen'd. Either answer is correct; what
    // matters is that asking is safe and cheap.
    bool ok = httpAvailable();
    CHECK((ok == true || ok == false));
}

TEST_CASE("a non-http scheme is rejected without touching the network") {
    auto r = httpGet("file:///etc/passwd");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("http") != std::string::npos);
}

TEST_CASE("an empty URL is rejected") {
    auto r = httpGet("");
    REQUIRE_FALSE(r.has_value());
    CHECK_FALSE(r.error().empty());
}

TEST_CASE("a scheme-relative URL is rejected") {
    auto r = httpGet("//example.com/apps.json");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("http") != std::string::npos);
}

TEST_CASE("a URL with leading whitespace is rejected") {
    auto r = httpGet(" https://example.com/apps.json");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("http") != std::string::npos);
}

TEST_CASE("an upper-case scheme is NOT rejected as an unsupported scheme") {
    // Final review (Fix 3). This file's scheme test used to be a case-SENSITIVE
    // rfind(), while the settings layer that validates what the user types
    // compares the scheme case-insensitively as RFC 3986 requires. So
    // "HTTPS://example.com/apps.json" was accepted by the App Explorer tab's
    // control, written to settings.ini, and then failed every fetch from then
    // on -- this session and every future launch -- with "only http and https
    // URLs are supported", for a URL that plainly is https. Both layers now
    // share fwog::hasHttpScheme().
    //
    // Deliberately a scheme with NO HOST after it, so every platform's
    // transport rejects it locally (a malformed URL) without a DNS lookup or a
    // socket -- this test must not depend on the network, and must not hang on
    // a machine that has none. The assertion is "it did not fail for the
    // SCHEME", which is exactly the defect: the scheme is fine, and the error
    // has to come from somewhere else.
    for (const char* url : { "HTTPS://", "HtTpS://", "HTTP://" }) {
        auto r = httpGet(url);
        REQUIRE_FALSE(r.has_value());   // no host: it cannot succeed
        CHECK(r.error().find("only http and https URLs are supported")
              == std::string::npos);
    }
}

TEST_CASE("an https request may not be redirected onto plain http") {
    // The redirect policy, pinned as a rule rather than as a libcurl option.
    //
    // This is the guarantee WinHTTP has always given the Windows build for
    // free -- WINHTTP_OPTION_REDIRECT_POLICY defaults to
    // DISALLOW_HTTPS_TO_HTTP -- and that the POSIX build did not give at all
    // until it started asking libcurl for it explicitly: libcurl's default
    // allows HTTP, HTTPS, FTP and FTPS on redirect, so a server could walk an
    // https catalog URL down to plain http.
    //
    // Why it matters here specifically and not just in general: a catalog
    // entry is authoritative over the target CPU, which is why
    // normalizeRemoteCatalogUrl() refuses to let a user configure a plain-http
    // catalog URL in the first place. A downgrade redirect would re-open that
    // hole from the server side, where the user cannot see it.
    for (const char* url : { "https://example.com/apps.json",
                             "HTTPS://example.com/apps.json",   // RFC 3986: schemes are
                             "HtTpS://example.com/apps.json" }) // case-insensitive
        CHECK_FALSE(mayRedirectToPlainHttp(url));
}

TEST_CASE("a plain-http request keeps both schemes on redirect") {
    // The rule is a DOWNGRADE ban, not an https-only ban. A request that was
    // already plain http has no TLS to lose, and refusing http -> http would
    // break ordinary redirects for the non-catalog things fetched with this
    // transport. Asserting this is what stops the rule from being "silently
    // return false for everything", which the case above alone would pass.
    for (const char* url : { "http://example.com/apps.json",
                             "HTTP://example.com/apps.json" })
        CHECK(mayRedirectToPlainHttp(url));
}

TEST_CASE("a request made with no transport fails cleanly") {
    // The contract that matters for the offline story: with no transport,
    // httpGet returns an error naming the reason. It never throws and never
    // crashes -- the caller shows a status line and the cached catalog stays
    // on screen.
    if (!httpAvailable()) {
        auto r = httpGet("https://example.com/apps.json");
        REQUIRE_FALSE(r.has_value());
        CHECK_FALSE(r.error().empty());
    }
}
