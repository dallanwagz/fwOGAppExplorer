#include "core/fwSettingsIo.h"

namespace fwog {
namespace {

/// ASCII whitespace only, deliberately: this trims what a copy/paste picks up
/// (space, tab, CR, LF, and the two vertical whitespace oddities), and leaves
/// every byte >= 0x80 alone so a URL carrying UTF-8 is passed through
/// untouched rather than half-eaten by a locale-dependent isspace().
bool isAsciiSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string_view trimAsciiSpace(std::string_view s)
{
    while (!s.empty() && isAsciiSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isAsciiSpace(s.back()))  s.remove_suffix(1);
    return s;
}

/// ASCII-only case fold, which is all a URL scheme can ever need (RFC 3986
/// restricts schemes to letters, digits, '+', '-' and '.'), and which avoids
/// dragging the C locale -- and its "is this byte a letter?" surprises on
/// UTF-8 input -- into a comparison that has no business being locale-aware.
bool startsWithNoCase(std::string_view s, std::string_view prefix)
{
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        char a = s[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

} // namespace

bool hasHttpsScheme(std::string_view url)
{
    return startsWithNoCase(url, "https://");
}

bool hasHttpScheme(std::string_view url)
{
    return hasHttpsScheme(url) || startsWithNoCase(url, "http://");
}

std::expected<std::string, std::string> normalizeRemoteCatalogUrl(std::string_view raw)
{
    const std::string_view trimmed = trimAsciiSpace(raw);

    // Empty (or whitespace-only) is a valid answer, not an error: it means
    // "no remote catalog", which is the state a fresh install starts in and
    // the state a user returns to by clearing the field.
    if (trimmed.empty()) return std::string{};

    for (char c : trimmed) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F)
            return std::unexpected(std::string(
                "A catalog URL has to be a single line, and this one contains a line break or "
                "another control character. Paste just the address itself, without the text "
                "around it."));
    }

    // Plain http:// is refused SEPARATELY from "no scheme at all", and with a
    // different sentence, because it is a different mistake: the address is
    // well-formed and would fetch fine, and the user needs the reason it is
    // still not good enough here rather than a generic "invalid". See
    // normalizeRemoteCatalogUrl's header comment (fwSettingsIo.h) for the
    // full reasoning -- in short, a catalog entry decides which of the board's
    // two CPUs gets written, and that decision must not be rewritable in
    // transit by anything between this computer and the server.
    if (!hasHttpsScheme(trimmed)) {
        if (hasHttpScheme(trimmed))
            return std::unexpected(std::string(
                "A catalog URL has to begin with https://, and this one begins with http://. "
                "The catalog decides which firmware image is written to which of the board's "
                "two CPUs, and writing a main-CPU image to the display CPU can damage the "
                "board -- so the catalog is fetched over an encrypted, authenticated "
                "connection, where nothing between this computer and the server can change "
                "what it says. Use the https:// address for the same catalog."));
        return std::unexpected(std::string(
            "A catalog URL has to begin with https://, because the app fetches it over the "
            "network. A bare host name or a path to a file on this computer cannot be used "
            "here -- files placed in the catalog folder beside the app are picked up "
            "automatically instead."));
    }

    // "https://" and nothing else parses as a scheme with no host. httpGet()
    // would fail on it, but with a transport error rather than the one thing
    // the user needs to be told, which is that the address is incomplete.
    constexpr std::size_t kHttpsSchemeLength = 8;   // "https://"
    if (trimmed.size() == kHttpsSchemeLength)
        return std::unexpected(std::string(
            "A catalog URL needs a host after the scheme, for example "
            "https://example.com/apps.json."));

    return std::string(trimmed);
}

std::string formatSettingsLine(std::string_view key, std::string_view value)
{
    std::string line;
    line.reserve(key.size() + value.size() + 2);
    line += key;
    line += '=';
    line += value;
    line += '\n';
    return line;
}

std::optional<std::pair<std::string, std::string>> parseSettingsLine(std::string_view line)
{
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) return std::nullopt;

    // Everything after the FIRST '=' is the value, '=' signs and all. This is
    // what lets a catalog URL with a query string ("...?ref=stable&v=2") come
    // back out of the file byte-for-byte as it went in; splitting at the last
    // '=', or refusing lines with more than one, would silently corrupt it.
    const std::string_view key   = trimAsciiSpace(line.substr(0, eq));
    const std::string_view value = trimAsciiSpace(line.substr(eq + 1));

    // "=something" names no setting; there is nothing a caller could do with
    // it, so it is skipped exactly like a line with no '=' at all.
    if (key.empty()) return std::nullopt;

    return std::make_pair(std::string(key), std::string(value));
}

} // namespace fwog
