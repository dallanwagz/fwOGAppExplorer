#include <doctest/doctest.h>
#include "core/fwSettingsIo.h"

#include <string>

using namespace fwog;

// ---------------------------------------------------------------------------
// normalizeRemoteCatalogUrl
// ---------------------------------------------------------------------------

TEST_CASE("an empty or whitespace-only URL is accepted as 'no remote catalog', not rejected") {
    // Clearing the field is how the user turns the remote catalog back off,
    // so this must not be an error -- the App Explorer tab distinguishes the
    // empty value (explained as "not configured") from a rejection.
    auto blank = normalizeRemoteCatalogUrl("");
    REQUIRE(blank.has_value());
    CHECK(blank->empty());

    auto spaces = normalizeRemoteCatalogUrl("   \t\r\n ");
    REQUIRE(spaces.has_value());
    CHECK(spaces->empty());
}

TEST_CASE("surrounding whitespace picked up by a copy/paste is trimmed off") {
    auto trimmed = normalizeRemoteCatalogUrl("  https://example.com/apps.json \r\n");
    REQUIRE(trimmed.has_value());
    CHECK(*trimmed == "https://example.com/apps.json");
}

TEST_CASE("a URL with no scheme is refused with an explanation, not silently repaired") {
    auto bare = normalizeRemoteCatalogUrl("example.com/apps.json");
    REQUIRE_FALSE(bare.has_value());
    CHECK(bare.error().find("https://") != std::string::npos);

    // A local path is the other thing a user is likely to try; it fails for
    // the same reason and gets the same sentence.
    auto localPath = normalizeRemoteCatalogUrl("C:\\catalog\\apps.json");
    REQUIRE_FALSE(localPath.has_value());
    CHECK_FALSE(localPath.error().empty());

    auto otherScheme = normalizeRemoteCatalogUrl("ftp://example.com/apps.json");
    REQUIRE_FALSE(otherScheme.has_value());
    CHECK_FALSE(otherScheme.error().empty());
}

TEST_CASE("https is accepted, and the scheme is matched case-insensitively") {
    // RFC 3986 says schemes are case-insensitive; a pasted "HTTPS://" is a
    // perfectly good address and refusing it would be a lie.
    auto plain = normalizeRemoteCatalogUrl("https://example.com/apps.json");
    REQUIRE(plain.has_value());
    CHECK(*plain == "https://example.com/apps.json");

    auto shouty = normalizeRemoteCatalogUrl("HTTPS://Example.COM/apps.json");
    REQUIRE(shouty.has_value());
    // Case is preserved, only compared insensitively -- the path segment of a
    // URL IS case-sensitive, so lowercasing the whole thing would break it.
    CHECK(*shouty == "HTTPS://Example.COM/apps.json");
}

TEST_CASE("plain http is refused for the catalog, with the reason rather than just 'invalid'") {
    // Final review (Fix 4): a catalog entry is AUTHORITATIVE OVER THE TARGET
    // CPU -- an entry declaring "flashScheme":"DisplayBootloader" produces a
    // DISPLAY-targeted plan that runs the ordinary path with no typed
    // confirmation, and a main-CPU image on the DISPLAY CPU can damage the
    // board. sha256 is optional in the schema and self-attested by the same
    // document, and parseUf2 cannot tell a MAIN image from a DISPLAY one, so
    // nothing downstream can catch a rewritten catalog. The design spec has
    // always said this is fetched over HTTPS.
    auto plaintext = normalizeRemoteCatalogUrl("http://example.com/apps.json");
    REQUIRE_FALSE(plaintext.has_value());
    CHECK(plaintext.error().find("https://") != std::string::npos);
    // Not a bare "invalid": it has to say WHY, in the app's own voice.
    CHECK(plaintext.error().find("CPU") != std::string::npos);

    // Case-insensitively too -- "HTTP://" is the same refusal, not an
    // accidental way around it.
    auto shouty = normalizeRemoteCatalogUrl("HTTP://Example.COM/apps.json");
    REQUIRE_FALSE(shouty.has_value());
    CHECK_FALSE(shouty.error().empty());

    // Including the LAN case, which is the one a user is most likely to hit.
    // It is refused for the same reason: this app cannot tell a friendly LAN
    // from a hostile one, and the consequence of being wrong is physical.
    auto lan = normalizeRemoteCatalogUrl("http://192.168.1.50:8080/catalog/apps.json");
    REQUIRE_FALSE(lan.has_value());
    CHECK_FALSE(lan.error().empty());
}

TEST_CASE("a scheme with no host after it is refused") {
    auto noHost = normalizeRemoteCatalogUrl("https://");
    REQUIRE_FALSE(noHost.has_value());
    CHECK_FALSE(noHost.error().empty());

    // "http://" alone is refused too -- by the plaintext rule, which is
    // checked first. Either sentence is a refusal; what matters is that it
    // never comes back as an accepted value.
    auto noHostHttp = normalizeRemoteCatalogUrl("  http://  ");
    REQUIRE_FALSE(noHostHttp.has_value());
    CHECK_FALSE(noHostHttp.error().empty());
}

// ---------------------------------------------------------------------------
// hasHttpScheme / hasHttpsScheme -- the ONE scheme predicate, shared with
// fwHttp.cpp's checkUrl().
//
// Final review (Fix 3): fwHttp.cpp used to carry its own case-SENSITIVE
// rfind("https://", 0) == 0 while this file compared the scheme
// case-INsensitively. The two disagreed, so "HTTPS://example.com/apps.json"
// was accepted by the App Explorer control, written to settings.ini, and then
// failed every fetch -- this session and every future launch -- with "only
// http and https URLs are supported", for a URL that plainly is https.
// ---------------------------------------------------------------------------

TEST_CASE("the scheme predicates are case-insensitive in every mix") {
    CHECK(hasHttpsScheme("https://example.com/a"));
    CHECK(hasHttpsScheme("HTTPS://example.com/a"));
    CHECK(hasHttpsScheme("HtTpS://example.com/a"));
    CHECK_FALSE(hasHttpsScheme("http://example.com/a"));
    CHECK_FALSE(hasHttpsScheme("HTTP://example.com/a"));

    CHECK(hasHttpScheme("http://example.com/a"));
    CHECK(hasHttpScheme("HTTP://example.com/a"));
    CHECK(hasHttpScheme("HtTp://example.com/a"));
    CHECK(hasHttpScheme("https://example.com/a"));
    CHECK(hasHttpScheme("HTTPS://example.com/a"));
}

TEST_CASE("the scheme predicates reject anything that is not an http scheme") {
    CHECK_FALSE(hasHttpScheme(""));
    CHECK_FALSE(hasHttpScheme("http"));
    CHECK_FALSE(hasHttpScheme("http:/example.com"));
    CHECK_FALSE(hasHttpScheme("ftp://example.com/a"));
    CHECK_FALSE(hasHttpScheme("file:///etc/passwd"));
    CHECK_FALSE(hasHttpScheme("//example.com/a"));
    // Leading whitespace is NOT tolerated here: trimming is
    // normalizeRemoteCatalogUrl's job, and httpGet() must not quietly accept
    // an address its transport would then send verbatim.
    CHECK_FALSE(hasHttpScheme(" https://example.com/a"));
    // The scheme must be a PREFIX, not merely present somewhere.
    CHECK_FALSE(hasHttpScheme("x-https://example.com/a"));
}

TEST_CASE("a URL the settings layer accepts is one the fetcher's predicate also accepts") {
    // The property the split predicates broke. Whatever normalization returns
    // non-empty must pass the transport check, or the URL is stored and then
    // refused forever.
    const char* inputs[] = {
        "https://example.com/apps.json",
        "HTTPS://Example.COM/apps.json",
        "  https://example.com/apps.json?ref=stable&v=2  ",
    };
    for (const char* raw : inputs) {
        auto normalized = normalizeRemoteCatalogUrl(raw);
        REQUIRE(normalized.has_value());
        REQUIRE_FALSE(normalized->empty());
        CHECK(hasHttpScheme(*normalized));
    }
}

TEST_CASE("a line break anywhere in the URL is refused rather than stripped") {
    // settings.ini is one key=value per line: an embedded newline would split
    // the value in half and silently truncate it on the next load.
    auto embedded = normalizeRemoteCatalogUrl("https://example.com/a\npps.json");
    REQUIRE_FALSE(embedded.has_value());
    CHECK_FALSE(embedded.error().empty());

    auto tabbed = normalizeRemoteCatalogUrl("https://example.com/app\ts.json");
    REQUIRE_FALSE(tabbed.has_value());
    CHECK_FALSE(tabbed.error().empty());
}

TEST_CASE("'=' and '&' inside a query string are preserved -- they are not a settings delimiter") {
    auto query = normalizeRemoteCatalogUrl("https://example.com/apps.json?ref=stable&v=2");
    REQUIRE(query.has_value());
    CHECK(*query == "https://example.com/apps.json?ref=stable&v=2");
}

// ---------------------------------------------------------------------------
// formatSettingsLine / parseSettingsLine
// ---------------------------------------------------------------------------

TEST_CASE("a formatted settings line parses back to the same key and value") {
    const auto parsed = parseSettingsLine(formatSettingsLine("theme", "Wili"));
    REQUIRE(parsed.has_value());
    CHECK(parsed->first == "theme");
    CHECK(parsed->second == "Wili");
}

TEST_CASE("a URL containing '=' survives the settings round trip byte for byte") {
    // The reason parseSettingsLine splits at the FIRST '=': a query string is
    // full of them, and splitting anywhere else would corrupt the value.
    const std::string url = "https://example.com/apps.json?ref=stable&v=2&sig=a=b";
    const auto parsed = parseSettingsLine(formatSettingsLine("remoteCatalogUrl", url));
    REQUIRE(parsed.has_value());
    CHECK(parsed->first == "remoteCatalogUrl");
    CHECK(parsed->second == url);
}

TEST_CASE("a normalized URL is a fixed point of the settings round trip") {
    // The property the app actually depends on: whatever the tab's control
    // accepts is what comes back out of settings.ini on the next launch, so
    // the second run fetches the same address the first one did.
    const char* inputs[] = {
        "  https://example.com/apps.json  ",
        "https://192.168.1.50:8080/catalog/apps.json",
        "https://example.com/apps.json?ref=stable&v=2",
        "HTTPS://Example.COM/Apps.JSON",
    };
    for (const char* raw : inputs) {
        auto normalized = normalizeRemoteCatalogUrl(raw);
        REQUIRE(normalized.has_value());
        const auto parsed = parseSettingsLine(formatSettingsLine("remoteCatalogUrl", *normalized));
        REQUIRE(parsed.has_value());
        CHECK(parsed->second == *normalized);
        // And normalising the round-tripped value again changes nothing --
        // loading a settings.ini this app wrote can never drift.
        auto again = normalizeRemoteCatalogUrl(parsed->second);
        REQUIRE(again.has_value());
        CHECK(*again == *normalized);
    }
}

TEST_CASE("a settings line ends in exactly one newline so consecutive lines stay separate") {
    const std::string line = formatSettingsLine("lastTab", "2");
    CHECK(line == "lastTab=2\n");
}

TEST_CASE("a trailing CR from a Windows-written file is dropped from the value") {
    // A settings.ini written with CRLF and read by a getline() that leaves the
    // CR behind must yield the same value as one written with bare LF --
    // otherwise a URL would come back with an invisible '\r' glued to it and
    // every fetch would 404.
    const auto parsed = parseSettingsLine("remoteCatalogUrl=https://example.com/apps.json\r\n");
    REQUIRE(parsed.has_value());
    CHECK(parsed->second == "https://example.com/apps.json");
}

TEST_CASE("whitespace around a hand-edited key or value is trimmed") {
    const auto parsed = parseSettingsLine("  remoteCatalogUrl  =   https://example.com/apps.json   ");
    REQUIRE(parsed.has_value());
    CHECK(parsed->first == "remoteCatalogUrl");
    CHECK(parsed->second == "https://example.com/apps.json");
}

TEST_CASE("an empty value round trips as empty, which is how the URL is cleared") {
    const auto parsed = parseSettingsLine(formatSettingsLine("remoteCatalogUrl", ""));
    REQUIRE(parsed.has_value());
    CHECK(parsed->first == "remoteCatalogUrl");
    CHECK(parsed->second.empty());
}

TEST_CASE("lines that name no setting are skipped rather than parsed into junk") {
    CHECK_FALSE(parseSettingsLine("").has_value());
    CHECK_FALSE(parseSettingsLine("# a comment someone added by hand").has_value());
    CHECK_FALSE(parseSettingsLine("noEqualsSignHere").has_value());
    CHECK_FALSE(parseSettingsLine("=valueWithNoKey").has_value());
    CHECK_FALSE(parseSettingsLine("   =valueWithBlankKey").has_value());
}
