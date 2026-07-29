#include "ui/fwTabSettings.h"

#include "catalog/fwCatalogRemote.h"
#include "core/fwSettingsIo.h"   // normalizeRemoteCatalogUrl

#include <imgui.h>
#include <IconsMaterialDesign.h>

#include <cstring>

namespace fwog {
namespace {

const ImVec4 kMutedColor { 0.65f, 0.65f, 0.65f, 1.00f };
const ImVec4 kWarnColor  { 0.90f, 0.65f, 0.15f, 1.00f };
const ImVec4 kErrorColor { 0.95f, 0.35f, 0.35f, 1.00f };
const ImVec4 kAccentColor{ 0.30f, 0.55f, 0.95f, 1.00f };

void copyToTextBuffer(char* dst, size_t dstLen, const std::string& src)
{
    const size_t n = src.size() < dstLen - 1 ? src.size() : dstLen - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

/// TextWrapped, never TextColored, for anything unbounded: a URL or a transport
/// error that runs past the pane's right edge silently hides whatever ran off
/// it. Same rule the other tabs apply to the same kind of text.
void wrappedColored(const ImVec4& colour, const std::string& text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

} // namespace

void SettingsTab::draw(RemoteCatalog& remoteCatalog, std::string& remoteCatalogUrl)
{
    // Seeded once, from whatever settings.ini restored at startup; after that
    // the buffer is the user's to type in. See the member's declaration.
    if (!m_urlPrimed) {
        copyToTextBuffer(m_urlBuf, sizeof(m_urlBuf), remoteCatalogUrl);
        m_urlPrimed = true;
    }

    ImGui::TextUnformatted(ICON_MD_CLOUD_DOWNLOAD " Remote catalog");
    ImGui::TextColored(kMutedColor,
                       "An apps.json to browse alongside the built-in apps and the catalog folder. "
                       "Fetch it with Online Update on the App Explorer tab.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(-1.0f);
    // EnterReturnsTrue so the obvious gesture after typing a URL -- press Enter
    // -- does the same thing as the Save button, instead of appearing to do
    // nothing.
    const bool submitted = ImGui::InputTextWithHint(
        "##RemoteCatalogUrl",
        "https://example.com/apps.json",
        m_urlBuf, sizeof(m_urlBuf),
        ImGuiInputTextFlags_EnterReturnsTrue);
    // A rejection message describes text that is no longer on screen once the
    // user starts fixing it, so it goes away as soon as they do.
    if (ImGui::IsItemEdited()) m_urlError.clear();

    // "Save", not "Update": this stores an address, and the FETCHING it may
    // kick off is a side effect of the address having changed. The button that
    // deliberately re-fetches is Online Update, on the App Explorer tab. Two
    // buttons a tab apart called the same thing would be two names for what a
    // user would reasonably read as one action.
    const bool clicked = ImGui::Button(ICON_MD_SAVE " Save");

    if (clicked || submitted) {
        auto normalized = normalizeRemoteCatalogUrl(m_urlBuf);
        if (!normalized) {
            // Nothing is stored and no fetch starts: the typed text stays in
            // the field, with the reason it was refused underneath it.
            m_urlError = normalized.error();
        } else {
            m_urlError.clear();
            // Show back exactly what was stored. Without this a trailing space
            // that normalisation removed would still be sitting in the field,
            // and the user would have no way to tell whether it had been kept.
            copyToTextBuffer(m_urlBuf, sizeof(m_urlBuf), *normalized);
            // The caller's live Settings field: assigning here is what makes
            // saveSettingsMerged() (fwApp.cpp) notice this run changed the URL
            // and carry it to disk at exit.
            remoteCatalogUrl = *normalized;
            // An empty URL is "turn the remote catalog off", not a fetch --
            // start("") would only produce a transport error for an address the
            // user has just deliberately cleared. Whatever was already fetched
            // stays visible for this session; nothing new is asked for, and the
            // next launch starts with no remote catalog at all.
            //
            // Still guarded on fetching(): start() is a silent no-op mid-fetch
            // (fwCatalogRemote.h), and saving a new address while the old one
            // is still downloading must not look like it took effect when it
            // did not. The address IS stored either way; only the immediate
            // fetch is skipped, and Online Update is there to ask again.
            if (!remoteCatalogUrl.empty() && !remoteCatalog.fetching())
                remoteCatalog.start(remoteCatalogUrl);
        }
    }

    // Exactly one line of explanation, always present, covering every state
    // this control can be in -- including, and especially, the do-nothing state
    // a fresh install starts in, which must read as "not configured" rather
    // than as a fetch that failed.
    const std::string status = remoteCatalog.status();
    if (!m_urlError.empty()) {
        wrappedColored(kErrorColor, m_urlError);
    } else if (remoteCatalog.fetching()) {
        wrappedColored(kAccentColor, "Fetching the catalog now.");
    } else if (!status.empty()) {
        // RemoteCatalog never clears good entries on a failure, so say what the
        // user is still looking at instead of leaving a bare error.
        wrappedColored(kWarnColor, "The last update did not succeed (" + status + "). The apps on the "
                                    "App Explorer tab are the ones this app already had.");
    } else if (remoteCatalogUrl.empty()) {
        // Careful with this wording: "built-in and local only" would be a lie
        // whenever apps-cache.json survives from a run that DID have a URL,
        // because RemoteCatalog::loadCache() reads that cache at startup
        // regardless of whether a URL is configured now. Saying what is and is
        // not being FETCHED is true in both cases.
        wrappedColored(kMutedColor, "No remote catalog is configured, so nothing is being fetched. The "
                                     "App Explorer tab shows the built-in apps, anything in the catalog "
                                     "folder beside this app, and anything an earlier run downloaded.");
    } else {
        wrappedColored(kMutedColor, "Browsing " + remoteCatalogUrl + " alongside the built-in and local "
                                     "catalogs.");
    }
}

} // namespace fwog
