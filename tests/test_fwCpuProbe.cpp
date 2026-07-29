#include <doctest/doctest.h>

#include "catalog/fwCatalogEmbedded.h"
#include "core/fwSha256.h"
#include "flash/fwCpuProbe.h"
#include "flash/fwCpuProbeController.h"
#include "flash/fwFlashEngine.h"
#include "platform/fwSerialPorts.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace fwog;

namespace {

// ---------------------------------------------------------------------------
// A fake board. Every effect the flow can have is recorded, and every input it
// reads is scripted, so the whole recovery flow -- including the one write this
// project makes to an unidentified CPU -- runs with no hardware anywhere near
// it.
//
// Time is modelled in POLL TICKS, not wall clock: waitTick() increments a
// counter and the volume/port state is a function of it. That matters for the
// same reason it does in test_fwFlashEngine.cpp -- on a real board, a volume
// disappears some real time after the copy and the CDC port appears some real
// time after that, and collapsing either delay to zero would hide exactly the
// races this flow has to survive.
// ---------------------------------------------------------------------------
struct Harness {
    /// The two RPI-RP2 volumes present at the start. identifyCpus() writes to
    /// the FIRST and expects the SECOND to be what survives.
    std::vector<std::string> volumes = { "E:/", "F:/" };
    /// What findVolumes() reports once `releaseAfterTicks` have passed since
    /// the copy. The realistic value is "only the volume we did NOT write to".
    std::vector<std::string> volumesAfterRelease = { "F:/" };
    int releaseAfterTicks = 2;
    /// -1: the prober's volume never releases.
    bool volumeEverReleases = true;

    /// Ports present before anything reboots. This is a list of NAMES from the
    /// OS, and on Windows it is allowed to contain names of devices that no
    /// longer exist -- which is exactly the bug these tests exist to pin down.
    std::vector<std::string> portsBefore;
    /// Ports that appear `portAppearsAfterTicks` after the copy. Appended to
    /// `portsBefore`, so a name in both is a name that never looked new.
    std::vector<std::string> portsAfter = { "COM99" };
    int portAppearsAfterTicks = 3;
    bool portEverAppears = true;

    /// Ports whose USB identity is the prober's, once it has booted. This is
    /// what listSerialPortInfo() + looksLikeProberUsbId() produce in
    /// production; empty models a platform that cannot report USB identities.
    std::vector<std::string> proberIdPorts;

    /// Lines readLine() hands back for the port named by proberPort(), in
    /// order. Exhausted -> nullopt.
    std::vector<std::string> lines = { "main", "main" };
    /// Scripts for any OTHER port. A port with no script answers nothing at
    /// all, which is what an unrelated device that will not open looks like.
    std::map<std::string, std::vector<std::string>> otherLines;

    /// The port `lines` belongs to: the first one that turns up.
    std::string proberPort() const { return portsAfter.empty() ? std::string{} : portsAfter.front(); }

    CpuIdentity identity;

    /// nullopt -> serve the REAL embedded prober image, which is what
    /// production does and what makes the hash check meaningful here.
    std::optional<std::vector<uint8_t>> imageOverride;
    std::optional<std::string> imageLoadError;
    std::optional<std::string> copyError;

    // --- recorded effects ---
    std::vector<std::string> copiedTo;
    std::vector<std::string> openedPorts;
    std::vector<std::string> progress;
    int ticks = 0;
    int cancelAtTick = -1;
    std::optional<int> copyTick;
    /// How far through each port's script readLine() has got.
    std::map<std::string, std::size_t> cursor;

    bool released() const
    {
        return volumeEverReleases && copyTick.has_value() &&
               ticks >= *copyTick + releaseAfterTicks;
    }

    bool portUp() const
    {
        return portEverAppears && copyTick.has_value() &&
               ticks >= *copyTick + portAppearsAfterTicks;
    }

    ProbeIo io()
    {
        ProbeIo io;
        io.findVolumes = [this] { return released() ? volumesAfterRelease : volumes; };
        io.listPorts   = [this] {
            if (!portUp()) return portsBefore;
            std::vector<std::string> all = portsBefore;
            all.insert(all.end(), portsAfter.begin(), portsAfter.end());
            return all;
        };
        io.listProberPorts = [this] {
            return portUp() ? proberIdPorts : std::vector<std::string>{};
        };
        io.identify = [this] { return identity; };
        io.loadProbeImage = [this]() -> std::expected<std::vector<uint8_t>, std::string> {
            if (imageLoadError) return std::unexpected(*imageLoadError);
            if (imageOverride)  return *imageOverride;
            return loadEmbeddedImage(kProbeImageId);
        };
        io.stageFile = [](std::span<const uint8_t>, const std::string& name)
            -> std::expected<std::filesystem::path, std::string> {
                return std::filesystem::path("staged") / name;
            };
        io.copyToVolume = [this](const std::filesystem::path&, const std::string& volume)
            -> std::expected<void, std::string> {
                if (copyError) return std::unexpected(*copyError);
                copiedTo.push_back(volume);
                copyTick = ticks;
                return {};
            };
        io.readLine = [this](const std::string& port, int) -> std::optional<std::string> {
            openedPorts.push_back(port);
            const std::vector<std::string>& script =
                (port == proberPort()) ? lines : otherLines[port];
            std::size_t& at = cursor[port];
            if (at >= script.size()) return std::nullopt;
            return script[at++];
        };
        io.waitTick = [this](int) {
            if (cancelAtTick >= 0 && ticks >= cancelAtTick) return false;
            ++ticks;
            return true;
        };
        return io;
    }

    IdentifyResult run()
    {
        return identifyCpus(io(), [this](const std::string& s) { progress.push_back(s); });
    }
};

} // namespace

// ---------------------------------------------------------------------------
// The image itself: the runtime half of the drift gate.
// ---------------------------------------------------------------------------

TEST_CASE("the embedded CPU prober is present and is the exact image this build expects") {
    // If this fails, the committed probe.uf2 and the hash recorded in
    // fwCpuProbe.h have parted company -- which is the whole failure mode
    // committing a binary risks, and the reason it is checked here as well as
    // at configure time.
    const auto bytes = loadEmbeddedImage(kProbeImageId);
    REQUIRE(bytes.has_value());
    CHECK(sha256Hex(*bytes) == std::string(kProbeImageSha256));
    // 49,664 bytes, per probe/README.md. Not a hash substitute -- a cheap,
    // legible statement that the thing embedded is the prober and not, say, a
    // 16 MB display image that happened to land under the same id.
    CHECK(bytes->size() == 49664u);
}

// ---------------------------------------------------------------------------
// parseProbeLine: strict, and strict in the direction that refuses.
// ---------------------------------------------------------------------------

TEST_CASE("parseProbeLine accepts exactly main and display, and nothing else") {
    CHECK(parseProbeLine("main")    == std::optional<ProbeAnswer>{ProbeAnswer::Main});
    CHECK(parseProbeLine("display") == std::optional<ProbeAnswer>{ProbeAnswer::Display});
    CHECK(parseProbeLine("  main\t") == std::optional<ProbeAnswer>{ProbeAnswer::Main});
    CHECK(parseProbeLine("MAIN")    == std::optional<ProbeAnswer>{ProbeAnswer::Main});

    // No prefix matching, no substring matching, no nearest match.
    CHECK_FALSE(parseProbeLine("mai").has_value());
    CHECK_FALSE(parseProbeLine("mainx").has_value());
    CHECK_FALSE(parseProbeLine("the main cpu").has_value());
    CHECK_FALSE(parseProbeLine("display main").has_value());
    CHECK_FALSE(parseProbeLine("").has_value());
    CHECK_FALSE(parseProbeLine("   ").has_value());
    CHECK_FALSE(parseProbeLine("0").has_value());
}

// ---------------------------------------------------------------------------
// The happy path, both ways round.
// ---------------------------------------------------------------------------

TEST_CASE("a prober answering \"main\" identifies the remaining volume as DISPLAY") {
    Harness h;
    h.lines = { "main", "main" };

    const auto r = h.run();

    REQUIRE(r.outcome == IdentifyOutcome::Success);
    CHECK(r.proberCpu    == TargetCpu::Main);
    CHECK(r.remainingCpu == TargetCpu::Display);
    CHECK(r.proberVolume    == "E:/");
    CHECK(r.remainingVolume == "F:/");
    CHECK(r.proberPort == "COM99");
    // Exactly one write happened, and it went to the volume the flow says it
    // went to.
    CHECK(h.copiedTo == std::vector<std::string>{ "E:/" });
    // The conclusion is by elimination and the message says so.
    CHECK(r.message.find("DISPLAY") != std::string::npos);
    CHECK(r.message.find("F:/") != std::string::npos);
}

TEST_CASE("a prober answering \"display\" identifies the remaining volume as MAIN") {
    Harness h;
    h.lines = { "display", "display" };

    const auto r = h.run();

    REQUIRE(r.outcome == IdentifyOutcome::Success);
    CHECK(r.proberCpu    == TargetCpu::Display);
    CHECK(r.remainingCpu == TargetCpu::Main);
    CHECK(h.copiedTo == std::vector<std::string>{ "E:/" });
}

TEST_CASE("the answer is taken from the prober, not from which volume was written") {
    // The two happy-path cases above differ ONLY in what the prober said, and
    // produce opposite mappings from identical volume state. That is the whole
    // mechanism: which drive was written to carries no information at all.
    Harness a; a.lines = { "main", "main" };
    Harness b; b.lines = { "display", "display" };
    const auto ra = a.run();
    const auto rb = b.run();
    REQUIRE(ra.outcome == IdentifyOutcome::Success);
    REQUIRE(rb.outcome == IdentifyOutcome::Success);
    CHECK(ra.proberVolume == rb.proberVolume);
    CHECK(ra.remainingCpu != rb.remainingCpu);
}

// ---------------------------------------------------------------------------
// Volume-count preconditions.
// ---------------------------------------------------------------------------

TEST_CASE("three or more volumes refuse, and nothing is written") {
    Harness h;
    h.volumes = { "E:/", "F:/", "G:/" };

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::NotExactlyTwoVolumes);
    CHECK(h.copiedTo.empty());
}

TEST_CASE("one volume refuses -- there is nothing to disambiguate") {
    Harness h;
    h.volumes = { "E:/" };
    const auto r = h.run();
    CHECK(r.outcome == IdentifyOutcome::NotExactlyTwoVolumes);
    CHECK(h.copiedTo.empty());
}

TEST_CASE("no volumes refuses") {
    Harness h;
    h.volumes = {};
    const auto r = h.run();
    CHECK(r.outcome == IdentifyOutcome::NotExactlyTwoVolumes);
    CHECK(h.copiedTo.empty());
}

TEST_CASE("a third volume appearing mid-flow refuses rather than concluding") {
    Harness h;
    h.volumesAfterRelease = { "F:/", "G:/", "H:/" };

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::VolumeSetChanged);
    // The write to the prober volume DID happen -- it is what started the
    // sequence -- but no mapping came out of it.
    CHECK(h.copiedTo == std::vector<std::string>{ "E:/" });
    CHECK(r.remainingVolume.empty());
}

TEST_CASE("an unrelated volume replacing the pair refuses") {
    Harness h;
    h.volumesAfterRelease = { "Z:/" };
    const auto r = h.run();
    CHECK(r.outcome == IdentifyOutcome::VolumeSetChanged);
}

TEST_CASE("the WRONG volume releasing refuses instead of assuming the mapping") {
    // The one we wrote to is still there and the other one vanished. Nothing
    // about that is what writing the prober does, so neither drive is known.
    Harness h;
    h.volumesAfterRelease = { "E:/" };

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::WrongVolumeReleased);
    CHECK(r.remainingVolume.empty());
}

TEST_CASE("a volume that never releases times out rather than waiting forever") {
    Harness h;
    h.volumeEverReleases = false;

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::VolumeReleaseTimeout);
    // Bounded: the wait really did stop, and at roughly its own budget.
    CHECK(h.ticks <= kProbeReleaseWaitMs / kProbePollMs + 2);
}

// ---------------------------------------------------------------------------
// The CDC port.
// ---------------------------------------------------------------------------

TEST_CASE("both volumes going away times out rather than concluding by elimination") {
    // There is nothing left to eliminate down to. This is bounded and refuses;
    // it is deliberately NOT an instant refusal, because by this point the
    // prober has already been written and there is no second volume left to
    // retry with -- see the wait loop's comment.
    Harness h;
    h.volumesAfterRelease = {};

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::VolumeReleaseTimeout);
    CHECK(r.remainingVolume.empty());
    CHECK(r.message.find("F:/") != std::string::npos);
}

TEST_CASE("a CDC port that never appears refuses, and never reads anything") {
    Harness h;
    h.portEverAppears = false;

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::NoProbePort);
    CHECK(h.openedPorts.empty());
    CHECK(r.remainingVolume.empty());
}

TEST_CASE("a port that appears late is still used -- Windows binds usbser after a delay") {
    Harness h;
    // Well after the volume released, which is the realistic ordering.
    h.portAppearsAfterTicks = 40;

    const auto r = h.run();

    REQUIRE(r.outcome == IdentifyOutcome::Success);
    CHECK(r.proberPort == "COM99");
}

TEST_CASE("ports that were already there are not mistaken for the prober's") {
    Harness h;
    h.portsBefore = { "COM3", "COM7" };
    h.portsAfter  = { "COM42" };

    const auto r = h.run();

    REQUIRE(r.outcome == IdentifyOutcome::Success);
    CHECK(r.proberPort == "COM42");
    // The ports that were already present are never opened: they are not
    // candidates, and a recovery tool must not go poking at hardware it was
    // not pointed at.
    for (const auto& p : h.openedPorts) CHECK(p == "COM42");
}

// ---------------------------------------------------------------------------
// Finding the port by USB IDENTITY rather than by difference. This is the bug
// the board owner hit on real hardware, and the section below is the regression
// suite for it.
// ---------------------------------------------------------------------------

TEST_CASE("looksLikeProberUsbId matches the prober's CDC and nothing else") {
    // Captured verbatim from the board this was debugged against: the prober
    // on COM69, as Windows reported it.
    CHECK(looksLikeProberUsbId("USB\\VID_2E8A&PID_000A&MI_00\\7&29198214&0&0000"));
    // Windows is inconsistent about the case of these strings.
    CHECK(looksLikeProberUsbId("usb\\vid_2e8a&pid_000a&mi_00\\7&29198214&0&0000"));
    // A non-composite id names no interface; the vendor/product pair is what
    // identifies the device.
    CHECK(looksLikeProberUsbId("USB\\VID_2E8A&PID_000A\\E6614C311B8B2A31"));

    // Interface 2 is pico_stdio_usb's Reset interface -- the 1200-baud path,
    // not a serial port. Matching it would queue something unreadable.
    CHECK_FALSE(looksLikeProberUsbId("USB\\VID_2E8A&PID_000A&MI_02\\7&29198214&0&0002"));
    // The FTDI port that was sitting alongside the prober on the same machine.
    CHECK_FALSE(looksLikeProberUsbId("FTDIBUS\\VID_0403+PID_6014+FW6548A\\0000"));
    // An RP2040 in BOOTSEL: same vendor, different product, and not a CDC.
    CHECK_FALSE(looksLikeProberUsbId("USB\\VID_2E8A&PID_0003\\E0C9125B0D9B"));
    CHECK_FALSE(looksLikeProberUsbId("USB\\VID_2E8A&MI_00\\7&1"));
    // "Could not say" is not "matches". Absence of evidence is never evidence.
    CHECK_FALSE(looksLikeProberUsbId(""));
}

TEST_CASE("the prober is found even when its COM name was ALREADY in the before list") {
    // THE BUG, exactly as it happened. Windows reuses COM numbers and
    // HKLM\HARDWARE\DEVICEMAP\SERIALCOMM keeps names for devices that are long
    // gone, so COM69 was in the "before" snapshot three times over from earlier
    // plug cycles. The prober then enumerated AS COM69. Set-difference on names
    // therefore saw nothing appear, and the app reported "no new serial port"
    // while Device Manager was showing the prober plainly.
    Harness h;
    h.portsBefore   = { "COM24", "COM59", "COM69", "COM69", "COM69" };
    h.portsAfter    = { "COM69" };          // the same name: nothing looks new
    h.proberIdPorts = { "COM69" };          // but the USB identity is right there
    h.lines = { "display", "display" };

    const auto r = h.run();

    REQUIRE(r.outcome == IdentifyOutcome::Success);
    CHECK(r.proberPort   == "COM69");
    CHECK(r.proberCpu    == TargetCpu::Display);
    CHECK(r.remainingCpu == TargetCpu::Main);
    CHECK(r.remainingVolume == "F:/");
    // Only the identified port was ever opened -- not the stale names.
    for (const auto& p : h.openedPorts) CHECK(p == "COM69");
}

TEST_CASE("duplicate entries in the port list do not make one port look like several") {
    // The second failure mode of the same registry key. Had COM69 not already
    // been in the before-set, three stale copies of it would have surfaced as
    // three separate "new" ports and tripped the old "more than one new serial
    // port appeared" refusal -- on a machine where exactly one had.
    Harness h;
    h.portsAfter = { "COM99", "COM99", "COM99" };

    const auto r = h.run();

    REQUIRE(r.outcome == IdentifyOutcome::Success);
    CHECK(r.proberPort == "COM99");
    for (const auto& p : h.openedPorts) CHECK(p == "COM99");
}

TEST_CASE("a candidate that answers nothing is not believed, and a later one is asked") {
    // Two ports turn up. The first one opens and says nothing at all; the
    // second speaks the protocol. The old flow refused outright at "more than
    // one new port"; nothing about that refusal made anyone safer, because the
    // answer was on the second port and could be had for the asking.
    Harness h;
    h.portsAfter  = { "COM99", "COM100" };
    h.lines       = {};                                   // COM99: silence
    h.otherLines["COM100"] = { "display", "display" };

    const auto r = h.run();

    REQUIRE(r.outcome == IdentifyOutcome::Success);
    CHECK(r.proberPort   == "COM100");
    CHECK(r.proberCpu    == TargetCpu::Display);
    CHECK(r.remainingCpu == TargetCpu::Main);
    // Both were asked, and the silent one was asked first.
    REQUIRE_FALSE(h.openedPorts.empty());
    CHECK(h.openedPorts.front() == "COM99");
    CHECK(std::find(h.openedPorts.begin(), h.openedPorts.end(), "COM100")
          != h.openedPorts.end());
}

TEST_CASE("a candidate that answers GARBAGE is written off, and a later one is asked") {
    Harness h;
    h.portsAfter  = { "COM99", "COM100" };
    h.lines       = { "READY.", "READY." };               // some other device entirely
    h.otherLines["COM100"] = { "main", "main" };

    const auto r = h.run();

    REQUIRE(r.outcome == IdentifyOutcome::Success);
    CHECK(r.proberPort   == "COM100");
    CHECK(r.remainingCpu == TargetCpu::Display);
    // Written off means written off: a port that said it is not the prober is
    // never asked a second time.
    CHECK(std::count(h.openedPorts.begin(), h.openedPorts.end(), std::string("COM99")) == 1);
}

TEST_CASE("a port with the prober's USB identity is asked BEFORE a merely-new one") {
    // Both are candidates; the identified one is stronger evidence, so it goes
    // first. Newness is an ordering hint now, not a gate.
    Harness h;
    h.portsBefore   = { "COM69" };            // stale name, reused below
    h.portsAfter    = { "COM69", "COM100" };  // COM100 is the only NEW name
    h.proberIdPorts = { "COM69" };
    h.lines         = { "main", "main" };     // COM69 is the real prober

    const auto r = h.run();

    REQUIRE(r.outcome == IdentifyOutcome::Success);
    CHECK(r.proberPort == "COM69");
    REQUIRE_FALSE(h.openedPorts.empty());
    CHECK(h.openedPorts.front() == "COM69");
    // The unrelated new port was never opened at all: the identified one
    // answered first.
    CHECK(std::find(h.openedPorts.begin(), h.openedPorts.end(), "COM100")
          == h.openedPorts.end());
}

TEST_CASE("a device with the prober's USB identity that is NOT the prober is rejected") {
    // Any RP2040 running pico_stdio_usb presents VID_2E8A&PID_000A&MI_00. A USB
    // identity match earns a port a place in the queue and nothing else; what
    // settles it is what the port says.
    Harness h;
    h.portsAfter    = { "COM99", "COM100" };
    h.proberIdPorts = { "COM99", "COM100" };
    h.lines         = { "MicroPython v1.22.0" };          // COM99: another Pico
    h.otherLines["COM100"] = { "display", "display" };

    const auto r = h.run();

    REQUIRE(r.outcome == IdentifyOutcome::Success);
    CHECK(r.proberPort == "COM100");
    CHECK(r.proberCpu  == TargetCpu::Display);
}

TEST_CASE("no candidate answering refuses, and says what was actually tried") {
    // Fail closed, but honestly. "No new serial port appeared" was the old
    // wording and is now a lie in this situation: two ports appeared, both were
    // asked, and neither answered.
    Harness h;
    h.portsAfter  = { "COM99", "COM100" };
    h.lines       = { "READY." };
    h.otherLines["COM100"] = {};                          // silence

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::UnrecognisedProbeLine);
    CHECK(r.remainingVolume.empty());
    // The message describes what happened to each port, by name.
    CHECK(r.message.find("COM99") != std::string::npos);
    CHECK(r.message.find("COM100") != std::string::npos);
    CHECK(r.message.find("READY.") != std::string::npos);
    CHECK(r.message.find("2 serial ports were asked") != std::string::npos);
    // And it does not claim the thing that was false.
    CHECK(r.message.find("no new serial port appeared") == std::string::npos);
}

TEST_CASE("nothing to ask at all refuses as NoProbePort, and opens nothing") {
    // Distinct from "asked and got nothing": there was no candidate. The
    // wording has to be able to tell the user which of those happened.
    Harness h;
    h.portEverAppears = false;
    h.portsBefore = { "COM24", "COM59" };   // ports exist; none of them is a candidate

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::NoProbePort);
    CHECK(h.openedPorts.empty());
    CHECK(r.message.find(kProbeUsbVid) != std::string::npos);
    CHECK(r.message.find(kProbeUsbPid) != std::string::npos);
}

TEST_CASE("the whole port search is bounded even with candidates that never answer") {
    Harness h;
    h.portsAfter    = { "COM99", "COM100", "COM101" };
    h.proberIdPorts = { "COM99", "COM100", "COM101" };
    h.lines = {};                                          // all three stay silent

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::NoProbeLine);
    // The ceiling is a fixed number of questions, each a fixed number of reads.
    CHECK(int(h.openedPorts.size()) <= kProbeMaxPortQuestions * kProbeLineAttempts);
    // And it stopped on the QUESTION budget, nowhere near the wait budget --
    // i.e. it did not sit in the poll loop re-listing ports for 25 seconds.
    CHECK(h.ticks * kProbePollMs < kProbePortWaitMs);
}

TEST_CASE("a silent candidate is asked again rather than written off") {
    // The window between Windows publishing a COM name and the port actually
    // opening is real. Disqualifying a port for losing that race would be the
    // same class of mistake as the bug being fixed.
    Harness h;
    h.portsAfter = { "COM99" };
    // Nothing on the first question (four reads), the answer on the second.
    h.lines = { "", "", "", "", "main", "main" };

    const auto r = h.run();

    REQUIRE(r.outcome == IdentifyOutcome::Success);
    CHECK(r.proberPort == "COM99");
    CHECK(int(h.openedPorts.size()) > kProbeLineAttempts);   // it really was asked twice
}

TEST_CASE("dedupePortNames keeps the first of each name and the order of what survives") {
    // The registry key listSerialPorts() used to read held 29 values naming 5
    // distinct ports on the machine this was fixed on -- COM69 four times over.
    // A list that names one port several times makes "how many ports are there"
    // and "how many appeared" both wrong, and a caller counting them reaches a
    // confident wrong conclusion rather than an obvious failure.
    const std::vector<std::string> stale = {
        "COM59", "COM59", "COM24", "COM69", "COM69", "COM69", "COM68", "COM68",
        "COM59", "COM69", "COM70"
    };
    CHECK(dedupePortNames(stale) ==
          std::vector<std::string>{ "COM59", "COM24", "COM69", "COM68", "COM70" });

    // Order is "whatever the OS said", so this must not quietly sort.
    CHECK(dedupePortNames({ "COM9", "COM1", "COM9" }) ==
          std::vector<std::string>{ "COM9", "COM1" });

    CHECK(dedupePortNames({}).empty());
    CHECK(dedupePortNames({ "COM1" }) == std::vector<std::string>{ "COM1" });
}

// ---------------------------------------------------------------------------
// What the port says.
// ---------------------------------------------------------------------------

TEST_CASE("a garbage line refuses outright") {
    Harness h;
    h.lines = { "hello world", "main" };

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::UnrecognisedProbeLine);
    CHECK(r.message.find("hello world") != std::string::npos);
    CHECK(r.remainingVolume.empty());
}

TEST_CASE("a single reading is not enough -- the second must confirm it") {
    Harness h;
    h.lines = { "main" };

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::NoProbeLine);
    CHECK(r.remainingVolume.empty());
}

TEST_CASE("no lines at all refuses") {
    Harness h;
    h.lines = {};
    const auto r = h.run();
    CHECK(r.outcome == IdentifyOutcome::NoProbeLine);
}

TEST_CASE("two readings that disagree refuse -- an unstable answer is not an answer") {
    Harness h;
    h.lines = { "main", "display" };

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::UnstableProbeAnswer);
    CHECK(r.remainingVolume.empty());
}

TEST_CASE("blank lines are skipped without being treated as an answer or an error") {
    Harness h;
    h.lines = { "", "main", "main" };
    const auto r = h.run();
    REQUIRE(r.outcome == IdentifyOutcome::Success);
    CHECK(r.proberCpu == TargetCpu::Main);
}

TEST_CASE("reading is bounded by a fixed number of attempts") {
    Harness h;
    // Endless blank lines: never an answer, never an error, must still stop.
    h.lines.assign(1000, "");
    const auto r = h.run();
    CHECK(r.outcome == IdentifyOutcome::NoProbeLine);
    // One question is kProbeLineAttempts reads; the search asks at most
    // kProbeMaxPortQuestions questions in total, however many candidates there
    // are and however many times each is retried.
    CHECK(int(h.openedPorts.size()) <= kProbeMaxPortQuestions * kProbeLineAttempts);
}

// ---------------------------------------------------------------------------
// Contradictory evidence.
// ---------------------------------------------------------------------------

TEST_CASE("a prober claiming MAIN while a MAIN port is visible refuses") {
    Harness h;
    h.lines = { "main", "main" };
    // fwfinder simultaneously reports a healthy MAIN serial port. A CPU in its
    // bootloader has none, so this cannot all be one board.
    h.identity.mainPort = "COM5";

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::ContradictedByPorts);
    CHECK(r.remainingVolume.empty());
}

TEST_CASE("a prober claiming DISPLAY while a DISPLAY port is visible refuses") {
    Harness h;
    h.lines = { "display", "display" };
    h.identity.displayPort = "COM6";
    const auto r = h.run();
    CHECK(r.outcome == IdentifyOutcome::ContradictedByPorts);
}

TEST_CASE("a port for the OTHER CPU is not a contradiction") {
    // The prober is on MAIN; a DISPLAY port existing says nothing about that,
    // and refusing here would block a legitimate recovery.
    Harness h;
    h.lines = { "main", "main" };
    h.identity.displayPort = "COM6";
    const auto r = h.run();
    CHECK(r.outcome == IdentifyOutcome::Success);
}

TEST_CASE("the prober's own port being reported as the CPU's port is not a contradiction") {
    Harness h;
    h.lines = { "main", "main" };
    h.identity.mainPort = "COM99";   // the prober's own port
    const auto r = h.run();
    CHECK(r.outcome == IdentifyOutcome::Success);
}

// ---------------------------------------------------------------------------
// The image, and the copy.
// ---------------------------------------------------------------------------

TEST_CASE("an image whose hash does not match is never written") {
    Harness h;
    // A structurally valid UF2 that simply is not the prober.
    auto bytes = loadEmbeddedImage(kProbeImageId);
    REQUIRE(bytes.has_value());
    (*bytes)[300] = uint8_t((*bytes)[300] ^ 0xFF);   // payload byte, still a valid UF2
    h.imageOverride = *bytes;

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::ProbeImageRejected);
    CHECK(h.copiedTo.empty());
    CHECK(r.message.find(kProbeImageSha256) != std::string::npos);
}

TEST_CASE("an image that will not load is never written") {
    Harness h;
    h.imageLoadError = "no embedded image named \"fwog_probe\"";
    const auto r = h.run();
    CHECK(r.outcome == IdentifyOutcome::ProbeImageRejected);
    CHECK(h.copiedTo.empty());
}

TEST_CASE("a failed copy reports CopyFailed and produces no mapping") {
    Harness h;
    h.copyError = "the device is not ready";
    const auto r = h.run();
    CHECK(r.outcome == IdentifyOutcome::CopyFailed);
    CHECK(h.copiedTo.empty());
    CHECK(r.remainingVolume.empty());
}

TEST_CASE("cancelling during the release wait aborts") {
    Harness h;
    h.volumeEverReleases = false;
    h.cancelAtTick = 3;

    const auto r = h.run();

    CHECK(r.outcome == IdentifyOutcome::Aborted);
    CHECK(h.ticks <= 4);
}

// ---------------------------------------------------------------------------
// The freshness gate. Same shape, and the same fail-closed rules, as
// selectionUnchangedFresh().
// ---------------------------------------------------------------------------

namespace {

IdentifyResult successfulMapping()
{
    IdentifyResult r;
    r.outcome         = IdentifyOutcome::Success;
    r.proberVolume    = "E:/";
    r.remainingVolume = "F:/";
    r.proberPort      = "COM99";
    r.proberCpu       = TargetCpu::Main;
    r.remainingCpu    = TargetCpu::Display;
    r.message = "the prober is running on the MAIN CPU (it was written to E:/), so the "
                "RPI-RP2 volume still mounted, F:/, is the DISPLAY CPU.";
    return r;
}

} // namespace

TEST_CASE("a mapping is fresh only while its volume is the one and only one mounted") {
    const auto r = successfulMapping();
    const std::vector<std::string> same = { "F:/" };
    CHECK(mappingStillFresh(r, same, std::chrono::milliseconds{500}) == MappingCheck::Fresh);

    const std::vector<std::string> none;
    CHECK(mappingStillFresh(r, none, std::chrono::milliseconds{500}) == MappingCheck::VolumesChanged);

    const std::vector<std::string> both = { "E:/", "F:/" };
    CHECK(mappingStillFresh(r, both, std::chrono::milliseconds{500}) == MappingCheck::VolumesChanged);

    const std::vector<std::string> other = { "G:/" };
    CHECK(mappingStillFresh(r, other, std::chrono::milliseconds{500}) == MappingCheck::VolumesChanged);
}

TEST_CASE("an unknown age is stale, never fresh") {
    const auto r = successfulMapping();
    const std::vector<std::string> same = { "F:/" };
    CHECK(mappingStillFresh(r, same, std::nullopt) == MappingCheck::StaleIdentification);
}

TEST_CASE("an age past the limit is stale") {
    const auto r = successfulMapping();
    const std::vector<std::string> same = { "F:/" };
    CHECK(mappingStillFresh(r, same, kMaxMappingAge) == MappingCheck::Fresh);
    CHECK(mappingStillFresh(r, same, kMaxMappingAge + std::chrono::milliseconds{1})
          == MappingCheck::StaleIdentification);
}

TEST_CASE("a failed identification is never a mapping, however fresh") {
    IdentifyResult r;
    r.outcome = IdentifyOutcome::NoProbeLine;
    r.remainingVolume = "F:/";   // even if the field happened to be populated
    const std::vector<std::string> same = { "F:/" };
    CHECK(mappingStillFresh(r, same, std::chrono::milliseconds{0}) == MappingCheck::NoIdentification);
}

TEST_CASE("every non-fresh MappingCheck explains itself") {
    CHECK(mappingCheckMessage(MappingCheck::Fresh).empty());
    CHECK_FALSE(mappingCheckMessage(MappingCheck::NoIdentification).empty());
    CHECK_FALSE(mappingCheckMessage(MappingCheck::VolumesChanged).empty());
    CHECK_FALSE(mappingCheckMessage(MappingCheck::StaleIdentification).empty());
}

// ---------------------------------------------------------------------------
// The controller's state machine, driven directly -- no worker thread.
// ---------------------------------------------------------------------------

TEST_CASE("the controller reports an age only for a SUCCESSFUL identification") {
    CpuProbeController c;
    CHECK(c.state() == ProbeState::Idle);
    CHECK_FALSE(c.identificationAge().has_value());

    c.onFinished(successfulMapping());
    CHECK(c.state() == ProbeState::Finished);
    REQUIRE(c.identificationAge().has_value());
    CHECK(*c.identificationAge() < std::chrono::milliseconds{5000});

    IdentifyResult bad;
    bad.outcome = IdentifyOutcome::NoProbePort;
    c.onFinished(bad);
    CHECK(c.state() == ProbeState::Finished);
    CHECK_FALSE(c.identificationAge().has_value());
}

TEST_CASE("the controller keeps the progress log and clears it on reset") {
    CpuProbeController c;
    c.onProgress("loading the CPU prober image");
    c.onProgress("writing the CPU prober to E:/");
    CHECK(c.log().size() == 2);

    c.onFinished(successfulMapping());
    // The final message joins the log so the user reads one story, not two.
    CHECK(c.log().size() == 3);

    c.reset();
    CHECK(c.state() == ProbeState::Idle);
    CHECK(c.log().empty());
    CHECK(c.result().outcome != IdentifyOutcome::Success);
    CHECK_FALSE(c.identificationAge().has_value());
}

TEST_CASE("returnProberToBootsel refuses when there is no successful identification") {
    CpuProbeController c;
    CHECK_FALSE(c.returnProberToBootsel());

    IdentifyResult bad;
    bad.outcome = IdentifyOutcome::UnstableProbeAnswer;
    c.onFinished(bad);
    CHECK_FALSE(c.returnProberToBootsel());
}

// ---------------------------------------------------------------------------
// verifiedVolumeFrom: the ONE door between this flow and everything that can
// write to a board, with the freshness rule bolted to it.
// ---------------------------------------------------------------------------

TEST_CASE("a fresh mapping yields the volume still mounted, named by elimination") {
    const auto r = successfulMapping();   // prober landed on MAIN; F:/ is DISPLAY
    const std::vector<std::string> same{ "F:/" };

    const VerifiedVolume v = verifiedVolumeFrom(r, same, std::chrono::milliseconds{500});
    REQUIRE(v.known());
    CHECK(v.volume == "F:/");
    // Named by ELIMINATION, not measured: the prober measured itself onto MAIN,
    // so the drive left over is the other one. Getting this backwards would
    // authorise a MAIN image onto the DISPLAY CPU, which is the damage every
    // refusal in this project exists to prevent.
    CHECK(v.cpu == TargetCpu::Display);
}

TEST_CASE("the prober's own volume never comes out, even though it was measured") {
    // It is the STRONGER of the two facts and still must not authorise
    // anything: writing the prober is what stopped that CPU being mass storage,
    // so the letter it used is gone -- and a drive letter that has gone is the
    // one most likely to come back attached to something else.
    const auto r = successfulMapping();
    const std::vector<std::string> proberBack{ "E:/" };
    CHECK_FALSE(verifiedVolumeFrom(r, proberBack, std::chrono::milliseconds{500}).known());
}

TEST_CASE("a stale mapping yields nothing at all") {
    const auto r = successfulMapping();

    // The volume went away.
    CHECK_FALSE(verifiedVolumeFrom(r, std::vector<std::string>{},
                                    std::chrono::milliseconds{500}).known());
    // A second volume appeared beside it.
    CHECK_FALSE(verifiedVolumeFrom(r, std::vector<std::string>{ "F:/", "G:/" },
                                    std::chrono::milliseconds{500}).known());
    // A different letter is mounted -- a replug.
    CHECK_FALSE(verifiedVolumeFrom(r, std::vector<std::string>{ "G:/" },
                                    std::chrono::milliseconds{500}).known());
    // Too old, even though the volume set is untouched.
    CHECK_FALSE(verifiedVolumeFrom(r, std::vector<std::string>{ "F:/" },
                                    kMaxMappingAge + std::chrono::milliseconds{1}).known());
    // Age unknown: the least verified state there is, and it must not read as
    // fresh.
    CHECK_FALSE(verifiedVolumeFrom(r, std::vector<std::string>{ "F:/" }, std::nullopt).known());
}

TEST_CASE("a failed identification yields nothing, whatever is mounted") {
    IdentifyResult bad;
    bad.outcome         = IdentifyOutcome::UnstableProbeAnswer;
    bad.remainingVolume = "F:/";       // set, but the flow refused
    bad.remainingCpu    = TargetCpu::Display;
    CHECK_FALSE(verifiedVolumeFrom(bad, std::vector<std::string>{ "F:/" },
                                    std::chrono::milliseconds{0}).known());
}

// ---------------------------------------------------------------------------
// And the thing this whole flow must NOT have changed.
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kMagic0 = 0x0A324655, kMagic1 = 0x9E5D5157;
constexpr uint32_t kMagicEnd = 0x0AB16F30, kRp2040 = 0xE48BFF56;
constexpr uint32_t kFamilyPresent = 0x00002000;

void put32(std::vector<uint8_t>& v, size_t off, uint32_t val)
{
    v[off + 0] = uint8_t(val); v[off + 1] = uint8_t(val >> 8);
    v[off + 2] = uint8_t(val >> 16); v[off + 3] = uint8_t(val >> 24);
}

std::vector<uint8_t> goodUf2()
{
    std::vector<uint8_t> v(512, 0);
    put32(v, 0, kMagic0);  put32(v, 4, kMagic1);
    put32(v, 8, kFamilyPresent); put32(v, 12, 0x10000000);
    put32(v, 16, 256); put32(v, 20, 0); put32(v, 24, 1);
    put32(v, 28, kRp2040); put32(v, 508, kMagicEnd);
    return v;
}

} // namespace

TEST_CASE("an ordinary flash with two volumes mounted is STILL refused") {
    // The recovery flow above writes to an unidentified volume. This is the
    // guarantee that doing so did not become generally permitted: the same two
    // mounted volumes, through the ordinary flash path, still refuse before
    // anything is touched -- with a typed confirmation supplied, which may not
    // make any difference.
    //
    // NO MAIN PORT, and that is the point of the fixture rather than an
    // omission: MAIN is one of these two drives here, and a CPU sitting in the
    // bootrom publishes mass storage, not a CDC port. A port would say MAIN is
    // RUNNING and therefore is NEITHER drive, which is a different board state
    // with a different (and correct) answer -- the engine touches that port and
    // takes the drive the touch produces. See "mounted volumes do not block a
    // target CPU that is demonstrably running" in test_fwFlashEngine.cpp.
    std::vector<std::string> copiedTo;
    std::vector<std::string> touched;

    FlashIo io;
    CpuIdentity identity;
    io.identify    = [identity] { return identity; };
    io.findVolumes = [] { return std::vector<std::string>{ "E:/", "F:/" }; };
    io.touchPort   = [&touched](const std::string& p) { touched.push_back(p); };
    io.loadImage   = [](const ImageRef&) -> std::expected<std::vector<uint8_t>, std::string> {
        return goodUf2();
    };
    io.stageFile   = [](std::span<const uint8_t>, const std::string& n)
        -> std::expected<std::filesystem::path, std::string> { return std::filesystem::path(n); };
    io.copyToVolume = [&copiedTo](const std::filesystem::path&, const std::string& v)
        -> std::expected<void, std::string> { copiedTo.push_back(v); return {}; };
    io.waitTick = [](int) { return true; };

    FlashStep step;
    step.cpu = TargetCpu::Main;
    step.description = "MAIN app";
    const std::vector<FlashStep> plan{ step };

    const auto result = runFlashPlan(io, plan, "MAIN", nullptr);

    CHECK(result.outcome == FlashOutcome::RefusedAmbiguous);
    CHECK(copiedTo.empty());
    CHECK(touched.empty());
    CHECK_FALSE(result.resumable);
}
