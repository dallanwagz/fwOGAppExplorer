#include <doctest/doctest.h>
#include "flash/fwFlashEngine.h"
#include "core/fwSha256.h"
#include "device/fwDeviceModel.h"   // withVerifiedVolume

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace fwog;

namespace {

constexpr uint32_t kMagic0 = 0x0A324655, kMagic1 = 0x9E5D5157;
constexpr uint32_t kMagicEnd = 0x0AB16F30, kRp2040 = 0xE48BFF56;
constexpr uint32_t kFamilyPresent = 0x00002000;

void put32(std::vector<uint8_t>& v, size_t off, uint32_t val) {
    v[off + 0] = uint8_t(val); v[off + 1] = uint8_t(val >> 8);
    v[off + 2] = uint8_t(val >> 16); v[off + 3] = uint8_t(val >> 24);
}

std::vector<uint8_t> goodUf2(uint32_t family = kRp2040) {
    std::vector<uint8_t> v(512, 0);
    put32(v, 0, kMagic0);  put32(v, 4, kMagic1);
    put32(v, 8, kFamilyPresent); put32(v, 12, 0x10000000);
    put32(v, 16, 256); put32(v, 20, 0); put32(v, 24, 1);
    put32(v, 28, family); put32(v, 508, kMagicEnd);
    return v;
}

/// Records every effect the engine produced, so tests assert on behaviour
/// rather than on internal state.
///
/// Volume timing is modelled relative to the tick a touch or a copy
/// happened, not to wall-clock ticks since the test started: on real
/// hardware, a touch causes a volume to appear only after the CPU actually
/// reboots into BOOTSEL, and a copy causes that same volume to disappear
/// only after the CPU actually reboots off mass storage. Both take a real,
/// non-zero, non-instant amount of time, and the fixture must not collapse
/// that delay to zero -- doing so was tried and hid a wrong-CPU-write defect
/// (see "a later step does not copy to the previous step's still-mounted
/// volume" below).
struct Harness {
    CpuIdentity identity;
    std::vector<std::string> volumes;
    std::vector<uint8_t> image = goodUf2();

    std::vector<std::string> touched;
    std::vector<std::string> copiedTo;
    int waitTicks = 0;

    /// Ticks since the most recent touch before its volume appears. -1 means
    /// it never does.
    int volumeAppearsAfter = 1;
    /// ABSOLUTE `waitTicks` value at which a volume appears with NO touch
    /// having happened -- the serial-less arrival, where the user holding the
    /// red button while plugging in is what raises the drive and no touch
    /// ever occurs to anchor `volumeAppearsAfter` to. One-shot, reset to -1
    /// when it fires, so a post-copy release wait does not see the same drive
    /// eternally re-mounting. -1 means never.
    int volumeAppearsAtTick = -1;
    /// The same, for a SECOND distinct volume ("F:/") -- two boards plugged
    /// in hand-raised, which the serial-less wait must refuse as ambiguous
    /// exactly like the touched wait does. One-shot, like the above.
    int secondVolumeAppearsAtTick = -1;
    /// Ticks since the most recent touch before a SECOND, distinct volume
    /// joins the first -- modelling a second board being plugged in while
    /// the engine is waiting. Only takes effect at or before
    /// `volumeAppearsAfter` (the wait loop stops polling the instant one
    /// volume is seen, so a later-arriving second volume is never observed
    /// by it). -1 means it never happens.
    int secondVolumeAppearsAfter = -1;
    /// Absolute value of the `waitTicks` counter (not relative to any
    /// particular wait) at which the wait is cancelled, as if the user
    /// clicked Cancel. -1 means never. Only meaningful today for a plan with
    /// a single wait before this fires; a test exercising cancellation
    /// across a second wait would need this reworked to be relative, the
    /// same way `volumeAppearsAfter` is relative to `touchTick`.
    int cancelAfterTicks = -1;
    /// Ticks after a successful copy before the just-flashed CPU's volume
    /// disappears -- modelling the real delay between copyToVolume returning
    /// and the RP2040 actually finishing the reboot off mass storage. -1
    /// means it never disappears on its own.
    int unmountAfterTicks = 1;

    /// How many times a volume comes back on its own AFTER unmounting, with
    /// nothing touched -- an RP2040 whose flash was just erased re-enumerates
    /// RPI-RP2 by itself, which is the entire hazard the erase-reboot handling
    /// exists for. 0 (the default) means a flashed CPU that stays flashed.
    ///
    /// Counted in POLLS OF findVolumes(), not in waitTicks, and that is the
    /// point rather than a convenience: the step after an erase calls
    /// findVolumes() with no intervening tick, so a tick-based model can never
    /// produce the state that step actually lands in. `eraseRebootAfterEmptyPolls`
    /// is how many consecutive empty polls precede the return -- 2 puts the
    /// volume there by the time the next step first looks, higher values leave
    /// it still absent so the step has to wait for it.
    int eraseReboots = 0;
    int eraseRebootAfterEmptyPolls = 2;
    /// A SECOND volume joins at the moment the erased CPU's own volume
    /// returns -- a user pressing the other CPU's BOOTSEL button inside that
    /// window. The two are indistinguishable, so this must still refuse.
    bool secondVolumeJoinsEraseReboot = false;

    /// The drive letter a touched CPU comes back on. Distinct from any
    /// "foreign" drive a test pre-mounts, because on real hardware a CPU
    /// entering BOOTSEL while another drive is already mounted ADDS a drive --
    /// it does not take over the existing one. Modelling that faithfully is
    /// what lets these tests exercise a board whose other CPU is already
    /// sitting in BOOTSEL, which is the ordinary state a user reaches for this
    /// app in and which the fixture previously could not express at all.
    static constexpr const char* kTouchVolume = "E:/";

    /// Overridable per test, because a plan that touches one CPU while ANOTHER
    /// CPU's drive is already mounted needs the two to be distinguishable --
    /// they are different drives on real hardware, and a fixture that gives
    /// them the same letter cannot tell a correct write from a wrong-CPU one.
    std::string touchVolume = kTouchVolume;

    std::optional<int> touchTick;   ///< waitTicks value at the last touch
    std::optional<int> copyTick;    ///< waitTicks value at the last copy
    std::string copiedVolume;       ///< the volume of the last copy, for unmounting
    bool anyCopyHappened = false;   ///< nothing reboots before something is written
    int  emptyPolls = 0;            ///< consecutive findVolumes() calls seeing nothing

    FlashIo io() {
        FlashIo io;
        io.identify    = [this] { return identity; };
        io.findVolumes = [this] {
            if (!volumes.empty()) { emptyPolls = 0; return volumes; }
            if (!anyCopyHappened || eraseReboots <= 0) return volumes;
            if (++emptyPolls >= eraseRebootAfterEmptyPolls) {
                volumes = { "E:/" };
                if (secondVolumeJoinsEraseReboot) volumes.push_back("F:/");
                --eraseReboots;
                emptyPolls = 0;
            }
            return volumes;
        };
        io.touchPort   = [this](const std::string& p) {
            touched.push_back(p);
            touchTick = waitTicks;
        };
        io.loadImage   = [this](const ImageRef&)
            -> std::expected<std::vector<uint8_t>, std::string> { return image; };
        io.stageFile   = [](std::span<const uint8_t>, const std::string& name)
            -> std::expected<std::filesystem::path, std::string> {
                return std::filesystem::path("/staged") / name;
            };
        io.copyToVolume = [this](const std::filesystem::path&, const std::string& vol)
            -> std::expected<void, std::string> {
                copiedTo.push_back(vol);
                copyTick = waitTicks;
                copiedVolume = vol;
                anyCopyHappened = true;
                return {};
            };
        io.waitTick = [this](int) {
            ++waitTicks;
            if (cancelAfterTicks >= 0 && waitTicks >= cancelAfterTicks) return false;

            if (volumeAppearsAtTick >= 0 && waitTicks >= volumeAppearsAtTick) {
                if (std::find(volumes.begin(), volumes.end(), touchVolume) == volumes.end())
                    volumes.push_back(touchVolume);
                volumeAppearsAtTick = -1;
            }
            if (secondVolumeAppearsAtTick >= 0 && waitTicks >= secondVolumeAppearsAtTick) {
                if (std::find(volumes.begin(), volumes.end(), "F:/") == volumes.end())
                    volumes.push_back("F:/");
                secondVolumeAppearsAtTick = -1;
            }

            if (copyTick.has_value() && unmountAfterTicks >= 0
                && waitTicks - *copyTick >= unmountAfterTicks) {
                // Only the drive that was written to reboots off mass storage.
                // Clearing every mount here would also spirit away an unrelated
                // CPU's drive, which no copy can do.
                std::erase(volumes, copiedVolume);
                copyTick.reset();
            }

            if (touchTick.has_value()) {
                const int sinceTouch = waitTicks - *touchTick;
                const bool touchDriveUp =
                    std::find(volumes.begin(), volumes.end(), touchVolume) != volumes.end();
                if (volumeAppearsAfter >= 0 && sinceTouch >= volumeAppearsAfter
                    && !touchDriveUp)
                    volumes.push_back(touchVolume);   // ADDS, beside anything already there
                if (secondVolumeAppearsAfter >= 0 && sinceTouch >= secondVolumeAppearsAfter
                    && std::find(volumes.begin(), volumes.end(), "F:/") == volumes.end())
                    volumes.push_back("F:/");
                if (volumeAppearsAfter >= 0 && sinceTouch >= volumeAppearsAfter)
                    touchTick.reset();   // one-shot: this touch's volume has settled
            }

            return true;
        };
        return io;
    }
};

FlashStep step(TargetCpu cpu, std::string sha = "") {
    FlashStep s;
    s.cpu = cpu;
    s.image.embeddedId = "img";
    s.sha256 = std::move(sha);
    s.description = "test step";
    return s;
}

/// A step that ERASES its CPU: the standard Pico flash-erase image. The
/// engine treats the volume that CPU brings back afterwards differently --
/// see the erase-reboot tests at the bottom of this file.
FlashStep eraseStep(TargetCpu cpu) {
    FlashStep s = step(cpu);
    s.action = StepAction::Erase;
    s.description = "test erase step";
    return s;
}

const ProgressFn kNoProgress = [](const FlashProgress&) {};

/// Captures every progress event a run produced, in order. The progress-bar
/// tests at the bottom of this file are about the SEQUENCE of events, not
/// about any one of them, so they need the whole tape.
struct ProgressRecorder {
    std::vector<FlashProgress> events;

    ProgressFn fn() { return [this](const FlashProgress& p) { events.push_back(p); }; }

    /// Every event of one phase, optionally only the periodic refreshes (or
    /// only the one announcement that is not a refresh).
    std::vector<FlashProgress> of(FlashPhase phase, std::optional<bool> refresh = std::nullopt) const
    {
        std::vector<FlashProgress> out;
        for (const auto& e : events)
            if (e.phase == phase && (!refresh.has_value() || e.isRefresh == *refresh))
                out.push_back(e);
        return out;
    }
};

} // namespace

TEST_CASE("the normal path touches the main port and copies") {
    Harness h;
    h.identity.mainPort = "COM60";
    h.identity.mainSource = IdentitySource::HubLocation;

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(r.stepsCompleted == 1);
    REQUIRE(h.touched.size() == 1);
    CHECK(h.touched[0] == "COM60");
    REQUIRE(h.copiedTo.size() == 1);
    CHECK(h.copiedTo[0] == "E:/");
}

TEST_CASE("a two-step plan touches main first, then display") {
    Harness h;
    h.identity.mainPort = "COM60";
    h.identity.displayPort = "COM65";

    std::vector<FlashStep> plan{ step(TargetCpu::Main), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(r.stepsCompleted == 2);
    REQUIRE(h.touched.size() == 2);
    CHECK(h.touched[0] == "COM60");
    CHECK(h.touched[1] == "COM65");
}

TEST_CASE("an unidentified target CPU refuses before touching anything") {
    Harness h;   // no ports identified at all
    std::vector<FlashStep> plan{ step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::RefusedUnidentified);
    CHECK(r.stepsCompleted == 0);
    CHECK(h.touched.empty());
    CHECK(h.copiedTo.empty());
    CHECK(r.message.find("DISPLAY") != std::string::npos);
}

TEST_CASE("two mounted volumes refuse without touching or copying") {
    // Both RP2040s in BOOTSEL present the same bootrom serial, so neither the
    // tools nor the operator can tell them apart.
    //
    // NO PORTS, matching that sentence: a CPU in BOOTSEL publishes mass storage
    // and no CDC port, so a fixture claiming both RP2040s are in BOOTSEL *and*
    // that MAIN is answering on COM60 describes a board that cannot exist. It
    // used to, and the contradiction went unnoticed because nothing consulted
    // the port here. Something does now -- see the companion test below.
    Harness h;
    h.volumes = { "E:/", "F:/" };

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::RefusedAmbiguous);
    CHECK(h.touched.empty());
    CHECK(h.copiedTo.empty());
}

TEST_CASE("mounted volumes do not block a target CPU that is demonstrably running") {
    // The companion to the refusal above, and the whole point of the change: a
    // CPU answering on a serial port is running firmware, so it is NOT any of
    // the drives already mounted -- however many there are, and whoever they
    // belong to. Touch it, and the drive that APPEARS is it.
    Harness h;
    h.identity.mainPort = "COM60";
    h.volumes = { "X:/", "Y:/" };   // somebody else's, and not MAIN's

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(h.touched == std::vector<std::string>{ "COM60" });
    // Written to the drive the touch produced, and emphatically not to either
    // drive that was sitting there beforehand.
    REQUIRE(h.copiedTo.size() == 1);
    CHECK(h.copiedTo[0] == Harness::kTouchVolume);
    CHECK(h.copiedTo[0] != "X:/");
    CHECK(h.copiedTo[0] != "Y:/");
}

TEST_CASE("a foreign mounted volume demands typed confirmation when nothing is running") {
    // The case the prompt was written for, and now the only case it fires in:
    // a drive is mounted and NEITHER CPU is answering, so nothing establishes
    // whose drive it is and the user is the only remaining source of truth.
    Harness h;
    h.volumes = { "E:/" };          // present before we did anything

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::NeedsConfirmation);
    REQUIRE(r.confirmationCpu.has_value());
    CHECK(*r.confirmationCpu == TargetCpu::Main);
    CHECK(h.copiedTo.empty());
    CHECK(h.touched.empty());
}

TEST_CASE("a foreign volume beside a RUNNING target CPU is touched, never confirmed") {
    // The user-facing regression this change exists for: install the display
    // bootloader on a board whose MAIN CPU is sitting in BOOTSEL. DISPLAY is
    // running and answering, so the mounted drive is not DISPLAY's, and the
    // install must simply run -- one click, no prompt, no probe, no ritual.
    Harness h;
    h.identity.displayPort = "COM65";
    h.volumes = { "G:/" };          // the OTHER CPU, sitting in BOOTSEL

    std::vector<FlashStep> plan{ step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(h.touched == std::vector<std::string>{ "COM65" });
    REQUIRE(h.copiedTo.size() == 1);
    CHECK(h.copiedTo[0] == Harness::kTouchVolume);
    CHECK(h.copiedTo[0] != "G:/");   // NOT the drive that was already there
}

TEST_CASE("a bootrom drive located by hub position is written with no touch and no prompt") {
    // The other half of "no matter what firmware is on it": DISPLAY has no
    // working firmware at all, so it publishes no port and there is nothing to
    // touch -- it IS the mounted drive. identifyCpus names that drive from its
    // port on the board's internal hub (see test_fwCpuIdentify.cpp), which
    // arrives here as an ordinary CpuIdentity volume.
    Harness h;
    h.identity.mainPort      = "COM60";           // MAIN running
    h.identity.displayVolume = "G:/";             // DISPLAY in the bootrom, located
    h.identity.displaySource = IdentitySource::HubLocation;
    h.volumes = { "G:/" };

    std::vector<FlashStep> plan{ step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(h.touched.empty());        // nothing to touch: it is already in BOOTSEL
    CHECK(h.copiedTo == std::vector<std::string>{ "G:/" });
}

TEST_CASE("the correct typed confirmation proceeds without touching") {
    // No mainPort: MAIN is the mounted drive, and a CPU in BOOTSEL publishes no
    // serial port. With the other CPU silent too there is no elimination to be
    // had, so this is the genuine confirmation path.
    Harness h;
    h.volumes = { "E:/" };

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "MAIN", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(h.touched.empty());       // it is already in BOOTSEL
    REQUIRE(h.copiedTo.size() == 1);
}

TEST_CASE("the wrong typed confirmation still refuses") {
    // Typing DISPLAY for a main-targeted step is exactly the mistake this
    // guard exists to catch.
    Harness h;
    h.volumes = { "E:/" };

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "DISPLAY", kNoProgress);

    CHECK(r.outcome == FlashOutcome::NeedsConfirmation);
    CHECK(h.copiedTo.empty());
    CHECK(h.touched.empty());
}

TEST_CASE("a sha256 mismatch fails before any copy") {
    Harness h;
    h.identity.mainPort = "COM60";

    std::vector<FlashStep> plan{ step(TargetCpu::Main, "not-the-real-hash") };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::VerifyFailed);
    CHECK(h.copiedTo.empty());
    CHECK(h.touched.empty());
}

TEST_CASE("a matching sha256 passes verification") {
    Harness h;
    h.identity.mainPort = "COM60";
    const std::string real = sha256Hex(h.image);

    std::vector<FlashStep> plan{ step(TargetCpu::Main, real) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
}

TEST_CASE("an empty sha256 deliberately skips verification") {
    // Uf2Asset::sha256 is documented "empty when unknown". A catalog entry
    // without a published hash must still be flashable -- an empty hash is
    // not the same thing as a hash that failed to match.
    Harness h;
    h.identity.mainPort = "COM60";

    std::vector<FlashStep> plan{ step(TargetCpu::Main, "") };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    REQUIRE(h.copiedTo.size() == 1);
}

TEST_CASE("a wrong-family UF2 is refused before any copy") {
    // 0xE48BFF59 is RP2350 ARM-S. Writing it to an OG is the single worst
    // thing a catalog typo could cause.
    Harness h;
    h.identity.mainPort = "COM60";
    h.image = goodUf2(0xE48BFF59);

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::BadImage);
    CHECK(h.copiedTo.empty());
    CHECK(h.touched.empty());
}

TEST_CASE("no volume appearing after the touch is a timeout") {
    Harness h;
    h.identity.mainPort = "COM60";
    h.volumeAppearsAfter = -1;      // never appears

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Timeout);
    CHECK(h.touched.size() == 1);   // we did try
    CHECK(h.copiedTo.empty());
}

TEST_CASE("a second volume appearing mid-wait refuses as ambiguous") {
    // The realistic hazard: a user plugs in a second board while the engine
    // is waiting for the first one's RPI-RP2 volume to appear. This is the
    // in-loop ambiguity check, distinct from the pre-touch classifyVolumes
    // path already covered by "two mounted volumes refuse...".
    Harness h;
    h.identity.mainPort = "COM60";
    h.secondVolumeAppearsAfter = 1;   // joins on the same tick the first one does

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::RefusedAmbiguous);
    CHECK(h.copiedTo.empty());
    CHECK(h.touched.size() == 1);   // the refusal came from the in-wait check,
                                     // not the pre-touch classifyVolumes path
}

TEST_CASE("cancelling the wait aborts without copying") {
    Harness h;
    h.identity.mainPort = "COM60";
    h.volumeAppearsAfter = -1;   // never appears; the user gives up first
    h.cancelAfterTicks = 2;

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Aborted);
    CHECK(h.copiedTo.empty());
    CHECK(h.touched.size() == 1);
}

TEST_CASE("a failed copy reports CopyFailed") {
    Harness h;
    h.identity.mainPort = "COM60";
    auto io = h.io();
    io.copyToVolume = [](const std::filesystem::path&, const std::string&)
        -> std::expected<void, std::string> {
            return std::unexpected("the disk went away");
        };

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(io, plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::CopyFailed);
    CHECK(r.message.find("the disk went away") != std::string::npos);
    CHECK(h.copiedTo.empty());
}

TEST_CASE("a plan aborts on the first failed step and reports how far it got") {
    // A partially applied LegacyDirect plan leaves the board in a mixed state,
    // so the message must name exactly what completed.
    Harness h;
    h.identity.mainPort = "COM60";   // display deliberately unidentified

    std::vector<FlashStep> plan{ step(TargetCpu::Main), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::RefusedUnidentified);
    CHECK(r.stepsCompleted == 1);
    CHECK(h.copiedTo.size() == 1);
}

// ---------------------------------------------------------------------------
// Final review (Fix 2, Critical): what the failure MESSAGE says about what was
// written.
//
// The tests above assert stepsCompleted and copiedTo and never inspect the
// message, which is exactly how this survived: every refusal message was worded
// as though the plan could only ever stop on step 0 ("... so nothing was
// written"), and the same string was then reused verbatim for step 1 of a
// LegacyDirect plan whose step 0 had SUCCEEDED -- destroying the display
// bootloader -- while telling the user nothing had been written at all.
//
// The design spec's requirement is that "the failure message names exactly
// which steps completed", so these assert on the message text directly.
// ---------------------------------------------------------------------------

TEST_CASE("a step-0 refusal still says plainly that nothing was written") {
    Harness h;   // no ports identified at all
    std::vector<FlashStep> plan{ step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    REQUIRE(r.outcome == FlashOutcome::RefusedUnidentified);
    REQUIRE(r.stepsCompleted == 0);
    CHECK(r.message.find("wrote nothing") != std::string::npos);
    // And no partial-install language, which would be its own lie here.
    CHECK(r.message.find("already been written") == std::string::npos);
    CHECK(r.message.find("had already completed") == std::string::npos);
}

TEST_CASE("a mid-plan refusal names the step that completed and the CPU it wrote") {
    // The exact real-world path: Original FreeWili firmware, plan
    // [MAIN, DISPLAY]. Step 0 succeeds -- which is what destroys the display
    // bootloader -- and step 1 refuses because the DISPLAY port could not be
    // identified, the likely outcome since the firmware just installed
    // publishes no FWOG_* product string and a running MAIN CPU suppresses the
    // display console entirely.
    Harness h;
    h.identity.mainPort = "COM60";   // display deliberately unidentified

    std::vector<FlashStep> plan{ step(TargetCpu::Main), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    REQUIRE(r.outcome == FlashOutcome::RefusedUnidentified);
    REQUIRE(r.stepsCompleted == 1);
    REQUIRE(h.copiedTo.size() == 1);   // the MAIN write really did happen

    // The defect verbatim: this message used to read "the DISPLAY CPU could
    // not be identified, so nothing was written."
    CHECK(r.message.find("nothing was written") == std::string::npos);
    // And what it must say instead -- which step, of how many, and which CPU
    // is now carrying firmware it was not carrying a moment ago.
    CHECK(r.message.find("step 1 of 2") != std::string::npos);
    CHECK(r.message.find("MAIN") != std::string::npos);
    CHECK(r.message.find("already been written") != std::string::npos);
    CHECK(r.message.find("Recovery") != std::string::npos);
}

TEST_CASE("a mid-plan release-wait timeout carries the same note") {
    // Not only the refusal path: any stop part-way through a plan leaves the
    // same mixed board, so every failure message has to account for it.
    Harness h;
    h.identity.mainPort = "COM60";
    h.identity.displayPort = "COM65";
    h.unmountAfterTicks = -1;   // MAIN's volume never releases on its own

    std::vector<FlashStep> plan{ step(TargetCpu::Main), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "DISPLAY", kNoProgress);

    REQUIRE(r.outcome == FlashOutcome::Timeout);
    REQUIRE(r.stepsCompleted == 1);
    CHECK(r.message.find("step 1 of 2") != std::string::npos);
    CHECK(r.message.find("already been written") != std::string::npos);
}

TEST_CASE("a mid-plan verify failure names the completed step too") {
    // VerifyFailed's own sentence ("... so it was not written") is scoped to
    // THIS step's image and stays true; what it cannot be allowed to imply is
    // that the board is untouched.
    Harness h;
    h.identity.mainPort = "COM60";
    h.identity.displayPort = "COM65";

    std::vector<FlashStep> plan{ step(TargetCpu::Main),
                                 step(TargetCpu::Display, "not-the-real-hash") };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    REQUIRE(r.outcome == FlashOutcome::VerifyFailed);
    REQUIRE(r.stepsCompleted == 1);
    CHECK(r.message.find("was not written") != std::string::npos);   // this step's image
    CHECK(r.message.find("step 1 of 2") != std::string::npos);        // and what did land
    CHECK(r.message.find("MAIN") != std::string::npos);
}

TEST_CASE("cancelling mid-plan says what was already written, not just 'cancelled'") {
    // Cancel between the MAIN and DISPLAY steps leaves exactly the mixed board
    // the Recovery tab's partial-install section documents.
    Harness h;
    h.identity.mainPort = "COM60";
    h.identity.displayPort = "COM65";
    h.unmountAfterTicks = -1;   // MAIN's volume never releases, so the release
    h.cancelAfterTicks  = 3;    // wait is still running when the user gives up

    std::vector<FlashStep> plan{ step(TargetCpu::Main), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "DISPLAY", kNoProgress);

    REQUIRE(r.outcome == FlashOutcome::Aborted);
    REQUIRE(r.stepsCompleted == 1);
    CHECK(r.message.find("step 1 of 2") != std::string::npos);
    CHECK(r.message.find("MAIN") != std::string::npos);
}

TEST_CASE("a NeedsConfirmation prompt is not turned into a partial-install report") {
    // It is a prompt in the middle of a plan that is still going, not a report
    // on a plan that stopped: appending "See the Recovery tab before retrying"
    // to a message whose whole purpose is "type MAIN and press Proceed" would
    // send the user the wrong way.
    Harness h;
    h.volumes = { "E:/" };   // no ports: the confirmation path, see above

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    REQUIRE(r.outcome == FlashOutcome::NeedsConfirmation);
    CHECK(r.message.find("Recovery") == std::string::npos);
    CHECK(r.message.find("already been written") == std::string::npos);
}

TEST_CASE("a two-step failure of a THREE-step plan names both completed CPUs") {
    // The plural wording, and proof the note is built from the plan rather than
    // from a hardcoded "MAIN then DISPLAY" assumption.
    Harness h;
    h.identity.mainPort = "COM60";
    h.identity.displayPort = "COM65";

    std::vector<FlashStep> plan{ step(TargetCpu::Main),
                                 step(TargetCpu::Display),
                                 step(TargetCpu::Main, "not-the-real-hash") };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    REQUIRE(r.outcome == FlashOutcome::VerifyFailed);
    REQUIRE(r.stepsCompleted == 2);
    CHECK(r.message.find("steps 1-2 of 3") != std::string::npos);
    CHECK(r.message.find("the MAIN and the DISPLAY CPUs have") != std::string::npos);
}

TEST_CASE("a later step does not copy to the previous step's still-mounted volume") {
    // Regression for a wrong-CPU write. copyToVolume() returning success
    // does not mean the RP2040 has finished consuming the file: on real
    // hardware it takes hundreds of milliseconds to seconds to parse the
    // UF2, reboot and drop its mass-storage endpoint. If the engine advanced
    // straight to step 1 without waiting for that, it would see step 0's
    // still-mounted volume, classify it ForeignMounted -- and because a
    // confirmation for the whole plan was supplied up front (a perfectly
    // reasonable Task 19 design for a known LegacyDirect plan), it would
    // copy DISPLAY's image straight onto the MAIN CPU it just finished
    // flashing, with no human pause in between to let the volume unmount.
    Harness h;
    h.identity.mainPort = "COM60";
    h.identity.displayPort = "COM65";
    h.unmountAfterTicks = -1;   // Main's volume never releases on its own

    std::vector<FlashStep> plan{ step(TargetCpu::Main), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "DISPLAY", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Timeout);
    CHECK(r.stepsCompleted == 1);          // Main's write itself did succeed
    REQUIRE(h.copiedTo.size() == 1);
    CHECK(h.copiedTo[0] == "E:/");         // only MAIN was ever written to
    REQUIRE(h.touched.size() == 1);
    CHECK(h.touched[0] == "COM60");        // DISPLAY's port was never touched
}

TEST_CASE("resumable is true after NeedsConfirmation") {
    // This is the one outcome where the failed step itself never touched or
    // copied anything, so resuming at stepsCompleted re-evaluates it fresh.
    Harness h;
    h.volumes = { "E:/" };   // no ports: the confirmation path, see above

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::NeedsConfirmation);
    CHECK(r.resumable == true);
}

TEST_CASE("resumable is false after a release-wait timeout") {
    // The case that matters: step 0's write genuinely succeeded, but by
    // definition of this failure its volume is still mounted. If a caller
    // resumed at stepsCompleted here with a whole-plan confirmation already
    // in hand, step 1 would meet that stale volume and copy to it -- the
    // exact wrong-CPU-write defect closed inside a single call, reappearing
    // across the call boundary. resumable must say no.
    Harness h;
    h.identity.mainPort = "COM60";
    h.identity.displayPort = "COM65";
    h.unmountAfterTicks = -1;   // Main's volume never releases on its own

    std::vector<FlashStep> plan{ step(TargetCpu::Main), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "DISPLAY", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Timeout);
    CHECK(r.resumable == false);
}

TEST_CASE("an image that cannot be loaded reports BadImage") {
    Harness h;
    h.identity.mainPort = "COM60";
    auto io = h.io();
    io.loadImage = [](const ImageRef&)
        -> std::expected<std::vector<uint8_t>, std::string> {
            return std::unexpected("download failed");
        };

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(io, plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::BadImage);
    CHECK(h.touched.empty());
    CHECK(h.copiedTo.empty());
}

TEST_CASE("progress is reported once per step") {
    Harness h;
    h.identity.mainPort = "COM60";
    h.identity.displayPort = "COM65";

    int stepStarts = 0;
    ProgressFn fn = [&](const FlashProgress& p) {
        if (p.phase == FlashPhase::StepStarted) ++stepStarts;
    };

    std::vector<FlashStep> plan{ step(TargetCpu::Main), step(TargetCpu::Display) };
    runFlashPlan(h.io(), plan, "", fn);
    CHECK(stepStarts == 2);
}

TEST_CASE("an empty plan succeeds with nothing done") {
    Harness h;
    auto r = runFlashPlan(h.io(), {}, "", kNoProgress);
    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(r.stepsCompleted == 0);
}

TEST_CASE("startIndex resumes a plan without redoing an earlier step") {
    // The fix for the resume trap: a caller reacting to NeedsConfirmation
    // must be able to resume at the failed step using the SAME, original
    // plan, rather than either replaying step 0 (a real, physical re-flash
    // of hardware that already succeeded) or having to slice the plan
    // itself. stepsCompleted must stay relative to the original plan.
    Harness h;
    h.identity.mainPort = "COM60";
    h.identity.displayPort = "COM65";

    std::vector<FlashStep> plan{ step(TargetCpu::Main), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress, /*startIndex=*/1);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(r.stepsCompleted == 2);          // relative to the original plan
    REQUIRE(h.touched.size() == 1);
    CHECK(h.touched[0] == "COM65");        // Main was never touched this call
}

// ---------------------------------------------------------------------------
// The progress bar: the phase -> fraction mapping, and the live reporting the
// two waits now do while they poll.
//
// What prompted this: a real LegacyDirect run (MAIN then DISPLAY) whose step 1
// succeeded and whose step 2 touched the DISPLAY CPU, entered WaitingForVolume,
// and sat on the single frozen line "waiting for the RPI-RP2 volume" for the
// full thirty-second timeout before failing. Nothing on screen moved, so there
// was no way to tell a working app from a hung one from a nearly-finished one.
// The wait was in fact counting down to a timeout, and nothing said so.
// ---------------------------------------------------------------------------

TEST_CASE("the phase fraction rises along the order phases are REPORTED, not declared") {
    // The trap this exists to close. StepFinished is reported the instant the
    // copy succeeds and WaitingForRelease only afterwards, but FlashPhase
    // DECLARES them the other way round -- so anything using the enumerator as
    // an ordinal makes the bar run backwards on every step but the last.
    REQUIRE(static_cast<int>(FlashPhase::WaitingForRelease)
            < static_cast<int>(FlashPhase::StepFinished));
    CHECK(flashPhaseFraction(FlashPhase::WaitingForRelease)
          > flashPhaseFraction(FlashPhase::StepFinished));

    const std::vector<FlashPhase> reported{
        FlashPhase::StepStarted, FlashPhase::Loading, FlashPhase::Verifying,
        FlashPhase::Touching, FlashPhase::WaitingForVolume, FlashPhase::Copying,
        FlashPhase::StepFinished, FlashPhase::WaitingForRelease };
    for (size_t k = 1; k < reported.size(); ++k)
        CHECK(flashPhaseFraction(reported[k]) > flashPhaseFraction(reported[k - 1]));

    CHECK(flashPhaseFraction(FlashPhase::StepStarted) == 0.0f);
    // Short of 1.0 on purpose: only a Success result completes the bar.
    CHECK(flashPhaseFraction(FlashPhase::WaitingForRelease) < 1.0f);
}

TEST_CASE("the overall fraction places a step's own progress inside the whole plan") {
    FlashProgress p;
    p.stepCount = 2;

    p.stepIndex = 0;
    p.phase = FlashPhase::StepStarted;
    CHECK(flashProgressFraction(p) == doctest::Approx(0.0));

    p.stepIndex = 1;
    p.phase = FlashPhase::StepStarted;
    CHECK(flashProgressFraction(p) == doctest::Approx(0.5));   // step 1 of 2 begins at half

    p.phase = FlashPhase::StepFinished;
    CHECK(flashProgressFraction(p) == doctest::Approx(0.925)); // 0.5 + 0.85/2
    CHECK(flashProgressFraction(p) < 1.0f);
}

TEST_CASE("the overall fraction cannot leave 0..1 for a degenerate report") {
    FlashProgress empty;
    empty.stepCount = 0;   // an empty plan reports nothing, but a bar must not divide by it
    CHECK(flashProgressFraction(empty) == 0.0f);

    FlashProgress over;
    over.stepCount = 2;
    over.stepIndex = 7;    // impossible from runFlashPlan; clamped, not trusted
    over.phase = FlashPhase::WaitingForRelease;
    CHECK(flashProgressFraction(over) == doctest::Approx(0.95));
    CHECK(flashProgressFraction(over) <= 1.0f);
}

TEST_CASE("the overall fraction never goes backwards across a whole two-step plan") {
    Harness h;
    h.identity.mainPort = "COM60";
    h.identity.displayPort = "COM65";

    ProgressRecorder rec;
    std::vector<FlashStep> plan{ step(TargetCpu::Main), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", rec.fn());

    REQUIRE(r.outcome == FlashOutcome::Success);
    // Both steps' worth of phases, plus the release wait's reports.
    REQUIRE(rec.events.size() > 12);

    float previous = -1.0f;
    for (const auto& e : rec.events) {
        const float f = flashProgressFraction(e);
        CHECK(f >= previous);
        previous = f;
    }
    // And it genuinely travelled, rather than the loop above passing because
    // nothing ever moved.
    CHECK(flashProgressFraction(rec.events.front()) == 0.0f);
    CHECK(previous > 0.9f);
}

TEST_CASE("a resumed plan picks the bar up where it left off instead of restarting at zero") {
    // stepIndex/stepCount stay relative to the ORIGINAL plan across a resume
    // (see runFlashPlan's comment), which is the whole reason a resumed flash
    // can keep its bar. Resuming step 1 of 2 must start the bar at half, not at
    // nothing -- and must not overshoot either.
    Harness h;
    h.identity.mainPort = "COM60";
    h.identity.displayPort = "COM65";

    ProgressRecorder rec;
    std::vector<FlashStep> plan{ step(TargetCpu::Main), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", rec.fn(), /*startIndex=*/1);

    REQUIRE(r.outcome == FlashOutcome::Success);
    REQUIRE(!rec.events.empty());
    CHECK(flashProgressFraction(rec.events.front()) == doctest::Approx(0.5));

    float previous = -1.0f;
    for (const auto& e : rec.events) {
        const float f = flashProgressFraction(e);
        CHECK(f >= previous);
        CHECK(f <= 1.0f);
        previous = f;
    }
    CHECK(previous > 0.9f);
}

TEST_CASE("a wait that has to poll reports itself more than once") {
    Harness h;
    h.identity.mainPort = "COM60";
    h.volumeAppearsAfter = 5;   // five polls before the volume turns up

    ProgressRecorder rec;
    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", rec.fn());
    REQUIRE(r.outcome == FlashOutcome::Success);

    // Exactly one announcement -- the phase transition, the line the scrolling
    // log keeps.
    const auto announced = rec.of(FlashPhase::WaitingForVolume, /*refresh=*/false);
    REQUIRE(announced.size() == 1);
    CHECK(announced[0].waitElapsedMs == 0);
    CHECK_FALSE(announced[0].message.empty());

    // ...and the live updates that used to not exist at all.
    const auto refreshes = rec.of(FlashPhase::WaitingForVolume, /*refresh=*/true);
    REQUIRE(refreshes.size() >= 4);
    CHECK(refreshes.front().waitElapsedMs == kVolumePollMs);
    for (size_t k = 1; k < refreshes.size(); ++k)
        CHECK(refreshes[k].waitElapsedMs > refreshes[k - 1].waitElapsedMs);

    for (const auto& e : refreshes) {
        // Every refresh carries the wait's total, so a countdown can be drawn
        // from it, and repeats the announcement's text, so a consumer showing
        // only the newest event never has to remember an earlier one.
        CHECK(e.waitTotalMs == kVolumeWaitMs);
        CHECK(e.message == announced[0].message);
        CHECK(e.cpu == announced[0].cpu);
        CHECK(e.stepCount == announced[0].stepCount);
    }
}

TEST_CASE("the wait that used to freeze for thirty seconds now reports right up to the timeout") {
    // The failure verbatim: the DISPLAY volume never appeared. Before this,
    // that produced ONE progress event and thirty seconds of silence.
    Harness h;
    h.identity.mainPort = "COM60";
    h.volumeAppearsAfter = -1;

    ProgressRecorder rec;
    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", rec.fn());
    REQUIRE(r.outcome == FlashOutcome::Timeout);

    const auto refreshes = rec.of(FlashPhase::WaitingForVolume, /*refresh=*/true);
    CHECK(refreshes.size() == static_cast<size_t>(kVolumeWaitMs / kVolumePollMs));
    CHECK(refreshes.back().waitElapsedMs == kVolumeWaitMs);   // the countdown reached the limit
}

TEST_CASE("the overall bar does not creep forward while a wait counts down") {
    // The honesty requirement. A wait is bounded, but its END is unpredictable:
    // the volume can appear on the first poll or never. Advancing the OVERALL
    // bar as the wait's clock runs down would claim the flash was getting
    // closer to finishing when what it was getting closer to was a timeout.
    // The wait's own elapsed/total is a fact and is reported separately; this
    // one stays put.
    Harness h;
    h.identity.mainPort = "COM60";
    h.volumeAppearsAfter = -1;

    ProgressRecorder rec;
    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", rec.fn());
    REQUIRE(r.outcome == FlashOutcome::Timeout);

    const auto waiting = rec.of(FlashPhase::WaitingForVolume);
    REQUIRE(waiting.size() > 100);   // the announcement plus every refresh
    const float frozen = flashProgressFraction(waiting.front());
    for (const auto& e : waiting)
        CHECK(flashProgressFraction(e) == frozen);
    // ...and the elapsed count really did move, so the check above is not
    // passing because nothing happened.
    CHECK(waiting.back().waitElapsedMs > waiting.front().waitElapsedMs);
}

TEST_CASE("the release wait reports live too, not only the volume wait") {
    // Both waits share one implementation on purpose (see
    // waitForVolumeChange's comment), so neither can be left behind.
    Harness h;
    h.identity.mainPort = "COM60";
    h.identity.displayPort = "COM65";
    h.unmountAfterTicks = 4;   // MAIN's volume takes four polls to go away

    ProgressRecorder rec;
    std::vector<FlashStep> plan{ step(TargetCpu::Main), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", rec.fn());
    REQUIRE(r.outcome == FlashOutcome::Success);

    CHECK(rec.of(FlashPhase::WaitingForRelease, /*refresh=*/false).size() == 1);
    const auto refreshes = rec.of(FlashPhase::WaitingForRelease, /*refresh=*/true);
    REQUIRE(refreshes.size() >= 3);
    CHECK(refreshes.front().waitElapsedMs == kVolumePollMs);
    CHECK(refreshes.back().waitElapsedMs > refreshes.front().waitElapsedMs);
    CHECK(refreshes.back().waitTotalMs == kVolumeWaitMs);
}

TEST_CASE("a cancelled wait stops reporting on the tick it is cancelled") {
    // Cancellation must stay responsive: the refresh is emitted AFTER the
    // io.waitTick() that can report a cancel, so the cancelling tick produces
    // no further progress and the wait returns immediately.
    Harness h;
    h.identity.mainPort = "COM60";
    h.volumeAppearsAfter = -1;
    h.cancelAfterTicks = 2;   // the second tick is the one that cancels

    ProgressRecorder rec;
    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", rec.fn());
    REQUIRE(r.outcome == FlashOutcome::Aborted);

    const auto refreshes = rec.of(FlashPhase::WaitingForVolume, /*refresh=*/true);
    REQUIRE(refreshes.size() == 1);                          // the first tick only
    CHECK(refreshes.front().waitElapsedMs == kVolumePollMs);
    CHECK(h.waitTicks == 2);                                  // and it stopped right there
}

TEST_CASE("a wait satisfied by its very first poll emits no refresh at all") {
    // Nothing was waited for, so there is nothing to count down. The typed-
    // confirmation path proceeds against an already-mounted volume and never
    // enters the volume wait; the single-step plan has no release wait either.
    Harness h;
    h.volumes = { "E:/" };   // no ports: the confirmation path, see above

    ProgressRecorder rec;
    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "MAIN", rec.fn());
    REQUIRE(r.outcome == FlashOutcome::Success);

    for (const auto& e : rec.events) {
        CHECK_FALSE(e.isRefresh);
        CHECK(e.waitTotalMs == 0);   // no phase here is a bounded wait
    }
}

// ---------------------------------------------------------------------------
// The erase-reboot path (DISPLAY-first reordering, item 6).
//
// The deprecated-firmware plan is now [ERASE DISPLAY, WRITE DISPLAY, WRITE
// MAIN]. An RP2040 whose flash has just been erased re-enumerates RPI-RP2 BY
// ITSELF, with no button and no 1200-baud touch, so step 2 arrives to find
// either no volume yet or a volume nobody touched. Before this handling
// existed, the first refused (RefusedUnidentified -- an erased CPU has no
// serial port) and the second demanded a typed DISPLAY confirmation in the
// middle of a plan working exactly as designed.
// ---------------------------------------------------------------------------

TEST_CASE("the step after an erase writes to the volume the erased CPU brings back, with no touch") {
    // The whole plan, end to end, with no confirmation supplied at all -- the
    // point being that none is needed.
    Harness h;
    h.identity.displayPort = "COM65";
    h.identity.mainPort    = "COM60";
    h.eraseReboots = 1;   // the erased DISPLAY CPU returns once, on its own

    std::vector<FlashStep> plan{ eraseStep(TargetCpu::Display),
                                 step(TargetCpu::Display),
                                 step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(r.stepsCompleted == 3);
    REQUIRE(h.copiedTo.size() == 3);
    // Two touches, not three: the erase step touched DISPLAY to get it into
    // BOOTSEL, the write step after it needed no touch because the erase had
    // already put it there, and the MAIN step touched MAIN.
    REQUIRE(h.touched.size() == 2);
    CHECK(h.touched[0] == "COM65");
    CHECK(h.touched[1] == "COM60");
}

TEST_CASE("the step after an erase waits for the volume rather than refusing when it is not back yet") {
    // The failure this closes: the erased CPU has no firmware and therefore no
    // serial port, so the old NoneMounted path would have called it
    // unidentified and refused -- failing the plan for doing exactly what it
    // was told to do.
    Harness h;
    // DISPLAY deliberately NOT identified for the second step: after the
    // erase there is genuinely no port there any more.
    h.identity.displayPort = "COM65";
    h.eraseReboots = 1;
    h.eraseRebootAfterEmptyPolls = 4;   // still absent when the step first looks

    auto io = h.io();
    bool erased = false;
    const auto baseCopy = io.copyToVolume;
    io.identify = [&h, &erased] {
        CpuIdentity id = h.identity;
        if (erased) id.displayPort.reset();   // the port is gone once erased
        return id;
    };
    io.copyToVolume = [baseCopy, &erased](const std::filesystem::path& p, const std::string& v) {
        erased = true;
        return baseCopy(p, v);
    };

    std::vector<FlashStep> plan{ eraseStep(TargetCpu::Display), step(TargetCpu::Display) };
    auto r = runFlashPlan(io, plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(r.stepsCompleted == 2);
    REQUIRE(h.copiedTo.size() == 2);
    REQUIRE(h.touched.size() == 1);   // only the erase step touched anything
}

TEST_CASE("the erase reboot exemption does not carry over to a step on the OTHER CPU") {
    // The exemption is "this volume is the CPU this plan erased one step ago",
    // not "a volume appeared and we are feeling relaxed". A following step on
    // a DIFFERENT CPU meets the ordinary guard.
    Harness h;
    h.identity.displayPort = "COM65";
    h.identity.mainPort    = "COM60";
    h.eraseReboots = 1;
    // MAIN's drive must be distinguishable from the drive the erased DISPLAY
    // CPU brings back, or this test cannot tell the right write from the wrong
    // one -- which is the only thing it is here to check.
    h.touchVolume = "T:/";
    h.eraseRebootAfterEmptyPolls = 1;   // DISPLAY is back before the MAIN step looks

    std::vector<FlashStep> plan{ eraseStep(TargetCpu::Display), step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    // The erased DISPLAY CPU's volume is sitting there when the MAIN step
    // looks. The property under test is that MAIN's image NEVER lands on it --
    // that is the wrong-CPU write this exemption boundary exists to prevent.
    //
    // How that is achieved has changed, and for the better: MAIN is answering
    // on COM60, so MAIN is provably not the erased CPU's drive, and the step
    // touches MAIN and writes to the drive MAIN itself brings up. It used to
    // stop and ask the user to type "MAIN" -- a prompt that protected nothing
    // here, since the drive in front of it was the wrong CPU either way and a
    // user who typed what they were asked for got the bad write anyway.
    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(h.touched == std::vector<std::string>{ "COM65", "COM60" });
    REQUIRE(h.copiedTo.size() == 2);   // the erase, then MAIN's own drive
    // THE assertion: MAIN's image did not land on the drive the erased DISPLAY
    // CPU brought back ("E:/" is the erase-reboot drive in this fixture; "T:/"
    // is the drive a touched CPU raises).
    CHECK(h.copiedTo[1] == "T:/");
    CHECK(h.copiedTo[1] != "E:/");
}

TEST_CASE("the erase reboot exemption lasts exactly one step") {
    // Three steps on the same CPU: erase, write, write. The exemption applies
    // to the step directly after the erase and to nothing else.
    //
    // The boundary is now visible in the TOUCHES rather than in a prompt, and
    // that is a sharper statement of the same property: step 1 is exempt, so it
    // waits for the erased CPU to come back on its own and touches nothing;
    // step 2 is not exempt, so it goes through the ordinary guard, which for a
    // CPU that is answering means touching it. An exemption that leaked into
    // step 2 would show up here as a missing second touch.
    Harness h;
    h.identity.displayPort = "COM65";   // DISPLAY answers: that is how step 0 erases it
    h.eraseReboots = 1;

    std::vector<FlashStep> plan{ eraseStep(TargetCpu::Display),
                                 step(TargetCpu::Display),
                                 step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    REQUIRE(h.copiedTo.size() == 3);
    // Step 0 touched to get into BOOTSEL; step 1 did NOT (the exemption); step
    // 2 did (no exemption left).
    CHECK(h.touched == std::vector<std::string>{ "COM65", "COM65" });
}

TEST_CASE("a foreign volume still demands confirmation when no erase preceded the step") {
    // The general ForeignMounted guard is untouched. This is the case it was
    // written for -- a board a user left in BOOTSEL by hand -- and a plan that
    // merely CONTAINS an erase later on does not soften it.
    //
    // No displayPort: the mounted drive is DISPLAY sitting in BOOTSEL, which
    // publishes no CDC port, and MAIN is silent too -- so nothing can establish
    // whose drive it is and the prompt is the honest answer.
    Harness h;
    h.volumes = { "E:/" };   // there before we did anything at all

    std::vector<FlashStep> plan{ eraseStep(TargetCpu::Display), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::NeedsConfirmation);
    CHECK(r.stepsCompleted == 0);
    CHECK(h.copiedTo.empty());
    CHECK(h.touched.empty());
}

TEST_CASE("an erased CPU that never comes back is a timeout, not a silent skip") {
    Harness h;
    h.identity.displayPort = "COM65";
    h.eraseReboots = 0;   // it never returns

    std::vector<FlashStep> plan{ eraseStep(TargetCpu::Display), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Timeout);
    CHECK(r.stepsCompleted == 1);
    REQUIRE(h.copiedTo.size() == 1);
    CHECK(r.message.find("erased") != std::string::npos);
    CHECK(r.message.find("DISPLAY") != std::string::npos);
}

TEST_CASE("an erased CPU is given far longer to return than a touched one") {
    // The regression this pins is not hypothetical: it shipped, and it cost a
    // real board. The erase-reboot wait used to run on kVolumeWaitMs, whose
    // 30 s is justified by "a touch works in about a second or it did not
    // work". An erase is not a touch -- flash_nuke erases the whole flash chip
    // before resetting, measured on real hardware at just over 60 seconds --
    // so the deprecated-firmware install erased the DISPLAY CPU, waited half
    // as long as the hardware needed, and reported that the CPU never came
    // back. It had; the app stopped looking about thirty seconds early.
    //
    // Counted in POLLS, because that is the clock the engine's budget is
    // measured against: kVolumePollMs each, so kVolumeWaitMs is 120 polls and
    // kEraseRebootWaitMs is 600.
    const int touchBudgetPolls = kVolumeWaitMs / kVolumePollMs;
    const int eraseBudgetPolls = kEraseRebootWaitMs / kVolumePollMs;
    REQUIRE(eraseBudgetPolls > touchBudgetPolls);   // the whole point

    SUBCASE("a return that would have missed the touch budget now succeeds") {
        Harness h;
        h.identity.displayPort = "COM65";
        h.eraseReboots = 1;
        // Comfortably past the old budget, comfortably inside the new one --
        // and inside the ~62 s the hardware actually takes.
        h.eraseRebootAfterEmptyPolls = touchBudgetPolls + 80;

        std::vector<FlashStep> plan{ eraseStep(TargetCpu::Display), step(TargetCpu::Display) };
        auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

        CHECK(r.outcome == FlashOutcome::Success);
        CHECK(h.copiedTo.size() == 2);   // the erase, then the image it exists to allow
    }

    SUBCASE("but the wait is still bounded -- it does not simply wait forever") {
        Harness h;
        h.identity.displayPort = "COM65";
        h.eraseReboots = 1;
        h.eraseRebootAfterEmptyPolls = eraseBudgetPolls + 10;   // past even the new budget

        std::vector<FlashStep> plan{ eraseStep(TargetCpu::Display), step(TargetCpu::Display) };
        auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

        CHECK(r.outcome == FlashOutcome::Timeout);
        CHECK(h.copiedTo.size() == 1);   // only the erase
    }
}

TEST_CASE("the countdown a user watches belongs to the wait actually running") {
    // waitTotalMs is what the dialog draws its determinate bar against. If the
    // erase wait reported the touch budget, the bar would fill in 30 s and
    // then sit at 100% for another half-minute while the wait was still
    // legitimately running -- precise about the wrong number, which is worse
    // than vague.
    Harness h;
    h.identity.displayPort = "COM65";
    h.eraseReboots = 1;
    h.eraseRebootAfterEmptyPolls = 8;

    int erasePhaseTotal = 0;
    std::vector<FlashStep> plan{ eraseStep(TargetCpu::Display), step(TargetCpu::Display) };
    runFlashPlan(h.io(), plan, "", [&](const FlashProgress& p) {
        if (p.phase == FlashPhase::WaitingForVolume &&
            p.message.find("erased") != std::string::npos)
            erasePhaseTotal = p.waitTotalMs;
    });

    CHECK(erasePhaseTotal == kEraseRebootWaitMs);
}

TEST_CASE("a second volume during the erase-reboot wait refuses as ambiguous") {
    // A hand-pressed BOOTSEL landing inside the window in which the erased CPU
    // is returning is exactly what the two-volume refusal is for, and it must
    // still fire on this path.
    Harness h;
    h.identity.displayPort = "COM65";
    h.eraseReboots = 1;
    h.eraseRebootAfterEmptyPolls = 4;   // so the step is inside the WAIT when it happens
    h.secondVolumeJoinsEraseReboot = true;

    std::vector<FlashStep> plan{ eraseStep(TargetCpu::Display), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::RefusedAmbiguous);
    REQUIRE(h.copiedTo.size() == 1);   // only the erase; nothing was guessed at
    // "appeared", not "are mounted": the in-wait check, i.e. the refusal came
    // from inside the erase-reboot wait rather than from the guard before it.
    CHECK(r.message.find("volumes appeared") != std::string::npos);
}

TEST_CASE("a stop after the erase step says the CPU was erased, not merely written") {
    // "the DISPLAY CPU has already been written" is true of an erase (a UF2
    // really was copied) but would let a user picture firmware sitting there.
    Harness h;
    h.identity.displayPort = "COM65";
    h.eraseReboots = 1;

    std::vector<FlashStep> plan{ eraseStep(TargetCpu::Display),
                                 step(TargetCpu::Display, "not-the-real-hash") };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    REQUIRE(r.outcome == FlashOutcome::VerifyFailed);
    REQUIRE(r.stepsCompleted == 1);
    CHECK(r.message.find("step 1 of 2") != std::string::npos);
    CHECK(r.message.find("ERASED") != std::string::npos);
    CHECK(r.message.find("blank flash") != std::string::npos);
}

TEST_CASE("a resumed call does not inherit an erase-reboot exemption it never observed") {
    // The flag lives in one runFlashPlan call and is never persisted into
    // FlashResult. A caller resuming at step 1 of [ERASE DISPLAY, WRITE
    // DISPLAY] did not watch that erase happen in THIS call, so the volume it
    // finds is, as far as this call can prove, just a mounted volume.
    //
    // No displayPort, for the same reason as the test above: the drive IS the
    // DISPLAY CPU in BOOTSEL, so it cannot also be answering on a serial port.
    Harness h;
    h.volumes = { "E:/" };

    std::vector<FlashStep> plan{ eraseStep(TargetCpu::Display), step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress, /*startIndex=*/1);

    CHECK(r.outcome == FlashOutcome::NeedsConfirmation);
    CHECK(h.copiedTo.empty());
}

TEST_CASE("an out-of-range startIndex is rejected, not silently clamped") {
    // startIndex > plan.size() cannot come from any real stepsCompleted --
    // it is unambiguously a caller bug, so it must not be reported as a
    // completed (or benign no-op) flash.
    Harness h;
    h.identity.mainPort = "COM60";

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress, /*startIndex=*/5);

    CHECK(r.outcome == FlashOutcome::InvalidArgument);
    CHECK(h.touched.empty());
    CHECK(h.copiedTo.empty());
    CHECK(r.message.find("5") != std::string::npos);
    CHECK(r.message.find("1") != std::string::npos);   // the actual plan size
}

// ---------------------------------------------------------------------------
// A verified CPU probe mapping.
//
// The state these are all about is the one the board owner was stuck in: a CPU
// sitting in the RP2040's bootrom, presenting an RPI-RP2 volume, with no serial
// port to identify it by and therefore -- before this -- no way to write to it
// that was not a typed confirmation the app had no business asking for, or a
// flat refusal.
//
// The mapping reaches the engine through CpuIdentity's volume fields, filled in
// by withVerifiedVolume() (fwDeviceModel.h) from a result verifiedVolumeFrom()
// has already found fresh. Here it is set directly, which is what an injected
// I/O layer is for.
// ---------------------------------------------------------------------------

TEST_CASE("a probed volume is written directly: no touch, no confirmation") {
    Harness h;
    h.volumes = { "G:/" };
    // No port for MAIN, on purpose -- a CPU in its bootloader has none, and
    // that absence is precisely what used to make this unreachable.
    h.identity.mainVolume = "G:/";
    h.identity.mainSource = IdentitySource::VerifiedProbe;

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    // Empty confirmation: if the engine were still routing this through the
    // typed-confirmation path, this would stop at NeedsConfirmation.
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(r.stepsCompleted == 1);
    REQUIRE(h.copiedTo.size() == 1);
    CHECK(h.copiedTo[0] == "G:/");
    // Nothing was rebooted. There is nothing running on that CPU to reboot,
    // and it is already where a reboot would have put it.
    CHECK(h.touched.empty());
}

TEST_CASE("elimination works: identifying one CPU's volume authorises the OTHER") {
    // The mechanism the whole recovery flow rests on. The prober was written to
    // one of two volumes and reported "display"; the volume still mounted is
    // therefore MAIN, and MAIN is what may be written to it. Nothing measured
    // MAIN directly -- it was named by elimination -- and that is enough,
    // because a FreeWili OG has exactly two CPUs.
    Harness h;
    h.volumes = { "G:/" };
    h.identity = withVerifiedVolume(CpuIdentity{}, VerifiedVolume{ "G:/", TargetCpu::Main });
    // The prober's own CPU is visible as an ordinary port, which is how the
    // board actually looks at this point -- and it must not be mistaken for
    // the mapping.
    h.identity.displayPort = "COM69";
    h.identity.displaySource = IdentitySource::HubLocation;

    // It named it MAIN, not DISPLAY: a plan for the MAIN CPU goes straight to
    // the drive...
    std::vector<FlashStep> mainPlan{ step(TargetCpu::Main) };
    auto ok = runFlashPlan(h.io(), mainPlan, "", kNoProgress);
    CHECK(ok.outcome == FlashOutcome::Success);
    REQUIRE(h.copiedTo.size() == 1);
    CHECK(h.copiedTo[0] == "G:/");
    CHECK(h.touched.empty());
}

TEST_CASE("a mapping for one CPU does not authorise a write to the other") {
    // NO DISPLAY PORT: with the drive known to be MAIN's and DISPLAY nowhere to
    // be found, there is genuinely nowhere for a DISPLAY image to go, and the
    // app says so rather than asking. (When DISPLAY IS answering the answer is
    // different and better -- see the companion test below.)
    Harness h;
    h.volumes = { "G:/" };
    h.identity = withVerifiedVolume(CpuIdentity{}, VerifiedVolume{ "G:/", TargetCpu::Main });

    std::vector<FlashStep> plan{ step(TargetCpu::Display) };
    // Typing the CPU name must not help either: this is not a confirmation
    // prompt, it is a refusal.
    auto r = runFlashPlan(h.io(), plan, "DISPLAY", kNoProgress);

    CHECK(r.outcome == FlashOutcome::RefusedWrongCpu);
    CHECK(r.stepsCompleted == 0);
    CHECK(h.copiedTo.empty());
    CHECK(h.touched.empty());
    CHECK_FALSE(r.resumable);
    // The refusal names the drive and both CPUs, so the user can act on it.
    CHECK(r.message.find("G:/") != std::string::npos);
    CHECK(r.message.find("MAIN") != std::string::npos);
    CHECK(r.message.find("DISPLAY") != std::string::npos);

    // AND THE REMEDY ADDRESSES THIS STEP'S CPU, not the one whose drive
    // happens to be mounted. The message used to end "Flash the MAIN CPU
    // first, or unmount that volume", which reads as actionable and is not:
    // flashing MAIN is a different plan, and unmounting leaves this step with
    // no drive AND no port, refusing again one branch over. A user who
    // followed it arrived at a second error having changed nothing that
    // mattered. What unblocks this step is putting ITS CPU into the bootloader.
    CHECK(r.message.find("Put the DISPLAY CPU into its bootloader") != std::string::npos);
    // The DISPLAY procedure specifically -- it has no button, so "hold the red
    // button" would send the user looking for one that does not exist.
    CHECK(r.message.find("BOOTSEL pad") != std::string::npos);
    CHECK(r.message.find("red button") == std::string::npos);
    // And it says the state the user is being sent into is legitimate, because
    // it looks like it should not be: two RPI-RP2 drives at once used to be
    // exactly what this app refused over, and hub position is why it no longer
    // is. Without this a cautious user stops here.
    CHECK(r.message.find("Both CPUs being in BOOTSEL at once is fine") != std::string::npos);
    // The advice that was wrong is gone, not merely supplemented.
    CHECK(r.message.find("unmount that volume") == std::string::npos);
}

TEST_CASE("both CPUs in the bootrom flash to their own drives, with no probe and no prompt") {
    // The state that used to be the app's dead end -- two RPI-RP2 drives, both
    // CPUs silent -- run end to end. Each step writes to the drive on ITS hub
    // port, and the pair being mounted simultaneously is not an ambiguity.
    //
    // The wrong-CPU assertion is the important one: DISPLAY's image on H:/ and
    // MAIN's on G:/, never crossed, and never to whatever happened to be
    // mounted first.
    Harness h;
    h.volumes = { "G:/", "H:/" };
    h.identity.mainVolume    = "G:/";
    h.identity.mainSource    = IdentitySource::HubLocation;
    h.identity.displayVolume = "H:/";
    h.identity.displaySource = IdentitySource::HubLocation;
    h.unmountAfterTicks = -1;   // bootrom drives that stay put between steps

    std::vector<FlashStep> plan{ step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(h.touched.empty());
    CHECK(h.copiedTo == std::vector<std::string>{ "H:/" });

    Harness m;
    m.volumes = h.volumes;
    m.identity = h.identity;
    m.unmountAfterTicks = -1;
    std::vector<FlashStep> mainPlan{ step(TargetCpu::Main) };
    auto rm = runFlashPlan(m.io(), mainPlan, "", kNoProgress);

    CHECK(rm.outcome == FlashOutcome::Success);
    CHECK(m.copiedTo == std::vector<std::string>{ "G:/" });
}

TEST_CASE("the other CPU's known drive does not block a DISPLAY install that can reach DISPLAY") {
    // The user-facing case, stated at the engine: MAIN is sitting in BOOTSEL
    // with its drive mounted and identified, and the user asks to install the
    // display bootloader. DISPLAY is answering, so DISPLAY is reachable and
    // MAIN's drive is simply none of this step's business.
    //
    // Knowing whose the drive is must make the app MORE capable, not less. The
    // safety property is unchanged and is asserted below: DISPLAY's image never
    // lands on G:/.
    Harness h;
    h.volumes = { "G:/" };
    h.identity = withVerifiedVolume(CpuIdentity{}, VerifiedVolume{ "G:/", TargetCpu::Main });
    h.identity.displayPort = "COM69";
    h.touchVolume = "T:/";   // the drive DISPLAY raises, distinct from MAIN's

    std::vector<FlashStep> plan{ step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(h.touched == std::vector<std::string>{ "COM69" });
    CHECK(h.copiedTo == std::vector<std::string>{ "T:/" });
    CHECK(h.copiedTo[0] != "G:/");   // never onto MAIN's drive
}

TEST_CASE("a stale mapping authorises nothing: the volume set moved") {
    // The invalidation rule, at the engine. The mapping names G:/ and G:/ is
    // not what is mounted -- the board was replugged and got another letter --
    // so it is not in play at all and the ordinary foreign-volume rule applies.
    Harness h;
    h.volumes = { "H:/" };
    h.identity = withVerifiedVolume(CpuIdentity{}, VerifiedVolume{ "G:/", TargetCpu::Main });

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::NeedsConfirmation);
    CHECK(h.copiedTo.empty());
    CHECK(h.touched.empty());
}

TEST_CASE("a mapping does not make two mounted volumes flashable") {
    // The regression guarantee, restated at the engine with a mapping present:
    // a mapping established against one volume says nothing about a second one
    // that has since appeared beside it, so this refuses exactly as it always
    // did -- with a matching mapping and a matching typed confirmation, neither
    // of which may make any difference.
    //
    // No mainPort: MAIN is claimed by the mapping to BE one of these drives, so
    // it is in BOOTSEL and publishes nothing. (A fixture asserting both would
    // now take the touch path -- correctly, since a running MAIN is neither
    // drive -- and would be testing something else entirely. That case is
    // covered by "mounted volumes do not block a target CPU that is
    // demonstrably running".)
    Harness h;
    h.volumes = { "G:/", "H:/" };
    h.identity = withVerifiedVolume(CpuIdentity{}, VerifiedVolume{ "G:/", TargetCpu::Main });

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "MAIN", kNoProgress);

    CHECK(r.outcome == FlashOutcome::RefusedAmbiguous);
    CHECK(h.copiedTo.empty());
    CHECK(h.touched.empty());
}

TEST_CASE("a probed write says in the log why it needed no confirmation") {
    // A write that skipped both the touch and the typed confirmation must state
    // its reason in the record the user reads afterwards, or an image appears
    // on a drive with nothing explaining how it was allowed to.
    Harness h;
    h.volumes = { "G:/" };
    h.identity = withVerifiedVolume(CpuIdentity{}, VerifiedVolume{ "G:/", TargetCpu::Display });

    ProgressRecorder rec;
    std::vector<FlashStep> plan{ step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", rec.fn());

    CHECK(r.outcome == FlashOutcome::Success);
    const auto copying = rec.of(FlashPhase::Copying);
    REQUIRE(copying.size() == 1);
    CHECK(copying[0].message.find("G:/") != std::string::npos);
    CHECK(copying[0].message.find("CPU probe") != std::string::npos);
    CHECK(copying[0].message.find("DISPLAY") != std::string::npos);
}

TEST_CASE("a mapping only covers the step whose volume it names") {
    // A two-step plan where the mapping answers the FIRST step and must not be
    // stretched to the second: after step 1's volume releases, nothing is
    // mounted, so step 2 falls back to the ordinary touch-the-port path. This
    // is the deprecated-firmware shape once its redundant erase is dropped.
    Harness h;
    h.volumes = { "G:/" };
    h.identity = withVerifiedVolume(CpuIdentity{}, VerifiedVolume{ "G:/", TargetCpu::Display });
    h.identity.mainPort = "COM60";

    std::vector<FlashStep> plan{ step(TargetCpu::Display), step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(r.stepsCompleted == 2);
    REQUIRE(h.copiedTo.size() == 2);
    CHECK(h.copiedTo[0] == "G:/");   // straight to the probed drive
    CHECK(h.copiedTo[1] == "E:/");   // the volume the touch below produced
    // Exactly one touch, and it belongs to the step the mapping said nothing
    // about.
    REQUIRE(h.touched.size() == 1);
    CHECK(h.touched[0] == "COM60");
}

// ---------------------------------------------------------------------------
// The identify wait. io.identify() is live in production and a step routinely
// begins while the board is mid-transition (the previous step rebooted a CPU;
// the DISPLAY bootloader's console takes ~10 s to appear after MAIN goes
// quiet), so a refusal is only final once it has held for kIdentifyWaitMs.
// The fixture's identity is a plain value the test mutates from waitTick(),
// which is exactly the shape of a board that changes under the engine.
// ---------------------------------------------------------------------------

TEST_CASE("a CPU that becomes reachable during the identify wait is written, not refused") {
    // Nothing identified at first; the DISPLAY console appears after a few
    // ticks (a MAIN erase just happened, and the bootloader's 10-second rule
    // is running). Before the wait existed this refused on the first look.
    Harness h;
    int ticksBeforeConsole = 6;
    FlashIo io = h.io();
    const auto tick = io.waitTick;
    io.waitTick = [&](int ms) {
        const bool r = tick(ms);
        if (--ticksBeforeConsole == 0) h.identity.displayPort = "COM65";
        return r;
    };
    ProgressRecorder rec;

    std::vector<FlashStep> plan{ step(TargetCpu::Display) };
    auto r = runFlashPlan(io, plan, "", rec.fn());

    CHECK(r.outcome == FlashOutcome::Success);
    REQUIRE(h.touched.size() == 1);
    CHECK(h.touched[0] == "COM65");
    // It said what it was doing while it waited: one announcement, refreshes after.
    CHECK(rec.of(FlashPhase::WaitingForCpu, false).size() == 1);
    CHECK_FALSE(rec.of(FlashPhase::WaitingForCpu, true).empty());
    CHECK(rec.of(FlashPhase::WaitingForCpu).front().message.find("DISPLAY") != std::string::npos);
}

TEST_CASE("a CPU that never becomes reachable is refused only after the whole identify budget") {
    Harness h;
    std::vector<FlashStep> plan{ step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::RefusedUnidentified);
    CHECK(h.waitTicks == kIdentifyWaitMs / kVolumePollMs);
    CHECK(h.touched.empty());
    CHECK(h.copiedTo.empty());
    CHECK(r.message.find(std::to_string(kIdentifyWaitMs / 1000)) != std::string::npos);
}

TEST_CASE("a cancel during the identify wait aborts cleanly with nothing written") {
    Harness h;
    h.cancelAfterTicks = 3;
    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Aborted);
    CHECK(r.stepsCompleted == 0);
    CHECK(h.waitTicks == 3);
    CHECK(h.copiedTo.empty());
}

TEST_CASE("the wrong CPU's drive being the only one mounted is refused, after waiting for ours") {
    // Hub position says the one drive is DISPLAY's; MAIN has no port and never
    // gets one. RefusedWrongCpu -- but only once MAIN has had its chance.
    Harness h;
    h.volumes = { "G:/" };
    h.identity.displayVolume = "G:/";
    h.identity.displaySource = IdentitySource::HubLocation;
    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::RefusedWrongCpu);
    CHECK(h.waitTicks == kIdentifyWaitMs / kVolumePollMs);
    CHECK(h.copiedTo.empty());
}

// ---------------------------------------------------------------------------
// Causation checked against structure: an arrival the live identity places at
// the OTHER CPU's hub port is not the touched CPU's drive, however good its
// timing. The real shape: ERASE MAIN, then touch DISPLAY -- and MAIN's blank
// flash re-enumerates its own drive a second later, inside the window.
// ---------------------------------------------------------------------------

TEST_CASE("an arrival hub-located at the other CPU is not taken for the touched CPU's drive") {
    Harness h;
    h.identity.displayPort = "COM65";
    // The MAIN drive comes back on its own 1 tick after the touch, at MAIN's
    // hub port; the DISPLAY's own drive follows 2 ticks later.
    FlashIo io = h.io();
    const auto tick = io.waitTick;
    int sinceTouch = -1;
    io.touchPort = [&](const std::string& p) { h.touched.push_back(p); sinceTouch = 0; };
    io.waitTick = [&](int ms) {
        const bool r = tick(ms);
        if (sinceTouch >= 0) {
            ++sinceTouch;
            if (sinceTouch == 1) {
                h.volumes.push_back("F:/");           // MAIN, blank, back on its own
                h.identity.mainVolume = "F:/";
                h.identity.mainSource = IdentitySource::HubLocation;
            }
            if (sinceTouch == 3) {
                h.volumes.push_back("G:/");           // the DISPLAY we touched
                h.identity.displayPort.reset();
                h.identity.displayVolume = "G:/";
                h.identity.displaySource = IdentitySource::HubLocation;
            }
        }
        return r;
    };

    std::vector<FlashStep> plan{ step(TargetCpu::Display) };
    auto r = runFlashPlan(io, plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    REQUIRE(h.copiedTo.size() == 1);
    CHECK(h.copiedTo[0] == "G:/");   // NOT F:/, which arrived first
}

TEST_CASE("two simultaneous arrivals are resolved when structure names the touched CPU's drive") {
    Harness h;
    h.identity.displayPort = "COM65";
    FlashIo io = h.io();
    const auto tick = io.waitTick;
    int sinceTouch = -1;
    io.touchPort = [&](const std::string& p) { h.touched.push_back(p); sinceTouch = 0; };
    io.waitTick = [&](int ms) {
        const bool r = tick(ms);
        if (sinceTouch >= 0 && ++sinceTouch == 1) {
            h.volumes.push_back("F:/");
            h.volumes.push_back("G:/");
            h.identity.mainVolume = "F:/";     h.identity.mainSource = IdentitySource::HubLocation;
            h.identity.displayPort.reset();
            h.identity.displayVolume = "G:/";  h.identity.displaySource = IdentitySource::HubLocation;
        }
        return r;
    };

    std::vector<FlashStep> plan{ step(TargetCpu::Display) };
    auto r = runFlashPlan(io, plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    REQUIRE(h.copiedTo.size() == 1);
    CHECK(h.copiedTo[0] == "G:/");
}

TEST_CASE("two simultaneous arrivals with no structure to tell them apart are still refused") {
    Harness h;
    h.identity.displayPort = "COM65";
    h.secondVolumeAppearsAfter = 1;   // the fixture's own two-arrival model, identity silent on drives
    std::vector<FlashStep> plan{ step(TargetCpu::Display) };
    auto r = runFlashPlan(h.io(), plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::RefusedAmbiguous);
    CHECK(h.copiedTo.empty());
}

// ---------------------------------------------------------------------------
// The serial-less arm of RefuseUnidentified -- iPadOS, where no port exists to
// touch and the USER raising the drive by hand is the reboot. Reachable here
// because serial availability is a datum on FlashIo (see its comment in
// fwFlashEngine.h) rather than a compile-time constant: every one of these
// cases runs the exact control flow the iPad runs.
// ---------------------------------------------------------------------------

TEST_CASE("a serial-less platform waits for a hand-raised drive instead of refusing") {
    Harness h;   // no ports identified, nothing mounted
    auto io = h.io();
    io.serialSupportAvailable = false;
    h.volumeAppearsAtTick = 3;   // the user plugs in, red button held

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(io, plan, "", kNoProgress);

    // The arrived drive still carries no identity, so the typed confirmation
    // is the guard -- the wait must NOT write on arrival alone.
    CHECK(r.outcome == FlashOutcome::NeedsConfirmation);
    REQUIRE(r.confirmationCpu.has_value());
    CHECK(*r.confirmationCpu == TargetCpu::Main);
    CHECK(r.resumable);
    CHECK(h.touched.empty());    // there is no port to have touched
    CHECK(h.copiedTo.empty());
}

TEST_CASE("the typed confirmation lets a serial-less flash write the arrived drive") {
    Harness h;
    auto io = h.io();
    io.serialSupportAvailable = false;
    h.volumeAppearsAtTick = 3;

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(io, plan, "MAIN", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Success);
    CHECK(h.touched.empty());
    REQUIRE(h.copiedTo.size() == 1);
    CHECK(h.copiedTo[0] == Harness::kTouchVolume);
}

TEST_CASE("the serial-less wait skips the identify hold and reports the volume wait at once") {
    // The identify hold's "waiting for the CPU to become reachable" is about a
    // serial port; on a platform with no ports it would be twenty silent
    // seconds in front of the one instruction that matters. The exemption in
    // isRefusal is what this pins: the FIRST progress phase must be the volume
    // wait carrying the red-button instruction, and WaitingForCpu must never
    // be announced at all.
    Harness h;
    auto io = h.io();
    io.serialSupportAvailable = false;
    h.volumeAppearsAtTick = 2;

    ProgressRecorder rec;
    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    runFlashPlan(io, plan, "MAIN", rec.fn());

    CHECK(rec.of(FlashPhase::WaitingForCpu).empty());
    const auto waits = rec.of(FlashPhase::WaitingForVolume);
    REQUIRE_FALSE(waits.empty());
    CHECK(waits.front().message.find("red button") != std::string::npos);
}

TEST_CASE("two hand-raised drives at once are refused as ambiguous on a serial-less platform") {
    Harness h;
    auto io = h.io();
    io.serialSupportAvailable = false;
    h.volumeAppearsAtTick = 2;
    h.secondVolumeAppearsAtTick = 2;

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(io, plan, "MAIN", kNoProgress);

    CHECK(r.outcome == FlashOutcome::RefusedAmbiguous);
    CHECK(h.copiedTo.empty());
}

TEST_CASE("a serial-less wait with no arrival times out with the red-button remedy") {
    Harness h;
    auto io = h.io();
    io.serialSupportAvailable = false;   // nothing ever appears

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(io, plan, "", kNoProgress);

    // Timeout, not RefusedUnidentified: the desktop refusal's "no serial port
    // to reboot" diagnosis and Recovery-tab remedy are about machinery this
    // platform does not have. What the user can actually do is in the message.
    CHECK(r.outcome == FlashOutcome::Timeout);
    CHECK(r.message.find("red button") != std::string::npos);
    CHECK(h.copiedTo.empty());
}

TEST_CASE("cancelling the serial-less wait aborts without writing") {
    Harness h;
    auto io = h.io();
    io.serialSupportAvailable = false;
    h.cancelAfterTicks = 2;

    std::vector<FlashStep> plan{ step(TargetCpu::Main) };
    auto r = runFlashPlan(io, plan, "", kNoProgress);

    CHECK(r.outcome == FlashOutcome::Aborted);
    CHECK(h.copiedTo.empty());
}
