#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace fwog {

// ---------------------------------------------------------------------------
// The text layer of settings.ini, and the one value in it a user can now type
// by hand inside the app.
//
// Kept here -- pure, header-only-dependency, no SDL and no ImGui -- rather
// than inside fwApp.cpp's anonymous namespace, so the round trip these
// functions define ("what saveSettings() writes is exactly what loadSettings()
// reads back") can be tested directly instead of only by launching the app and
// squinting at a file. fwApp.cpp keeps the typed Settings struct and the
// two-instance merge; only the string handling lives down here.
// ---------------------------------------------------------------------------

/// True when `url` begins with "https://", compared case-insensitively.
///
/// RFC 3986 says a scheme is case-insensitive, so "HTTPS://example.com/..." is
/// a perfectly good address. The comparison is ASCII-only on purpose: a scheme
/// can only ever contain letters, digits, '+', '-' and '.', and using
/// std::tolower here would drag the C locale -- and its "is this byte a
/// letter?" surprises on UTF-8 input -- into a decision that has no business
/// being locale-aware.
bool hasHttpsScheme(std::string_view url);

/// True when `url` begins with "http://" OR "https://", compared
/// case-insensitively (see hasHttpsScheme above for why).
///
/// This is the general TRANSPORT predicate: it answers "can httpGet() fetch
/// this at all", and fwHttp.cpp's checkUrl() is its one caller besides this
/// header's own normalizeRemoteCatalogUrl(). It deliberately lives here, in
/// fwog_core, rather than in fwog_platform: fwog_core cannot depend on
/// fwog_platform, and having TWO scheme predicates is exactly the defect this
/// replaced -- fwHttp.cpp used to compare the scheme case-SENSITIVELY while
/// this file compared it case-INsensitively, so a pasted "HTTPS://..." was
/// accepted by the settings layer, written to settings.ini, and then refused
/// by every fetch from then on with "only http and https URLs are supported"
/// for a URL that plainly was https. One predicate, one behaviour.
bool hasHttpScheme(std::string_view url);

/// Cleans up a remote catalog URL the user typed or pasted, or explains why it
/// cannot be used.
///
/// Success may legitimately be an EMPTY string: clearing the field is how a
/// user turns the remote catalog back off, and the App Explorer tab explains
/// that state rather than reporting it as a failed fetch. So callers must
/// check the value, not just the success, before starting a fetch.
///
/// Three things are enforced, each for a concrete reason:
///
///  * Surrounding whitespace is trimmed. Copying a URL out of a browser or a
///    chat window routinely brings a trailing space or newline along, and a
///    trailing space would otherwise be stored, sent to the server, and turn
///    a perfectly good address into a 404 the user cannot see.
///
///  * Line breaks and other control characters are REFUSED rather than
///    stripped. settings.ini is one `key=value` per line (see
///    formatSettingsLine below), so an embedded newline would split the value
///    across two lines and silently truncate it on the next load. Refusing
///    says so; stripping would quietly store something other than what was
///    pasted, which is usually a sign the user pasted the wrong thing anyway.
///
///  * The scheme must be https:// (compared case-insensitively -- RFC 3986
///    says schemes are). This is fetched over the network by httpGet(); a bare
///    "example.com/apps.json" or a local file path cannot work, and failing
///    here with a sentence beats failing later with a transport error the user
///    has to decode.
///
///    Plain http:// is REFUSED, and this is stricter than httpGet() itself is
///    (hasHttpScheme() above still accepts both -- it is the general transport
///    predicate and other things are fetched with it). The restriction belongs
///    here because of what a catalog document specifically is: a catalog entry
///    is AUTHORITATIVE OVER THE TARGET CPU. An entry declaring
///    `"flashScheme":"DisplayBootloader"` with a display-targeted UF2 produces
///    a DISPLAY-targeted plan that runs the ordinary path with no typed
///    confirmation, and a main-CPU image written to the DISPLAY CPU drives
///    GPIO 29 against the PDM microphone's own output -- the physical hazard
///    this whole app is built around. Nothing downstream can catch it: sha256
///    is optional in the schema and is self-attested by the same document, and
///    parseUf2() cannot tell a MAIN image from a DISPLAY one (both are RP2040
///    family 0xE48BFF56). Over plain http anything between this computer and
///    the server can rewrite that document, and the app has no way to notice.
///    The design spec has always said the remote catalog is fetched over
///    HTTPS; this is where that is enforced.
///
/// A '=' inside the URL is explicitly FINE -- query strings are full of them,
/// and parseSettingsLine() splits at the first '=' precisely so that a value
/// containing more of them survives the round trip intact.
std::expected<std::string, std::string> normalizeRemoteCatalogUrl(std::string_view raw);

/// The catalog this app ships pointed at: the published FreeWili OG app
/// catalog on the documentation site.
///
/// A DEFAULT, not a hardcoded destination. It seeds the field on a first run
/// so that a user who installs the app sees real apps without first being told
/// an address to paste (the gap the Settings tab's control closed only halfway
/// -- reachable, but still undiscoverable). Everything downstream treats it as
/// exactly as trustworthy as a URL the user typed: it is fetched over https,
/// it goes through normalizeRemoteCatalogUrl() like any other, and the Settings
/// tab can overwrite or clear it.
///
/// Kept here rather than in fwApp.cpp so the seeding RULE below and the value
/// it seeds are one unit, and so a test can assert the shipped default is a
/// URL this app would have accepted from a user.
std::string_view defaultRemoteCatalogUrl();

/// The remote catalog URL a launch should start with.
///
/// `stored` is what settings.ini held for `remoteCatalogUrl` (already through
/// normalizeRemoteCatalogUrl, so empty covers "the key was absent", "the value
/// was empty" and "the value was rejected"). `defaultSeeded` is whether the
/// file carried the `remoteCatalogDefaultSeeded` marker.
///
/// The marker exists to tell two states apart that `stored` alone cannot:
///
///   * NEVER CONFIGURED -- a fresh install, or an install upgraded from a build
///     that shipped no default. Seed the default, so the app arrives useful.
///   * DELIBERATELY CLEARED -- the user emptied the field and pressed Save.
///     That is a choice, and re-seeding the default on the next launch would
///     silently undo it, which is the one behaviour a "turn it off" control
///     must never have.
///
/// Both look like an empty `remoteCatalogUrl=` line on disk. Only the marker,
/// written from the first save onward, separates them -- which is also what
/// makes this safe for the users who ALREADY have a settings.ini holding an
/// empty URL from an earlier build: they have no marker, so they are treated as
/// never-configured and get the default, rather than being permanently stuck
/// with a remote catalog that never turns on.
///
/// A non-empty `stored` always wins, seeded or not: a URL in the file is a URL
/// somebody chose.
std::string remoteCatalogUrlAtStartup(std::string_view stored, bool defaultSeeded);

/// One complete settings.ini line, trailing newline included, ready to stream
/// straight out. The value is written verbatim: callers are responsible for
/// having rejected anything containing a newline first (see
/// normalizeRemoteCatalogUrl above for the only free-text value this file
/// stores).
std::string formatSettingsLine(std::string_view key, std::string_view value);

/// The inverse of formatSettingsLine(): splits one line into its key and
/// value, or returns nullopt for a line this format has nothing to say about
/// (no '=' at all, or an empty key).
///
/// Splits at the FIRST '=' and keeps everything after it, so a value may
/// itself contain '='. Any trailing CR/LF is dropped, so a settings.ini
/// written on Windows and read on a platform whose getline() leaves the CR
/// behind parses the same either way, and surrounding whitespace is trimmed
/// from both key and value, so a hand-edited file stays readable.
std::optional<std::pair<std::string, std::string>> parseSettingsLine(std::string_view line);

} // namespace fwog
