#include "device/fwDeviceModel.h"

#include "device/fwFinderManager.h"

#ifndef __EMSCRIPTEN__
#include "device/fwDeviceRecords.h"
#include "device/fwCpuIdentify.h"
#endif

#include <utility>

namespace fwog {

bool serialIsUnidentified(std::string_view serial)
{
    return serial.empty() || serial == "Unknown";
}

namespace {

// "not identified" / "by hub position" / "by USB product name" / "by CPU
// probe" are the exact substrings the device-bar and Recovery-tab text, and
// the tests, key off of. Do not reword without checking both.
std::string cpuLine(const char* label, const std::optional<std::string>& port,
                    const std::optional<std::string>& volume, IdentitySource source)
{
    std::string s = label;
    s += ": ";
    // The port wins when there is one -- see describeIdentity()'s comment. A
    // pointer rather than a ternary over the two optionals, which would copy
    // the string every call for no reason.
    const std::optional<std::string>* shown = port.has_value() ? &port : &volume;
    if (!shown->has_value()) {
        s += "not identified";
        return s;
    }
    s += **shown;
    s += " (";
    switch (source) {
    case IdentitySource::HubLocation:   s += "by hub position";     break;
    case IdentitySource::ProductString: s += "by USB product name"; break;
    case IdentitySource::VerifiedProbe: s += "by CPU probe";        break;
    case IdentitySource::None:          s += "unknown";             break; // should not happen: a resolved port or volume always sets a source
    }
    s += ")";
    return s;
}

// Production's ScanFn: the fwfinder round trip that used to live directly in
// refresh(). Pulled out to a free function so it can be handed to the
// production constructor as a ScanFn, exactly like any test's fake -- refresh()
// itself no longer knows or cares whether it is talking to real hardware.
std::expected<std::vector<DeviceView>, std::string> defaultScan()
{
    auto result = fwFinderManager::instance().getDevices(false);
    if (!result) return std::unexpected(result.error());

    std::vector<DeviceView> next;
#ifndef __EMSCRIPTEN__
    next.reserve(result->size());
    for (const auto& device : *result) {
        DeviceView view;
        view.name     = device.name;
        view.serial   = device.serial;
        view.isOg     = (device.deviceType == Fw::DeviceType::FreeWili);
        view.identity = identifyCpus(toCpuPortRecords(device));
        view.uniqueID = device.uniqueID;
        next.push_back(std::move(view));
    }
#endif
    // On the web build fwfinder does not exist, getDevices() always returns
    // an empty list, and `next` stays empty -- the bar shows the desktop-only
    // notice (Task 21) rather than a device list.
    return next;
}

} // namespace

std::string describeIdentity(const CpuIdentity& id)
{
    // Both halves are always present -- never omit a CPU. Silence would read
    // as "everything is fine"; the user must be able to see which CPU (if
    // either) could not be found.
    return cpuLine("MAIN", id.mainPort, id.mainVolume, id.mainSource) + " \xC2\xB7 " +
           cpuLine("DISPLAY", id.displayPort, id.displayVolume, id.displaySource);
}

CpuIdentity withVerifiedVolume(CpuIdentity identity, const VerifiedVolume& verified)
{
    if (!verified.known()) return identity;

    if (verified.cpu == TargetCpu::Main) {
        identity.mainVolume = verified.volume;
        if (!identity.mainPort) identity.mainSource = IdentitySource::VerifiedProbe;
    } else {
        identity.displayVolume = verified.volume;
        if (!identity.displayPort) identity.displaySource = IdentitySource::VerifiedProbe;
    }
    return identity;
}

DeviceModel::DeviceModel() : m_scan(&defaultScan)
{
}

DeviceModel::DeviceModel(ScanFn scan) : m_scan(std::move(scan))
{
}

void DeviceModel::refresh()
{
    m_error.clear();

    // Single call per refresh(): the underlying getDevices() takes a lock
    // every time it is called, and this result is cached in m_devices for
    // the rest of the frame (and until the next refresh()) rather than
    // re-queried.
    auto result = m_scan();
    if (!result) {
        m_devices.clear();
        m_error = result.error();
        return;
    }
    m_devices = std::move(*result);

    // The CPU probe's fact, folded in before anything reads an identity, so
    // there is one identity per device rather than one identity plus a second
    // thing every consumer has to remember to consult. Only for a single
    // connected device -- see setVerifiedVolume()'s comment on why a mapping
    // derived by elimination across ONE board's two CPUs must not be attached
    // to a second board.
    if (m_verified.known() && m_devices.size() == 1)
        m_devices.front().identity = withVerifiedVolume(m_devices.front().identity, m_verified);

    // m_selectedId is otherwise left untouched here -- it is resolved
    // against the new m_devices by selected() on demand. No clamping or
    // resetting needed: a uniqueID that is no longer present simply resolves
    // to "nothing selected", and one that reappears (the same device
    // replugged) resolves correctly again, whatever position it now occupies.
    //
    // Auto-select is the one exception, and it only ever acts on
    // m_selectedId itself (never on whether it currently resolves), so a
    // temporarily-absent explicit selection is never disturbed by it -- see
    // the header comment on refresh() for the precise rules.
    if (m_devices.size() == 1) {
        if (!m_selectedId) {
            m_selectedId = m_devices.front().uniqueID;
            m_selectedSerial = m_devices.front().serial;
            m_selectionIsAuto = true;
        }
    } else if (m_devices.size() >= 2) {
        if (m_selectionIsAuto) {
            m_selectedId.reset();
            m_selectedSerial.clear();
            m_selectionIsAuto = false;
        }
    }

    // FIRST identification, not a CHANGE of identity -- and that distinction is
    // the whole of why this is not a hole in the substitution guard above.
    //
    // The problem it fixes: fwfinder emits the literal "Unknown" as a FreeWili
    // board's serial whenever it cannot find the board's FTDI child device
    // (see serialIsUnidentified()), a TRANSIENT state this hardware really
    // does pass through. If a selection is recorded during that window --
    // auto-select above, or an explicit select() -- m_selectedSerial is
    // latched to a string that identifies nothing. Nothing else in this class
    // ever wrote m_selectedSerial again, so when the real serial arrived a
    // moment later selected() compared "FW6548" against "Unknown", found a
    // mismatch, and refused FOREVER: a healthy board that could only be
    // flashed by restarting the app.
    //
    // Why adopting is safe. The serial comparison in selected() exists to
    // catch BOARD SUBSTITUTION -- uniqueID is topological (see
    // DeviceView::uniqueID), so board B plugged into board A's port comes back
    // with A's uniqueID and the serial is the only thing that can tell them
    // apart. That guard protects a CONFIRMED identity. An unidentified stored
    // serial is not a confirmed identity: it is the absence of one. There is
    // no fact here being overwritten, no earlier answer being contradicted --
    // the app is learning, for the first time, which board is on that port.
    // It is exactly the state the user would reach by clicking the row again,
    // or by restarting the app, so refusing it protects nothing and only
    // strands them.
    //
    // What it does NOT do: once a real serial is pinned here, this branch can
    // never fire again for that selection (the guard is on the STORED serial),
    // so every subsequent serial change is a plain mismatch and stays refused
    // by selected(). Adoption is a one-way door out of "unknown", never a way
    // back into it -- and the device's own serial must be identified too, so a
    // board that keeps saying "Unknown" pins nothing and keeps being refused.
    //
    // Applied to explicit selections as well as auto ones, deliberately: a
    // click made while the row read "Unknown" confirmed no identity either, and
    // the flash path does not care which way the selection was made. Any
    // substitution that happened entirely inside the unidentified window is
    // undetectable from this data whether we adopt or not -- there is nothing
    // to compare against -- and is caught downstream anyway, where FlashDialog
    // re-captures the (now real) serial at open() and selectionUnchangedFresh()
    // requires it to still match, against a snapshot no more than
    // kMaxSnapshotAgeForFlash old, before Proceed does anything.
    if (m_selectedId && serialIsUnidentified(m_selectedSerial)) {
        for (const auto& d : m_devices) {
            if (d.uniqueID == *m_selectedId && !serialIsUnidentified(d.serial)) {
                m_selectedSerial = d.serial;
                break;
            }
        }
    }
}

void DeviceModel::setVerifiedVolume(VerifiedVolume verified)
{
    m_verified = std::move(verified);
}

void DeviceModel::requestRescan()
{
    fwFinderManager::instance().requestRefresh(0);
}

bool DeviceModel::scanning() const
{
    // isActivelyScanning(), NOT isRunning(): since the worker stopped exiting
    // when its request window expires (see fwFinderManager::run()'s comment),
    // isRunning() is true for the app's whole life and would pin the device
    // bar's status to "(scanning...)" forever. What the bar means by scanning
    // is the fast-poll window a request opens.
    return fwFinderManager::instance().isActivelyScanning();
}

std::optional<std::chrono::milliseconds> DeviceModel::snapshotAge() const
{
    return fwFinderManager::instance().lastScanAge();
}

std::optional<DeviceView> DeviceModel::selectedByIdOnly() const
{
    if (!m_selectedId) return std::nullopt;
    for (const auto& d : m_devices) {
        if (d.uniqueID == *m_selectedId) return d; // copy: see selected()'s header comment on why
    }
    return std::nullopt;
}

std::optional<DeviceView> DeviceModel::selected() const
{
    const auto byId = selectedByIdOnly();
    if (!byId) return std::nullopt;
    // uniqueID alone identifies a USB port, not a board (see
    // DeviceView::uniqueID's comment): a different physical device can land
    // on the very port the selected one vacated and come back with the SAME
    // uniqueID. Require the serial recorded at selection time to still
    // match before treating this as a hit -- a serial that carries no
    // identifying information (empty, or fwfinder's own "Unknown" sentinel
    // -- see serialIsUnidentified()) on either side is never a match; a
    // device that will not say what it is does not get silently reported as
    // the still-selected one.
    if (serialIsUnidentified(byId->serial) || serialIsUnidentified(m_selectedSerial) || byId->serial != m_selectedSerial)
        return std::nullopt;
    return byId;
}

void DeviceModel::select(size_t index)
{
    if (index < m_devices.size()) {
        m_selectedId     = m_devices[index].uniqueID;
        m_selectedSerial = m_devices[index].serial;
    } else {
        m_selectedId = std::nullopt;
        m_selectedSerial.clear();
    }
    // Always explicit -- see the header comment on why this matters even
    // when index happens to name the device auto-select would have picked.
    m_selectionIsAuto = false;
}

} // namespace fwog
