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
