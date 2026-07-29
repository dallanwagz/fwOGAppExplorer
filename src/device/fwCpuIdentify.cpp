#include "device/fwCpuIdentify.h"

#include <string_view>

namespace fwog {
namespace {

// The trailing space is load-bearing: without it "FWOG displayfoo" would
// match. fw.py's _pick_cpu_port has the same requirement.
constexpr std::string_view kMainPrefix    = "FWOG main ";
constexpr std::string_view kDisplayPrefix = "FWOG display ";

bool startsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

/// Exactly-one-match or nothing. Two candidates are as useless as zero.
struct Candidate {
    const std::string* port = nullptr;
    int count = 0;

    void offer(const std::string& p) { if (++count == 1) port = &p; }
    bool resolved() const { return count == 1; }
};

/// Whichever of a CPU's port or bootrom volume was resolved, if either.
///
/// The two are mutually exclusive on real hardware -- a CPU runs firmware and
/// presents a CDC port, or sits in the bootrom and presents mass storage -- so
/// finding both for one CPU means the scan is describing a board that no longer
/// exists (a stale port record beside a live mount, most likely). That is
/// treated as no answer at all rather than by preferring one: this pass exists
/// to state where a CPU IS, and two contradictory claims do not.
struct Located {
    const std::string* port   = nullptr;
    const std::string* volume = nullptr;

    bool contradictory() const { return port && volume; }
    bool resolved() const { return (port || volume) && !contradictory(); }
};

} // namespace

CpuIdentity identifyCpus(std::span<const CpuPortRecord> records)
{
    CpuIdentity id;

    // --- Pass 1: structural. Position on the FreeWili's USB hub is
    // authoritative and holds regardless of what firmware is running, which
    // is what lets the deprecated firmware be targeted at all.
    //
    // Bootrom DRIVES are resolved here too, from the same hub ports and with
    // the same exactly-one-or-nothing rule. That is what makes a CPU sitting in
    // BOOTSEL identifiable: the two RP2040s publish an identical bootrom USB
    // serial, so nothing they say distinguishes them, but they are wired to
    // different ports of the board's own internal hub. Before this, such a CPU
    // came back "not identified" and the app fell back to asking the user or to
    // writing a prober image -- for a fact already sitting in the USB tree.
    Candidate hubMainPort, hubDisplayPort, hubMainVol, hubDisplayVol;
    for (const auto& r : records) {
        if (!r.port.empty()) {
            if (r.isSerialMain)    hubMainPort.offer(r.port);
            if (r.isSerialDisplay) hubDisplayPort.offer(r.port);
        }
        if (!r.volume.empty()) {
            if (r.isMassStorageMain)    hubMainVol.offer(r.volume);
            if (r.isMassStorageDisplay) hubDisplayVol.offer(r.volume);
        }
    }

    Located main, display;
    if (hubMainPort.resolved())    main.port      = hubMainPort.port;
    if (hubMainVol.resolved())     main.volume    = hubMainVol.port;
    if (hubDisplayPort.resolved()) display.port   = hubDisplayPort.port;
    if (hubDisplayVol.resolved())  display.volume = hubDisplayVol.port;

    if (main.resolved()) {
        if (main.port)   id.mainPort   = *main.port;
        if (main.volume) id.mainVolume = *main.volume;
        id.mainSource = IdentitySource::HubLocation;
    }
    if (display.resolved()) {
        if (display.port)   id.displayPort   = *display.port;
        if (display.volume) id.displayVolume = *display.volume;
        id.displaySource = IdentitySource::HubLocation;
    }
    // The product string of whichever record supplied the display PORT. Looked
    // up rather than carried through Located because it is not part of locating
    // anything -- it says what that CPU is RUNNING, which is a different
    // question answered by the same record. See ogBootloaderState().
    if (id.displayPort)
        for (const auto& r : records)
            if (r.port == *id.displayPort) { id.displayProduct = r.product; break; }

    // --- Pass 2: product strings, only for whichever CPU pass 1 missed.
    // Skip any port already identified in pass 1, to prevent a contradicting
    // product string from overriding structural position.
    if (main.resolved() && display.resolved()) return id;

    Candidate strMain, strDisplay;
    for (const auto& r : records) {
        if (r.port.empty() || r.product.empty()) continue;
        // A port carrying ANY structural signal has its role decided by hub
        // position -- or is ambiguous, in which case we refuse rather than guess.
        // Either way a product string must never reassign it. Guarding on the
        // resolved outcome instead would leave ambiguous hub-flagged ports eligible
        // for cross-role assignment in pass 2.
        if (r.isSerialMain || r.isSerialDisplay) continue;
        if (startsWith(r.product, kMainPrefix))    strMain.offer(r.port);
        if (startsWith(r.product, kDisplayPrefix)) strDisplay.offer(r.port);
    }
    // Guarded on the STRUCTURAL result, not on id.mainPort alone: a CPU located
    // by hub position as a bootrom drive has been located, and a product string
    // on some other port must not go on to claim it as well.
    if (!main.resolved() && strMain.resolved()) {
        id.mainPort   = *strMain.port;
        id.mainSource = IdentitySource::ProductString;
    }
    if (!display.resolved() && strDisplay.resolved()) {
        id.displayPort   = *strDisplay.port;
        id.displaySource = IdentitySource::ProductString;
    }
    // Pass 2 can resolve a display port pass 1 did not, so the product lookup
    // repeats here rather than only above.
    if (id.displayPort && id.displayProduct.empty())
        for (const auto& r : records)
            if (r.port == *id.displayPort) { id.displayProduct = r.product; break; }

    return id;
}

OgBootloaderState ogBootloaderState(const CpuIdentity& identity)
{
    // Order matters: a CPU answering on a port is RUNNING something, and what
    // it is running is the direct evidence. Only when nothing is answering do we
    // fall back to "is it sitting in its bootrom", which is evidence that
    // nothing is installed at all.
    if (identity.displayPort) {
        // "FWOG " with the trailing space, the same test identifyCpus() uses
        // for its product prefixes: without it "FWOGGLE" would match.
        constexpr std::string_view kOgPrefix = "FWOG ";
        const std::string& p = identity.displayProduct;
        // An EMPTY product string is not evidence of anything -- some hosts
        // simply do not report one -- so it is Unknown, never Missing. Claiming
        // a working board has no bootloader would send a user to erase and
        // reflash a CPU that was fine.
        if (p.empty()) return OgBootloaderState::Unknown;
        return p.rfind(kOgPrefix, 0) == 0 ? OgBootloaderState::Present
                                          : OgBootloaderState::Missing;
    }
    // A bootrom drive where the DISPLAY CPU should be: blank flash, so nothing
    // is installed on it.
    if (identity.displayVolume) return OgBootloaderState::Missing;

    return OgBootloaderState::Unknown;
}

} // namespace fwog
