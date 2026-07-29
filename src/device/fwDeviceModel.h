#pragma once

#include "core/fwTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fwog {

struct DeviceView {
    std::string name;
    /// The board's own USB serial string -- identifies the physical unit,
    /// unlike `uniqueID` below (which identifies a port). Comparisons that
    /// care whether this is still the SAME board must check both. Can be a
    /// non-identifying placeholder rather than empty -- see
    /// serialIsUnidentified() below before comparing this for equality.
    std::string serial;
    CpuIdentity identity;
    /// True for DeviceType::FreeWili (the OG / Classic). False for a FreeWili 2,
    /// a badge, or a bare UF2 device -- shown, but not flashable by this app.
    bool isOg = false;
    /// fwfinder's Fw::FreeWiliDevice::uniqueID. Despite the name, this is
    /// TOPOLOGICAL, not a device identity: fwfinder builds it purely from the
    /// USB port chain (`_generateUniqueIDFromUSBPortChain`, fwfinder.cpp),
    /// packing each port index into a bitfield with nothing else mixed in.
    /// It identifies a SOCKET, not a board -- unplug board A from a port and
    /// plug board B into that same port, and `uniqueID` comes back identical
    /// for B. DeviceModel's selection is keyed on this (not on list
    /// position: see select()/selected() below for why), which resolves the
    /// list-position hazard but NOT the socket-vs-board one -- any comparison
    /// that needs to tell "the same physical device" apart from "a different
    /// device that landed in the same slot" must also compare `serial`
    /// below, never `uniqueID` alone. See selectionUnchanged()
    /// (fwFlashController.h) for the flash-time version of this check.
    uint64_t uniqueID = 0;
};

/// True when `serial` carries no identifying information: empty, or
/// fwfinder's own sentinel literal "Unknown". fwfinder emits "Unknown"
/// (_deps/fwfinder-src/src/fwfinder.cpp, the Fw::DeviceType::FreeWili branch
/// that runs when no FTDI child USB device is found) rather than leaving
/// `serial` empty -- a non-empty string that nonetheless identifies nothing,
/// and it has been observed on real hardware in a degraded identification
/// state, not just a theoretical case. Comparing two "Unknown" serials as
/// equal would let a DIFFERENT board that landed in the same USB port (same
/// topological `uniqueID` -- see DeviceView::uniqueID) pass as unchanged,
/// since nothing else would catch it. Used by both DeviceModel::selected()
/// and fwFlashController.h's selectionUnchanged() so both layers apply
/// exactly the same rule; deliberately not case- or whitespace-insensitive
/// -- it matches what fwfinder actually emits today, not a guess at what it
/// might emit.
bool serialIsUnidentified(std::string_view serial);

/// One human-readable line describing what was identified and how. Never omits
/// a CPU: silence would read as "fine". The identification sources are worded
/// differently on purpose -- hub position is structural and authoritative, a
/// USB product string is a fallback, and a CPU probe is a measurement of the
/// silicon that named a DRIVE rather than a port -- so the user can tell which
/// one is in play. Examples:
///
///   MAIN: COM60 (by hub position) · DISPLAY: COM65 (by hub position)
///   MAIN: COM60 (by USB product name) · DISPLAY: not identified
///   MAIN: G:/ (by CPU probe) · DISPLAY: COM69 (by hub position)
///   MAIN: not identified · DISPLAY: not identified
///
/// A CPU with both a port and a probed volume renders the PORT: that is the
/// thing the flash engine would act on for a CPU that is running, and showing
/// the drive letter instead would describe a state the board is not in.
std::string describeIdentity(const CpuIdentity& id);

/// Fold a verified CPU-probe result into `identity` as a first-class identity
/// source, and return the result. PURE -- the argument is taken by value and
/// the caller's copy is never mutated.
///
/// `verified` names one mounted RPI-RP2 volume and the CPU a probe MEASURED it
/// to be. A default-constructed (`known() == false`) value returns `identity`
/// untouched, which is the case every caller hits until an identification has
/// actually run and while one is stale -- so "no mapping" costs nothing and
/// looks like nothing.
///
/// The volume field is filled in unconditionally, because that is the fact.
/// `mainSource`/`displaySource` are only overwritten when that CPU has NO port,
/// so the source always describes what describeIdentity() actually renders (see
/// its comment: a port wins the line). A CPU that has both is not a state this
/// board reaches -- a CPU in the bootrom publishes no port, and identifyCpus()
/// refuses outright on that contradiction (fwCpuProbe.cpp step 6) -- but the
/// rule is stated here rather than assumed, because "cannot happen" is not a
/// thing to leave a display inconsistent over.
///
/// It does NOT check freshness. That is verifiedVolumeFrom()'s job
/// (fwCpuProbe.h): freshness is a property of the moment a fact is produced,
/// not of the act of copying it into a struct, and splitting the two is what
/// keeps this a pure function that a test can drive without a clock.
CpuIdentity withVerifiedVolume(CpuIdentity identity, const VerifiedVolume& verified);

/// Owns the device list and the selection. Polls fwFinderManager once per
/// refresh() call and caches the result; the UI never calls fwfinder
/// directly and must not call getDevices() itself.
class DeviceModel {
public:
    /// The scan operation refresh() performs: a snapshot of the connected
    /// devices, or an error string on scan failure. Production's default
    /// (see the DeviceModel() constructor) queries
    /// fwFinderManager::instance().getDevices(false) and flattens each
    /// Fw::FreeWiliDevice into a DeviceView via toCpuPortRecords()/
    /// identifyCpus(). Tests inject a fake here so the identity-based
    /// selection logic below can be exercised without real hardware or
    /// fwfinder -- matching the FetchFn injection pattern RemoteCatalog
    /// uses for the same reason (see fwCatalogRemote.h).
    using ScanFn = std::function<std::expected<std::vector<DeviceView>, std::string>()>;

    /// Production use: scans via fwFinderManager.
    DeviceModel();

    /// Test use: scans via the given function instead.
    explicit DeviceModel(ScanFn scan);

    /// Records the one fact the Recovery tab's CPU probe can contribute: this
    /// mounted RPI-RP2 volume is that CPU. refresh() folds it into the scanned
    /// devices' CpuIdentity via withVerifiedVolume(), so every consumer of an
    /// identity -- the device bar, the disabled-reason, the flash engine --
    /// sees one identity with all of the evidence in it rather than each
    /// having to remember to go and ask a second source.
    ///
    /// Pass a default-constructed VerifiedVolume to clear it, and DO pass one
    /// the moment the identification stops applying: this model has no clock
    /// and does not watch the volumes, so it cannot expire anything on its own.
    /// The caller owns the freshness rule (verifiedVolumeFrom(), fwCpuProbe.h)
    /// and is expected to re-supply the answer every frame, which is exactly
    /// what fwApp.cpp's main loop does.
    ///
    /// Applied only when EXACTLY ONE device is connected. The identification is
    /// derived by elimination across one board's two CPUs (see
    /// identifyDisabledReason(), fwTabRecovery.cpp, which refuses to start with
    /// more than one board attached for that reason), so attaching it to a
    /// second board's identity would be attaching it to a board it says nothing
    /// about.
    void setVerifiedVolume(VerifiedVolume verified);

    /// Pulls the latest scan result, flattens it into devices(), and leaves
    /// the current selection (see select()) alone -- it is resolved against
    /// the new list by identity, not reset. On the web build fwfinder does
    /// not exist and this always yields an empty list.
    ///
    /// Also applies auto-select: with identity-based selection (see
    /// select() below) there is no more implicit "index 0" default, so with
    /// nothing selected yet and exactly one device connected, that single
    /// device is selected automatically -- friction-free for the common
    /// case of one board plugged in, with no safety cost since there is
    /// only one candidate. Precisely:
    ///   - exactly one device and no selection recorded at all -> selected;
    ///   - two or more devices -> never auto-selected, even with nothing
    ///     recorded -- this app must never pick between candidate boards;
    ///   - an auto-selection, if a second device then appears, is CLEARED
    ///     (not left dangling) so the user makes a deliberate choice rather
    ///     than inheriting one;
    ///   - a selection the user made explicitly via select() is never
    ///     touched by any of the above, whether or not its device is
    ///     currently present in devices() -- see select()'s comment on why
    ///     a temporarily-absent explicit selection is preserved rather than
    ///     cleared.
    ///
    /// Finally, it adopts a FIRST identifying serial onto a selection that
    /// never had one: when the recorded selection serial is unidentified (see
    /// serialIsUnidentified()) and the device it points at now reports a real
    /// one, that real one is recorded. This is what stops a selection made
    /// while fwfinder was transiently reporting "Unknown" from being stuck
    /// unresolvable in selected() forever. It is NOT a relaxation of the
    /// substitution guard -- see the long comment at the adoption site in
    /// fwDeviceModel.cpp before touching it.
    void refresh();

    /// Asks the background scanner to look again. Non-blocking; the new
    /// result shows up on a later refresh() once the scan completes.
    void requestRescan();

    /// True while fwFinderManager is in its fast-poll window -- i.e. a
    /// requestRescan() (startup, the Rescan button, or an open flash dialog)
    /// happened recently enough that the scanner is actively looking rather
    /// than merely watching. Routed through here (rather than the UI calling
    /// fwFinderManager itself) so DeviceModel stays the single point of
    /// contact with fwfinder. NOT "a worker thread exists": that is true for
    /// the app's whole lifetime now, and would make the status line a
    /// constant.
    bool scanning() const;

    /// How long ago the device snapshot behind devices() was actually taken,
    /// or nullopt if no scan has ever succeeded. Distinct from "how long ago
    /// refresh() ran": refresh() re-reads the SAME cached scan result every
    /// frame, so its own recency says nothing at all about the data's.
    ///
    /// Exists for one caller: FlashDialog's identity gate, which must not
    /// report "still the same board" on the strength of a snapshot that
    /// predates the user swapping boards. nullopt is the fail-closed answer
    /// (including on the web build, where nothing is ever scanned) -- see
    /// selectionUnchangedFresh() in fwFlashController.h.
    std::optional<std::chrono::milliseconds> snapshotAge() const;

    const std::vector<DeviceView>& devices() const { return m_devices; }

    /// A snapshot of the currently selected device, or nullopt when there is
    /// none. Returned BY VALUE, not by pointer: refresh() replaces the
    /// backing vector every call (once per frame, even with nothing
    /// hotplugged), so a pointer or reference into it is only valid for the
    /// frame it was obtained in -- a trap for a caller that holds onto it
    /// across frames, such as a flash-confirmation modal. A DeviceView is
    /// two strings, a CpuIdentity and two scalars: the copy costs nothing
    /// that matters at this scale.
    ///
    /// Resolves the recorded uniqueID against devices() AND requires its
    /// serial to still match what was recorded at select()/auto-select time
    /// (see select()'s comment on why uniqueID alone is not enough); nullopt
    /// if either check fails, including when the matching-uniqueID device's
    /// serial is unidentified (see serialIsUnidentified()) on either side.
    ///
    /// "What was recorded at select()/auto-select time" is subject to
    /// refresh()'s adoption of a FIRST identifying serial onto a selection
    /// that had none -- so a selection made while the board was reporting
    /// "Unknown" resolves normally once the board says who it is, rather than
    /// staying unresolvable for the life of the process.
    std::optional<DeviceView> selected() const;

    /// The device selectedByIdOnly() resolves purely by uniqueID -- WITHOUT
    /// the serial check selected() adds. Exists for a caller that needs to
    /// know WHY there was no match from selected(), not just that there
    /// wasn't one: selected() collapses "nothing recorded", "uniqueID no
    /// longer present" and "uniqueID present under a different/unidentified
    /// serial" all down to nullopt, which is correct for ordinary display
    /// use (the device bar) but throws away information two other callers
    /// need:
    ///   - FlashDialog applies its OWN, equally strict identity comparison
    ///     to word a refusal specifically (see fwFlashController.h's
    ///     SelectionCheck / selectionUnchanged()). Safe ONLY because
    ///     selectionUnchanged() re-applies its own uniqueID-and-serial check
    ///     on the result before anything is allowed to proceed.
    ///   - flashDisabledReason() (fwCatalogFilter.h, Task 22) uses it to
    ///     word a disabled Flash button accurately when a board's serial
    ///     reads fwfinder's "Unknown" sentinel (see serialIsUnidentified()):
    ///     "No FreeWili is connected" would be false when the device bar is
    ///     still showing that exact board's row. Safe ONLY because the
    ///     caller's actual flash click still gates on selected() (the
    ///     confirmed device), never on this raw one -- see
    ///     DefaultFirmwareTab's/AppExplorerTab's drawFlashButton()-equivalent
    ///     functions.
    /// Neither caller may use this method's result to decide whether to
    /// flash on its own; both re-apply their own confirmation before acting.
    std::optional<DeviceView> selectedByIdOnly() const;

    /// Selects the device currently at `index` within devices() -- but what
    /// gets remembered is that device's uniqueID AND serial, not `index`
    /// itself. An out-of-range index clears the selection.
    ///
    /// This distinction matters across a refresh(): if the selected device
    /// is unplugged, the list shrinks and selected() correctly reports
    /// nothing selected. If a *different* device is then plugged in and the
    /// list regrows to the same size, a numeric index would silently
    /// reattach the selection to that unrelated device merely because it
    /// landed on the same row -- exactly the hazard this app exists to
    /// avoid. Keying on uniqueID moves that hazard from "any row" down to
    /// "the exact same USB port", but does not close it: uniqueID is
    /// TOPOLOGICAL, not a device identity (see DeviceView::uniqueID's
    /// comment) -- plug a different board into the very port the selected
    /// one just vacated, and uniqueID alone would resolve the selection onto
    /// that new board, not onto "nothing selected". selected() therefore
    /// also compares `serial`, recorded here alongside uniqueID, before
    /// reporting a match; an empty serial on either side is never treated as
    /// a match. This is what actually makes the selection resolve back onto
    /// the same physical device and never onto whatever happens to occupy
    /// its old slot.
    ///
    /// Always records an EXPLICIT selection, even when it happens to name
    /// the same device refresh()'s auto-select would have chosen anyway --
    /// this is what protects it from the "auto-selection is cleared when a
    /// second device appears" rule above, which only ever clears a
    /// selection refresh() made on its own.
    void select(size_t index);

    /// Non-empty when the last refresh()'s scan failed. A scan failure is
    /// information for the user, not something to hide.
    const std::string& error() const { return m_error; }

private:
    ScanFn m_scan;
    std::vector<DeviceView> m_devices;
    std::optional<uint64_t> m_selectedId;
    /// The serial recorded alongside m_selectedId at select()/auto-select
    /// time -- selected() requires this to still match before reporting a
    /// hit, so a different board landing on the same USB port (same
    /// uniqueID) is never silently reported as the still-selected device.
    /// See select()'s comment.
    ///
    /// Written in exactly three places: select(), refresh()'s auto-select, and
    /// refresh()'s adoption of a first identifying serial onto a selection
    /// recorded while the board was reporting fwfinder's "Unknown" sentinel.
    /// That last one only ever replaces an UNIDENTIFIED value, so a real
    /// serial, once pinned, is never overwritten by anything short of a new
    /// select() -- which is what keeps the substitution check meaningful.
    std::string m_selectedSerial;
    /// True when m_selectedId was set by refresh()'s auto-select rather than
    /// by an explicit select() call. Distinguishing the two is what lets
    /// refresh() clear an auto-selection when a second device shows up while
    /// leaving an explicit one alone -- see refresh()'s and select()'s
    /// comments above.
    bool m_selectionIsAuto = false;
    /// The latest verified probe fact, re-supplied by the caller every frame --
    /// see setVerifiedVolume(). Default-constructed means "none", which is what
    /// this is for the whole life of a session in which nobody runs the
    /// Recovery tab's identification.
    VerifiedVolume m_verified;
    std::string m_error;
};

} // namespace fwog
