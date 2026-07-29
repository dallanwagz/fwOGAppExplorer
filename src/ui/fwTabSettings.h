#pragma once

#include <string>

namespace fwog {

class RemoteCatalog;

/// The Settings tab: where the app is CONFIGURED, as distinct from where it is
/// used. Today that is the remote catalog address and nothing else.
///
/// The split from the App Explorer tab is the point. A catalog URL is typed
/// once and then never again, and it was sitting full-width above the app list
/// on the tab a user spends all their time in -- permanent chrome for a
/// one-time decision. The ACTION it enables ("fetch now") is not configuration
/// and stayed behind on App Explorer, next to the filter chips, as Online
/// Update: that is a thing you do while browsing, so it belongs where the
/// browsing is.
///
/// Owns only the edit buffer and the last rejection message. The URL itself
/// lives in the caller's Settings (App::run()), which is what carries it to
/// disk at exit.
class SettingsTab {
public:
    /// `remoteCatalogUrl` is the caller's live Settings field: assigning to it
    /// is what makes saveSettingsMerged() notice the change. `remoteCatalog` is
    /// the fetcher, started immediately on a valid new address so the change
    /// takes effect without a restart.
    void draw(RemoteCatalog& remoteCatalog, std::string& remoteCatalogUrl);

private:
    /// Seeded ONCE from the caller's stored URL and thereafter owned by
    /// whatever the user is typing: re-seeding every frame would fight the text
    /// cursor. 512 bytes is comfortably more than any real apps.json address; a
    /// longer paste is truncated by ImGui, which is visible in the field rather
    /// than silent.
    char        m_urlBuf[512] = {};
    bool        m_urlPrimed = false;
    std::string m_urlError;      ///< empty == the last change was accepted
};

} // namespace fwog
