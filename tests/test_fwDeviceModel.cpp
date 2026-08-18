#include <doctest/doctest.h>
#include "device/fwDeviceModel.h"

#include <expected>
#include <string>
#include <vector>

using namespace fwog;

namespace {
// `serial` defaults to `name` so every existing call site (each naming a
// distinct device -- "A", "B", "C" -- across the file) keeps a distinct,
// non-empty serial for free, and re-scanning the SAME `device("B", 2)` call
// still matches on serial as well as uniqueID. Tests that need to exercise
// the serial check directly (a different board landing on the same port, or
// an empty serial) pass it explicitly via the 3-argument overload.
DeviceView device(std::string name, uint64_t uniqueID, std::string serial)
{
    DeviceView v;
    v.uniqueID = uniqueID;
    v.serial   = std::move(serial);
    v.name     = std::move(name);
    return v;
}

DeviceView device(std::string name, uint64_t uniqueID)
{
    std::string serial = name;
    return device(std::move(name), uniqueID, std::move(serial));
}
} // namespace

TEST_CASE("both CPUs identified by hub location reads clearly") {
    CpuIdentity id;
    id.mainPort = "COM60";    id.mainSource    = IdentitySource::HubLocation;
    id.displayPort = "COM65"; id.displaySource = IdentitySource::HubLocation;

    auto s = describeIdentity(id);
    CHECK(s.find("COM60") != std::string::npos);
    CHECK(s.find("COM65") != std::string::npos);
    CHECK(s.find("hub") != std::string::npos);
}

TEST_CASE("an unidentified CPU says so rather than being omitted") {
    // Silence would read as "fine". The user must be able to see that the
    // display CPU is the one that could not be found.
    CpuIdentity id;
    id.mainPort = "COM60"; id.mainSource = IdentitySource::HubLocation;

    auto s = describeIdentity(id);
    CHECK(s.find("DISPLAY") != std::string::npos);
    CHECK(s.find("not identified") != std::string::npos);
}

TEST_CASE("product-string identification is labelled differently from hub location") {
    CpuIdentity byHub, byString;
    byHub.mainPort = "COM60";    byHub.mainSource    = IdentitySource::HubLocation;
    byString.mainPort = "COM60"; byString.mainSource = IdentitySource::ProductString;
    CHECK(describeIdentity(byHub) != describeIdentity(byString));
}

TEST_CASE("DISPLAY identified and MAIN missing also says so -- the reciprocal of the MAIN case above") {
    CpuIdentity id;
    id.displayPort = "COM65"; id.displaySource = IdentitySource::HubLocation;

    auto s = describeIdentity(id);
    CHECK(s.find("MAIN") != std::string::npos);
    CHECK(s.find("not identified") != std::string::npos);
    CHECK(s.find("COM65") != std::string::npos);
}

TEST_CASE("a product-string-sourced DISPLAY is labelled by product name, not hub position") {
    CpuIdentity id;
    id.displayPort = "COM65"; id.displaySource = IdentitySource::ProductString;

    auto s = describeIdentity(id);
    CHECK(s.find("COM65") != std::string::npos);
    CHECK(s.find("by USB product name") != std::string::npos);
    CHECK(s.find("by hub position") == std::string::npos);
}

TEST_CASE("nothing identified at all still produces a readable line") {
    auto s = describeIdentity(CpuIdentity{});
    CHECK_FALSE(s.empty());
    CHECK(s.find("MAIN") != std::string::npos);
    CHECK(s.find("DISPLAY") != std::string::npos);
}

TEST_CASE("selection clamps to the device list") {
    // selected() returns std::optional<DeviceView> by value (fix round,
    // Finding 2): a pointer/reference into m_devices would dangle after the
    // very next refresh(), which replaces the backing vector every call.
    DeviceModel m;
    CHECK_FALSE(m.selected().has_value());   // nothing connected
    m.select(5);                             // out of range must not crash
    CHECK_FALSE(m.selected().has_value());
}

// --- Fix round, Finding 1: selection must be keyed on device identity
// (Fw::FreeWiliDevice::uniqueID), not on list position -- otherwise an
// index left over from a device that was unplugged can silently reattach to
// an unrelated device that happens to land on the same row later. These
// three tests inject a fake ScanFn (see DeviceModel::ScanFn in
// fwDeviceModel.h) so the list can be rewritten between refresh() calls
// without real hardware.

TEST_CASE("selecting a device then unplugging it leaves nothing selected") {
    std::vector<DeviceView> scan{ device("A", 1), device("B", 2) };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();

    m.select(1); // B
    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->uniqueID == 2);

    scan = { device("A", 1) }; // B unplugged; list shrinks
    m.refresh();
    CHECK_FALSE(m.selected().has_value());
}

TEST_CASE("a different device landing on the vacated row is never silently selected") {
    std::vector<DeviceView> scan{ device("A", 1), device("B", 2) };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();
    m.select(1); // B, at index 1

    scan = { device("A", 1) }; // B unplugged
    m.refresh();

    // A DIFFERENT device (uniqueID 3) is plugged in, regrowing the list back
    // to two entries -- so it lands at index 1, the row the stale selection
    // used to point at. Selecting by index here would resurrect the
    // selection onto this unrelated device.
    scan = { device("A", 1), device("C", 3) };
    m.refresh();
    CHECK_FALSE(m.selected().has_value());
}

TEST_CASE("re-plugging the originally selected device reselects it") {
    std::vector<DeviceView> scan{ device("A", 1), device("B", 2) };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();
    m.select(1); // B

    scan = { device("A", 1) }; // B unplugged
    m.refresh();
    scan = { device("A", 1), device("B", 2) }; // the SAME device (uniqueID 2) plugged back in
    m.refresh();

    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->uniqueID == 2);
}

// --- Controller addition: auto-select if and only if exactly one device is
// connected. Task 16 moved selection to identity-based tracking, which
// removed the old implicit "index 0" default -- with nothing selected by
// default, the Flash button would stay permanently disabled until the user
// clicked a device even with only one board plugged in. These four tests
// exercise the transitions the controller called out explicitly.

TEST_CASE("exactly one device and no prior selection auto-selects it") {
    std::vector<DeviceView> scan{ device("A", 1) };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();

    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->uniqueID == 1);
}

TEST_CASE("two or more devices never auto-select -- the user must choose") {
    std::vector<DeviceView> scan{ device("A", 1), device("B", 2) };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();

    // This app must never pick between candidate boards, even though
    // nothing has been explicitly selected.
    CHECK_FALSE(m.selected().has_value());
}

TEST_CASE("an auto-selection is cleared the moment a second device appears") {
    std::vector<DeviceView> scan{ device("A", 1) };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();
    REQUIRE(m.selected().has_value());   // auto-selected A
    CHECK(m.selected()->uniqueID == 1);

    scan = { device("A", 1), device("B", 2) }; // a second board is plugged in
    m.refresh();

    // The auto-selection must not survive into a two-device state -- the
    // user makes a deliberate choice now, rather than inheriting one.
    CHECK_FALSE(m.selected().has_value());
}

TEST_CASE("an explicit selection is never disturbed by auto-select bookkeeping") {
    std::vector<DeviceView> scan{ device("A", 1), device("B", 2) };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();
    m.select(0); // user explicitly picks A while two devices are present
    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->uniqueID == 1);

    // The list narrows to exactly one device -- the same shape that would
    // trigger auto-select from a blank slate -- but A was already chosen
    // explicitly, so nothing about this refresh should reclassify it.
    scan = { device("A", 1) };
    m.refresh();
    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->uniqueID == 1);

    // A second device reappears. If A's selection had been (mis)treated as
    // auto, this is exactly the transition that would clear it; it must not.
    scan = { device("A", 1), device("C", 3) };
    m.refresh();
    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->uniqueID == 1);
}

// --- Fix round: DeviceModel's own selection was keyed on uniqueID alone,
// which is fwfinder's topological port-chain ID (see DeviceView::uniqueID's
// header comment) -- a SOCKET identity, not a board identity. Plug a
// DIFFERENT board into the exact port the selected one just vacated, and the
// old uniqueID-only selected() would silently resolve the selection onto
// the new board: the selection follows the socket, not the board, and
// nothing in this app noticed. select()/selected() now also record and
// compare `serial`; these tests are the regression coverage for that.

TEST_CASE("a different physical board landing on the vacated PORT is never silently kept selected")
{
    // Distinct from "a different device landing on the vacated row" above --
    // that test used a DIFFERENT uniqueID (a different port) and was already
    // caught by uniqueID-only comparison. This is the case that was not: the
    // SAME uniqueID (same port), a DIFFERENT board.
    std::vector<DeviceView> scan{ device("A", 1, "SN-AAA") };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();
    m.select(0);
    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->serial == "SN-AAA");

    // Board A is unplugged and a DIFFERENT board is plugged into the SAME
    // USB port -- fwfinder's uniqueID is location-based, so it comes back as
    // uniqueID 1 again even though this is a different physical unit.
    scan = { device("A-slot", 1, "SN-ZZZ") };
    m.refresh();

    CHECK_FALSE(m.selected().has_value());
}

TEST_CASE("the same board replugged into the same port is still recognised via serial")
{
    // The non-regression companion to the test above: uniqueID AND serial
    // both matching (the ordinary "still the same board" case) must keep
    // working, not just the mismatch case.
    std::vector<DeviceView> scan{ device("A", 1, "SN-AAA") };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();
    m.select(0);

    scan = { device("A", 1, "SN-AAA") }; // same uniqueID, same serial
    m.refresh();

    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->serial == "SN-AAA");
}

TEST_CASE("a device with no identifiers at all is still selected -- there is nothing to contradict")
{
    // The rule is CONTRADICTION, not confirmation. A board that says nothing
    // about itself (no FTDI serial, both CPUs in the bootrom) cannot be shown
    // to be a different board, and refusing it refused every FreeWili OG in
    // the states it most needs flashing in. See BoardFingerprint.
    std::vector<DeviceView> scan{ device("A", 1, "") };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();
    m.select(0);

    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->uniqueID == 1);
}

// --- BoardFingerprint: the pure comparison DeviceModel and the flash gate
// share. Pinned here because both layers depend on exactly these answers.

TEST_CASE("serialIsUnidentified recognises both empty and fwfinder's \"Unknown\" sentinel")
{
    CHECK(serialIsUnidentified(""));
    CHECK(serialIsUnidentified("Unknown"));
    CHECK_FALSE(serialIsUnidentified("FW4788"));
    // Deliberately NOT case/whitespace insensitive -- matches only what
    // fwfinder actually emits today.
    CHECK_FALSE(serialIsUnidentified("unknown"));
    CHECK_FALSE(serialIsUnidentified(" Unknown"));
}

TEST_CASE("fingerprintOf normalises \"Unknown\" to empty and carries both chip ids")
{
    CpuIdentity id;
    id.mainPort = "COM10";  id.mainChipSerial    = "AAAA";
    id.displayPort = "COM11"; id.displayChipSerial = "BBBB";
    const BoardFingerprint fp = fingerprintOf("Unknown", id);
    CHECK(fp.serial.empty());
    CHECK(fp.mainChip == "AAAA");
    CHECK(fp.displayChip == "BBBB");
    CHECK(fp.identified());
    CHECK(fingerprintOf("FW4788", CpuIdentity{}).serial == "FW4788");
    CHECK_FALSE(fingerprintOf("", CpuIdentity{}).identified());
    CHECK_FALSE(fingerprintOf("Unknown", CpuIdentity{}).identified());
}

TEST_CASE("fingerprintsContradict fires only on an identifier both sides know and disagree on")
{
    const BoardFingerprint a{ "FW1", "AAAA", "BBBB" };
    CHECK_FALSE(fingerprintsContradict(a, a));
    CHECK_FALSE(fingerprintsContradict(a, BoardFingerprint{}));            // nothing known -> nothing differs
    CHECK_FALSE(fingerprintsContradict(BoardFingerprint{}, BoardFingerprint{}));
    CHECK_FALSE(fingerprintsContradict(a, BoardFingerprint{ "", "AAAA", "" }));  // partial agreement
    CHECK_FALSE(fingerprintsContradict(a, BoardFingerprint{ "FW1", "", "" }));
    CHECK(fingerprintsContradict(a, BoardFingerprint{ "FW2", "", "" }));         // serial differs
    CHECK(fingerprintsContradict(a, BoardFingerprint{ "", "XXXX", "" }));        // main chip differs
    CHECK(fingerprintsContradict(a, BoardFingerprint{ "", "", "YYYY" }));        // display chip differs
    CHECK(fingerprintsContradict(a, BoardFingerprint{ "FW1", "AAAA", "YYYY" })); // one field is enough
    // Symmetric.
    CHECK(fingerprintsContradict(BoardFingerprint{ "FW2", "", "" }, a));
}

TEST_CASE("adoptKnown fills only the empty fields and never overwrites a known one")
{
    const BoardFingerprint stored{ "", "AAAA", "" };
    const BoardFingerprint seen{ "FW1", "ZZZZ", "BBBB" };
    const BoardFingerprint out = adoptKnown(stored, seen);
    CHECK(out.serial == "FW1");
    CHECK(out.mainChip == "AAAA");     // NOT ZZZZ: known stays known
    CHECK(out.displayChip == "BBBB");
    CHECK(adoptKnown(BoardFingerprint{}, BoardFingerprint{}) == BoardFingerprint{});
}

TEST_CASE("describeFingerprint prefers the FTDI serial, then a chip id, and never says \"Unknown\"")
{
    CHECK(describeFingerprint(BoardFingerprint{ "FW4788", "AAAA", "BBBB" }) == "serial FW4788");
    CHECK(describeFingerprint(BoardFingerprint{ "", "AAAA", "BBBB" }) == "MAIN chip AAAA");
    CHECK(describeFingerprint(BoardFingerprint{ "", "", "BBBB" }) == "DISPLAY chip BBBB");
    CHECK(describeFingerprint(BoardFingerprint{}) == "unidentified");
}

// --- The FTDI serial. On a FreeWili OG it is read from a separate FTDI chip
// that does not enumerate at all under OG firmware, so fwfinder reports the
// literal "Unknown" for the board's whole working life. That used to refuse
// every such board; now it is simply an identifier the board does not offer.

TEST_CASE("a board reporting fwfinder's \"Unknown\" serial is selectable, and told apart by chip id when one is known")
{
    std::vector<DeviceView> scan{ device("A", 1, "Unknown") };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();
    m.select(0);
    REQUIRE(m.selected().has_value());            // an OG board under OG firmware, every day

    // Board A's MAIN comes up and reports its chip id: learned, not refused.
    DeviceView aRunning = device("A", 1, "Unknown");
    aRunning.identity.mainPort = "COM10";
    aRunning.identity.mainChipSerial = "AAAA";
    scan = { aRunning };
    m.refresh();
    REQUIRE(m.selected().has_value());

    // A DIFFERENT board on the same port, also "Unknown", but a different
    // chip: contradicted, refused. (An explicit selection -- select(0) above --
    // is never re-made on its own; that is the user's call.)
    DeviceView bRunning = device("B", 1, "Unknown");
    bRunning.identity.mainPort = "COM10";
    bRunning.identity.mainChipSerial = "BBBB";
    scan = { bRunning };
    m.refresh();
    CHECK_FALSE(m.selected().has_value());
    REQUIRE(m.selectedByIdOnly().has_value());    // still visible for wording
}

// --- Adoption. A selection recorded while the board said nothing about
// itself must be able to LEARN who it is, or a transient "Unknown" would latch
// a fingerprint that identifies nothing and every later real serial would
// look like a mismatch -- the bug the serial-only predecessor of this code
// had. adoptKnown() fills only empty fields, so this is learning, not
// re-identification, and a real value once pinned can only be contradicted.

TEST_CASE("a selection auto-made while the serial read \"Unknown\" learns the real serial and is then held to it")
{
    std::vector<DeviceView> scan{ device("FreeWili", 1, "Unknown") };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });

    m.refresh();                                  // auto-selects with an empty fingerprint
    REQUIRE(m.selected().has_value());            // usable straight away

    scan = { device("FreeWili", 1, "FW6548") };   // fwfinder finds the FTDI child
    m.refresh();
    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->serial == "FW6548");
    CHECK(m.selected()->uniqueID == 1);
}

TEST_CASE("the same adoption happens from an EMPTY serial, not just fwfinder's \"Unknown\"")
{
    std::vector<DeviceView> scan{ device("FreeWili", 1, "") };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });

    m.refresh();
    REQUIRE(m.selected().has_value());

    scan = { device("FreeWili", 1, "FW6548") };
    m.refresh();

    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->serial == "FW6548");
}

TEST_CASE("adoption applies to an EXPLICIT selection too, and pins it against a later substitution")
{
    // Two devices present, so nothing here could have been auto-selected.
    std::vector<DeviceView> scan{ device("A", 1, "Unknown"), device("B", 2, "SN-BBB") };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();
    m.select(0);                                  // explicitly picks the unidentified one
    REQUIRE(m.selected().has_value());

    scan = { device("A", 1, "SN-AAA"), device("B", 2, "SN-BBB") };
    m.refresh();
    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->uniqueID == 1);
    CHECK(m.selected()->serial == "SN-AAA");

    // A DIFFERENT board on the SAME topological uniqueID -- the exact hazard
    // the fingerprint comparison exists for. Explicit selection: refused, and
    // it stays refused; it does not recover by cycling through "Unknown"
    // either, because the pinned serial is never overwritten short of a new
    // select().
    scan = { device("C", 1, "SN-CCC"), device("B", 2, "SN-BBB") };
    m.refresh();
    CHECK_FALSE(m.selected().has_value());
    scan = { device("C", 1, "Unknown"), device("B", 2, "SN-BBB") };
    m.refresh();
    REQUIRE(m.selected().has_value());            // says nothing -> not contradicted (see BoardFingerprint)
    scan = { device("C", 1, "SN-CCC"), device("B", 2, "SN-BBB") };
    m.refresh();
    CHECK_FALSE(m.selected().has_value());        // ...but the moment it speaks, refused again
}

TEST_CASE("an AUTO-selection follows a swapped-in single board instead of stranding the app")
{
    // With ONE board connected there is no choice to protect: a contradiction
    // can only mean the board was swapped, and the new board is the one the
    // user is looking at. Refusing forever here helped nobody -- a flash in
    // progress is guarded by FlashDialog's own gate, not by this.
    std::vector<DeviceView> scan{ device("A", 1, "FW1111") };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();
    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->serial == "FW1111");

    scan = { device("B", 1, "FW2222") };
    m.refresh();
    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->serial == "FW2222");     // re-selected: the only board there is
}

TEST_CASE("a pinned real serial going unidentified is not a refusal -- the mirror case")
{
    // A board part-way through re-enumeration briefly reports "Unknown", and a
    // MAIN CPU dropping into BOOTSEL takes its chip id with it. Neither is
    // evidence of a different board, and the flash that caused the second one
    // must not be refused by it.
    std::vector<DeviceView> scan{ device("A", 1, "FW6548") };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();
    REQUIRE(m.selected().has_value());

    scan = { device("A", 1, "Unknown") };
    m.refresh();
    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->serial == "Unknown");

    scan = { device("A", 1, "FW6548") };
    m.refresh();
    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->serial == "FW6548");
}

TEST_CASE("a second device still clears an auto-selection that was made while the serial read \"Unknown\"")
{
    // Adoption must not resurrect a selection the two-device rule has cleared:
    // this app never picks between candidate boards, whatever their serials
    // were doing when the first one was auto-selected.
    std::vector<DeviceView> scan{ device("A", 1, "Unknown") };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();

    scan = { device("A", 1, "FW1111"), device("B", 2, "FW2222") };
    m.refresh();

    CHECK_FALSE(m.selected().has_value());
    CHECK_FALSE(m.selectedByIdOnly().has_value());   // genuinely cleared, not merely unconfirmed

    // ...and auto-select from a blank slate is fine once the list narrows back
    // to one.
    scan = { device("A", 1, "FW1111") };
    m.refresh();
    REQUIRE(m.selected().has_value());
    CHECK(m.selected()->serial == "FW1111");
}

TEST_CASE("selectedByIdOnly resolves by uniqueID alone, without the fingerprint check selected() adds")
{
    // The seam FlashDialog uses to get a specific SelectionCheck reason
    // (DifferentBoard vs DifferentPort) instead of selected()'s generic
    // nullopt -- see fwFlashDialog.cpp's Idle-state handling. Deliberately
    // permissive on its own; selectionUnchanged() is what re-applies the
    // check before anything is allowed to proceed.
    std::vector<DeviceView> scan{ device("A", 1, "SN-AAA") };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });
    m.refresh();
    m.select(0);

    // A different board on the same port: selected() refuses (an explicit
    // selection is never re-made on its own), but selectedByIdOnly() still
    // resolves it, by design, since it's the caller's job
    // (selectionUnchanged()) to catch the mismatch from here.
    scan = { device("B", 1, "SN-BBB") };
    m.refresh();

    CHECK_FALSE(m.selected().has_value());
    REQUIRE(m.selectedByIdOnly().has_value());
    CHECK(m.selectedByIdOnly()->serial == "SN-BBB");
}

TEST_CASE("snapshotAge reports the SCAN's age, and stays unknown no matter how often refresh() runs")
{
    // The distinction the flash dialog's freshness gate rests on. refresh()
    // re-reads the same cached scan result every frame, so "refresh() ran a
    // moment ago" says nothing whatever about how old the underlying device
    // data is -- which is precisely why a frozen scanner could hand the gate
    // stale data that looked current. snapshotAge() must therefore never be
    // derived from refresh() at all.
    //
    // Nothing has scanned in this process (see test_fwFinderManager.cpp), so
    // the honest answer is "unknown" -- and it stays unknown however many
    // refreshes happen, which is the property being asserted.
    std::vector<DeviceView> scan{ device("A", 1, "SN-AAA") };
    DeviceModel m([&scan]() -> std::expected<std::vector<DeviceView>, std::string> { return scan; });

    CHECK_FALSE(m.snapshotAge().has_value());
    for (int i = 0; i < 5; ++i) m.refresh();
    REQUIRE(m.devices().size() == 1);      // refresh() really did run and produce data
    CHECK_FALSE(m.snapshotAge().has_value());   // ...and it did not make that data look fresh
}

// ---------------------------------------------------------------------------
// A verified CPU probe as an identity source. It names a VOLUME rather than a
// port, because the CPU it speaks for is sitting in the RP2040's bootrom and
// publishes no port at all -- which is exactly why the other two sources can
// never speak for it.
// ---------------------------------------------------------------------------

TEST_CASE("a probed CPU is rendered with its drive and labelled by CPU probe") {
    CpuIdentity id;
    id.displayPort = "COM69";
    id.displaySource = IdentitySource::HubLocation;
    id = withVerifiedVolume(id, VerifiedVolume{ "G:/", TargetCpu::Main });

    const auto s = describeIdentity(id);
    // This is the exact line the board owner's session should now produce.
    CHECK(s.find("MAIN: G:/ (by CPU probe)") != std::string::npos);
    CHECK(s.find("DISPLAY: COM69 (by hub position)") != std::string::npos);
    // And no longer the thing that made it look hopeless.
    CHECK(s.find("MAIN: not identified") == std::string::npos);
}

TEST_CASE("withVerifiedVolume is pure and does nothing without a fact to fold in") {
    CpuIdentity original;
    original.mainPort = "COM60";
    original.mainSource = IdentitySource::HubLocation;

    const CpuIdentity same = withVerifiedVolume(original, VerifiedVolume{});
    CHECK(same.mainPort == original.mainPort);
    CHECK(same.mainSource == IdentitySource::HubLocation);
    CHECK_FALSE(same.mainVolume.has_value());
    CHECK_FALSE(same.displayVolume.has_value());

    // The argument is taken by value; the caller's copy is untouched.
    const CpuIdentity folded = withVerifiedVolume(original, VerifiedVolume{ "G:/", TargetCpu::Display });
    CHECK(folded.displayVolume.has_value());
    CHECK_FALSE(original.displayVolume.has_value());
}

TEST_CASE("a probe never overwrites a source that describes a port") {
    // describeIdentity() renders the PORT when a CPU has one, so the source
    // must keep describing the port -- otherwise the line would read
    // "COM60 (by CPU probe)", which is a drive letter's provenance attached to
    // a serial port.
    CpuIdentity id;
    id.mainPort = "COM60";
    id.mainSource = IdentitySource::HubLocation;
    id = withVerifiedVolume(id, VerifiedVolume{ "G:/", TargetCpu::Main });

    CHECK(id.mainSource == IdentitySource::HubLocation);
    CHECK(id.mainVolume == std::optional<std::string>{ "G:/" });   // the fact is still recorded
    CHECK(describeIdentity(id).find("MAIN: COM60 (by hub position)") != std::string::npos);
}

TEST_CASE("setVerifiedVolume folds the probe result into the scanned device") {
    DeviceModel model([] {
        std::vector<DeviceView> v{ device("A", 1) };
        return std::expected<std::vector<DeviceView>, std::string>(v);
    });

    model.refresh();
    REQUIRE(model.devices().size() == 1);
    CHECK_FALSE(model.devices()[0].identity.mainVolume.has_value());

    model.setVerifiedVolume(VerifiedVolume{ "G:/", TargetCpu::Main });
    model.refresh();
    REQUIRE(model.devices().size() == 1);
    CHECK(model.devices()[0].identity.mainVolume == std::optional<std::string>{ "G:/" });
    CHECK(model.devices()[0].identity.mainSource == IdentitySource::VerifiedProbe);

    // Cleared by supplying nothing -- which is what the app does every frame
    // the mapping stops applying. The model has no clock of its own, so this
    // is the ONLY way the fact goes away, and it must work.
    model.setVerifiedVolume(VerifiedVolume{});
    model.refresh();
    REQUIRE(model.devices().size() == 1);
    CHECK_FALSE(model.devices()[0].identity.mainVolume.has_value());
    CHECK(model.devices()[0].identity.mainSource == IdentitySource::None);
}

TEST_CASE("a probe result is never attached to a second board") {
    // The identification is derived by elimination across ONE board's two
    // CPUs. With two boards attached, two RPI-RP2 volumes can come from two
    // different boards, and attaching the answer to either would be attaching
    // it to a board it says nothing about.
    DeviceModel model([] {
        std::vector<DeviceView> v{ device("A", 1), device("B", 2) };
        return std::expected<std::vector<DeviceView>, std::string>(v);
    });

    model.setVerifiedVolume(VerifiedVolume{ "G:/", TargetCpu::Main });
    model.refresh();

    REQUIRE(model.devices().size() == 2);
    CHECK_FALSE(model.devices()[0].identity.mainVolume.has_value());
    CHECK_FALSE(model.devices()[1].identity.mainVolume.has_value());
}
