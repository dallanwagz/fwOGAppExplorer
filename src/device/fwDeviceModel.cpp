#include "device/fwFinderAvailable.h"
#include "device/fwDeviceModel.h"

#include "device/fwFinderManager.h"

#ifdef FWOG_HAVE_FWFINDER
#include "device/fwDeviceRecords.h"
#include "device/fwCpuIdentify.h"
#endif

#include <utility>

namespace fwog {

bool serialIsUnidentified(std::string_view serial)
{
    return serial.empty() || serial == "Unknown";
}

BoardFingerprint fingerprintOf(std::string_view serial, const CpuIdentity& identity)
{
    BoardFingerprint fp;
    if (!serialIsUnidentified(serial)) fp.serial = std::string(serial);
    fp.mainChip    = identity.mainChipSerial;
    fp.displayChip = identity.displayChipSerial;
    return fp;
}

BoardFingerprint fingerprintOf(const DeviceView& device)
{
    return fingerprintOf(device.serial, device.identity);
}

bool fingerprintsContradict(const BoardFingerprint& a, const BoardFingerprint& b)
{
    const auto differ = [](const std::string& x, const std::string& y) {
        return !x.empty() && !y.empty() && x != y;
    };
    return differ(a.serial, b.serial) || differ(a.mainChip, b.mainChip)
        || differ(a.displayChip, b.displayChip);
}

BoardFingerprint adoptKnown(BoardFingerprint stored, const BoardFingerprint& seen)
{
    if (stored.serial.empty())      stored.serial      = seen.serial;
    if (stored.mainChip.empty())    stored.mainChip    = seen.mainChip;
    if (stored.displayChip.empty()) stored.displayChip = seen.displayChip;
    return stored;
}

std::string describeFingerprint(const BoardFingerprint& fp)
{
    if (!fp.serial.empty())      return "serial " + fp.serial;
    if (!fp.mainChip.empty())    return "MAIN chip " + fp.mainChip;
    if (!fp.displayChip.empty()) return "DISPLAY chip " + fp.displayChip;
    return "unidentified";
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
#ifdef FWOG_HAVE_FWFINDER
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
        // An auto-selection whose one board now CONTRADICTS what was recorded
        // is dropped here and re-made just below. Only a swap can produce a
        // contradiction, and with a single board there is no choice to
        // protect: the board in front of the user is the one they want. This
        // is also what stops an identifier that turns out not to be as stable
        // as believed from stranding the app the way a latched "Unknown"
        // serial once did (see the adoption comment below) -- a flash in
        // progress is guarded by FlashDialog's own gate, not by this.
        if (m_selectedId && m_selectionIsAuto
            && m_devices.front().uniqueID == *m_selectedId
            && fingerprintsContradict(fingerprintOf(m_devices.front()), m_selectedPrint)) {
            m_selectedId.reset();
            m_selectedPrint = {};
        }
        if (!m_selectedId) {
            m_selectedId = m_devices.front().uniqueID;
            m_selectedPrint = fingerprintOf(m_devices.front());
            m_selectionIsAuto = true;
        }
    } else if (m_devices.size() >= 2) {
        if (m_selectionIsAuto) {
            m_selectedId.reset();
            m_selectedPrint = {};
            m_selectionIsAuto = false;
        }
    }

    // LEARNING, not a CHANGE of identity -- and that distinction is the whole
    // of why this is not a hole in the substitution guard above.
    //
    // A FreeWili OG's FTDI serial is missing whenever the board runs OG
    // firmware, and both chip ids are missing while both CPUs sit in the
    // bootrom, so a selection is routinely recorded with a fingerprint that
    // knows nothing, or only part of what it could. Nothing else in this class
    // ever writes m_selectedPrint again, so without this a board that later
    // reveals an identifier could never be held to it -- and, in the older
    // serial-only version of this code, a fingerprint latched to a transient
    // "Unknown" was compared against the real serial forever after and refused
    // for the life of the process.
    //
    // adoptKnown() fills only EMPTY fields. It never overwrites a known one, so
    // an identifier once pinned can only be contradicted, never replaced --
    // that is what keeps the check meaningful. Applied to explicit selections
    // as well as auto ones: a click made while the board said nothing about
    // itself confirmed no identity either.
    if (m_selectedId) {
        for (const auto& d : m_devices) {
            if (d.uniqueID == *m_selectedId) {
                m_selectedPrint = adoptKnown(std::move(m_selectedPrint), fingerprintOf(d));
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
    // uniqueID. Refuse when the device there now positively contradicts the
    // fingerprint recorded at selection time. A board that reveals nothing
    // about itself is not refused -- see BoardFingerprint for why that is the
    // right answer and not a weakening.
    if (fingerprintsContradict(fingerprintOf(*byId), m_selectedPrint))
        return std::nullopt;
    return byId;
}

void DeviceModel::select(size_t index)
{
    if (index < m_devices.size()) {
        m_selectedId    = m_devices[index].uniqueID;
        m_selectedPrint = fingerprintOf(m_devices[index]);
    } else {
        m_selectedId = std::nullopt;
        m_selectedPrint = {};
    }
    // Always explicit -- see the header comment on why this matters even
    // when index happens to name the device auto-select would have picked.
    m_selectionIsAuto = false;
}

} // namespace fwog
