#include <doctest/doctest.h>
#include "flash/fwVolumeState.h"
#include "device/fwDeviceModel.h"   // withVerifiedVolume

#include <string>
#include <vector>

using namespace fwog;

namespace {

/// A CpuIdentity that names no volume for either CPU -- what every caller has
/// until somebody runs the Recovery tab's CPU identification, and what the
/// pre-existing cases below are all about. Spelled as a named function rather
/// than a bare `{}` at each call site so that "no mapping is in play" is
/// something these tests SAY rather than something a reader has to infer from
/// an empty brace pair.
CpuIdentity noMapping() { return CpuIdentity{}; }

/// A CpuIdentity carrying a verified probe result: `volume` is `cpu`.
CpuIdentity mapping(const char* volume, TargetCpu cpu)
{
    return withVerifiedVolume(CpuIdentity{}, VerifiedVolume{ volume, cpu });
}

} // namespace

TEST_CASE("no volumes mounted") {
    std::vector<std::string> v;
    CHECK(classifyVolumes(v, /*weTouched=*/false, /*expectedFromPriorErase=*/false, noMapping(), TargetCpu::Main)
          == VolumeState::NoneMounted);
}

TEST_CASE("one volume that we created by touching") {
    std::vector<std::string> v{ "E:/" };
    CHECK(classifyVolumes(v, /*weTouched=*/true, /*expectedFromPriorErase=*/false, noMapping(), TargetCpu::Main)
          == VolumeState::OursAfterTouch);
}

TEST_CASE("one volume that was already there") {
    std::vector<std::string> v{ "E:/" };
    CHECK(classifyVolumes(v, /*weTouched=*/false, /*expectedFromPriorErase=*/false, noMapping(), TargetCpu::Main)
          == VolumeState::ForeignMounted);
}

TEST_CASE("two volumes are ambiguous even if we touched") {
    // Both RP2040s in BOOTSEL present the same bootrom serial, so neither the
    // tools nor the operator can tell them apart (wiliOGBsp facts.md 15).
    std::vector<std::string> v{ "E:/", "F:/" };
    CHECK(classifyVolumes(v, /*weTouched=*/true,  /*expectedFromPriorErase=*/false, noMapping(), TargetCpu::Main)
          == VolumeState::Ambiguous);
    CHECK(classifyVolumes(v, /*weTouched=*/false, /*expectedFromPriorErase=*/false, noMapping(), TargetCpu::Main)
          == VolumeState::Ambiguous);
}

// ---------------------------------------------------------------------------
// expectedFromPriorErase: the step immediately before this one, in this same
// plan, erased THIS SAME CPU and its volume was then observed to go away. Only
// runFlashPlan() may pass it, and only under exactly those conditions.
// ---------------------------------------------------------------------------

TEST_CASE("after an erase of this CPU, one mounted volume is expected rather than foreign") {
    // Without this, the deprecated-firmware plan's step 2 would demand a typed
    // DISPLAY confirmation in the middle of a plan doing exactly what it was
    // asked to do -- training the user to type confirmations without reading
    // them, which every other guard here depends on them not doing.
    std::vector<std::string> v{ "E:/" };
    CHECK(classifyVolumes(v, /*weTouched=*/false, /*expectedFromPriorErase=*/true, noMapping(), TargetCpu::Main)
          == VolumeState::ExpectedAfterErase);
}

TEST_CASE("after an erase of this CPU, NO volume yet is also expected, not a refusal") {
    // The erased CPU takes a real and variable amount of time to wipe its
    // flash and re-enumerate, so "not back yet" and "back already" are two
    // frames of one event. Answering NoneMounted for the first would send the
    // step down the touch-or-refuse path, and a CPU whose firmware this plan
    // just destroyed has no port left to touch.
    std::vector<std::string> v;
    CHECK(classifyVolumes(v, /*weTouched=*/false, /*expectedFromPriorErase=*/true, noMapping(), TargetCpu::Main)
          == VolumeState::ExpectedAfterErase);
}

TEST_CASE("an expected erase reboot does not excuse a second volume") {
    // Knowing why ONE volume is here says nothing about why a SECOND one is.
    // Ambiguity still dominates.
    std::vector<std::string> v{ "E:/", "F:/" };
    CHECK(classifyVolumes(v, /*weTouched=*/false, /*expectedFromPriorErase=*/true, noMapping(), TargetCpu::Main)
          == VolumeState::Ambiguous);
}

// ---------------------------------------------------------------------------
// A verified CPU probe naming the mounted volume. The mapping is carried in
// CpuIdentity's volume fields (see withVerifiedVolume, fwDeviceModel.h); this
// layer re-checks that the mounted set is EXACTLY the one volume it names
// before acting on it, and falls back to the pre-existing states otherwise.
// ---------------------------------------------------------------------------

TEST_CASE("the one mounted volume, probed as this step's CPU, is not foreign") {
    std::vector<std::string> v{ "G:/" };
    CHECK(classifyVolumes(v, /*weTouched=*/false, /*expectedFromPriorErase=*/false,
                          mapping("G:/", TargetCpu::Main), TargetCpu::Main)
          == VolumeState::MappedToTarget);
    // ...and specifically NOT the state that would demand a typed confirmation
    // for a question that has already been answered by measurement.
    CHECK(classifyVolumes(v, false, false, mapping("G:/", TargetCpu::Main), TargetCpu::Main)
          != VolumeState::ForeignMounted);
}

TEST_CASE("the one mounted volume, probed as the OTHER CPU, refuses rather than prompts") {
    // The half that makes this a net tightening. Before the probe existed this
    // case reached RequireTypedConfirmation, and a user who typed the CPU name
    // they were asked for got a wrong-CPU write.
    std::vector<std::string> v{ "G:/" };
    CHECK(classifyVolumes(v, false, false, mapping("G:/", TargetCpu::Main), TargetCpu::Display)
          == VolumeState::MappedToOtherCpu);
    CHECK(classifyVolumes(v, false, false, mapping("G:/", TargetCpu::Display), TargetCpu::Main)
          == VolumeState::MappedToOtherCpu);
}

TEST_CASE("a mapping does not excuse a second volume") {
    // The regression guarantee, at this layer: a mapping was established
    // against ONE volume and says nothing whatsoever about a second one that
    // has appeared beside it. Ambiguity dominates the mapping exactly as it
    // dominates an expected erase reboot.
    std::vector<std::string> v{ "G:/", "H:/" };
    CHECK(classifyVolumes(v, false, false, mapping("G:/", TargetCpu::Main), TargetCpu::Main)
          == VolumeState::Ambiguous);
    CHECK(classifyVolumes(v, false, false, mapping("G:/", TargetCpu::Main), TargetCpu::Display)
          == VolumeState::Ambiguous);
}

TEST_CASE("a mapping whose volume is no longer mounted authorises nothing") {
    // Fail closed on every way the volume set can stop being the one the
    // mapping was established against.
    std::vector<std::string> gone;
    CHECK(classifyVolumes(gone, false, false, mapping("G:/", TargetCpu::Main), TargetCpu::Main)
          == VolumeState::NoneMounted);

    // A DIFFERENT letter is mounted -- the board was replugged and Windows
    // handed out another one. The mapping names G:/ and G:/ is not here, so it
    // is not in play and the ordinary foreign-volume rule applies.
    std::vector<std::string> other{ "H:/" };
    CHECK(classifyVolumes(other, false, false, mapping("G:/", TargetCpu::Main), TargetCpu::Main)
          == VolumeState::ForeignMounted);
}

TEST_CASE("an expected erase reboot still wins over a mapping") {
    // Ordering, stated as a test: the plan's own knowledge of what it just did
    // to this CPU is about the volume in front of it right now, whereas a
    // mapping was established before the plan started.
    std::vector<std::string> v{ "G:/" };
    CHECK(classifyVolumes(v, false, /*expectedFromPriorErase=*/true,
                          mapping("G:/", TargetCpu::Display), TargetCpu::Main)
          == VolumeState::ExpectedAfterErase);
}

TEST_CASE("a mapping is consulted ahead of weTouched") {
    // They cannot disagree through runFlashPlan() today (it never passes
    // weTouched), which is exactly why the ordering is decided in
    // classifyVolumes rather than left to a call site that happens not to
    // exercise it: if a touch of MAIN's port ever produced a volume a probe
    // measured to be DISPLAY, the honest answer is the refusal.
    std::vector<std::string> v{ "G:/" };
    CHECK(classifyVolumes(v, /*weTouched=*/true, false,
                          mapping("G:/", TargetCpu::Display), TargetCpu::Main)
          == VolumeState::MappedToOtherCpu);
    CHECK(classifyVolumes(v, /*weTouched=*/true, false,
                          mapping("G:/", TargetCpu::Main), TargetCpu::Main)
          == VolumeState::MappedToTarget);
}

TEST_CASE("a hub-located drive survives a second drive being mounted") {
    // Both CPUs in BOOTSEL: two RPI-RP2 drives, identical bootrom serials, and
    // -- before hub position was read -- the canonical unresolvable state.
    //
    // Hub position is re-derived from the live USB tree every scan, so a second
    // drive appearing somewhere else cannot falsify "this drive is on the port
    // the MAIN CPU is soldered to". Both are located, and each step goes to its
    // own CPU with no prober and no prompt.
    CpuIdentity id;
    id.mainVolume    = "G:/";
    id.mainSource    = IdentitySource::HubLocation;
    id.displayVolume = "H:/";
    id.displaySource = IdentitySource::HubLocation;

    std::vector<std::string> v{ "G:/", "H:/" };
    CHECK(classifyVolumes(v, false, false, id, TargetCpu::Main)    == VolumeState::MappedToTarget);
    CHECK(classifyVolumes(v, false, false, id, TargetCpu::Display) == VolumeState::MappedToTarget);
}

TEST_CASE("a PROBED mapping is still voided by a second drive") {
    // The asymmetry is the point. A probe result is a measurement taken at a
    // moment, against a mounted set that has since changed; it says nothing
    // about the drive that has appeared beside it. Only the structural source
    // is exempt, and this is the test that keeps the exemption from leaking.
    CpuIdentity id;
    id.mainVolume = "G:/";
    id.mainSource = IdentitySource::VerifiedProbe;

    std::vector<std::string> two{ "G:/", "H:/" };
    CHECK(classifyVolumes(two, false, false, id, TargetCpu::Main) == VolumeState::Ambiguous);

    // ...and is honoured again the moment it is the only thing mounted.
    std::vector<std::string> one{ "G:/" };
    CHECK(classifyVolumes(one, false, false, id, TargetCpu::Main) == VolumeState::MappedToTarget);
}

TEST_CASE("a hub-located drive that is no longer mounted is not acted on") {
    // Structural or not, it is still checked against what is actually there.
    // The board was unplugged and came back on another letter.
    CpuIdentity id;
    id.mainVolume = "G:/";
    id.mainSource = IdentitySource::HubLocation;

    std::vector<std::string> v{ "H:/", "K:/" };
    CHECK(classifyVolumes(v, false, false, id, TargetCpu::Main) == VolumeState::Ambiguous);
}

// Shorthands for decideAction's two port facts, so each call below reads as
// the board state it describes rather than as a pair of bare booleans.
namespace {
constexpr bool kTargetRunning = true;    // the target CPU publishes a serial port
constexpr bool kTargetSilent  = false;
constexpr bool kOtherRunning  = true;    // the other CPU publishes a serial port
constexpr bool kOtherSilent   = false;
} // namespace

TEST_CASE("normal path: no volume and an identified port means touch") {
    auto a = decideAction(VolumeState::NoneMounted, kTargetRunning, kOtherSilent);
    CHECK(a == GuardAction::TouchThenWait);
}

TEST_CASE("no volume and no identified port refuses") {
    auto a = decideAction(VolumeState::NoneMounted, kTargetSilent, kOtherSilent);
    CHECK(a == GuardAction::RefuseUnidentified);
    // The other CPU running changes nothing: elimination names a drive, and
    // with nothing mounted there is no drive to name.
    CHECK(decideAction(VolumeState::NoneMounted, kTargetSilent, kOtherRunning)
          == GuardAction::RefuseUnidentified);
}

TEST_CASE("a volume we created proceeds") {
    CHECK(decideAction(VolumeState::OursAfterTouch, kTargetRunning, kOtherSilent)
          == GuardAction::Proceed);
    CHECK(decideAction(VolumeState::OursAfterTouch, kTargetSilent, kOtherSilent)
          == GuardAction::Proceed);
}

TEST_CASE("a running target CPU is touched, not confirmed, whatever is already mounted") {
    // THE one-click case, and the regression this guards: the other CPU is
    // sitting in BOOTSEL with its drive mounted while the target CPU is running
    // its firmware. The target is therefore not that drive, so there is nothing
    // to disambiguate -- touch the port and take the drive that appears.
    //
    // This used to be RequireTypedConfirmation, which made an ordinary install
    // impossible to complete without the user asserting something the app could
    // work out itself, and whose only correct answer was to refuse.
    CHECK(decideAction(VolumeState::ForeignMounted, kTargetRunning, kOtherSilent)
          == GuardAction::TouchThenWait);
    CHECK(decideAction(VolumeState::ForeignMounted, kTargetRunning, kOtherRunning)
          == GuardAction::TouchThenWait);
}

TEST_CASE("a silent target CPU is located by hub position, not by elimination") {
    // A CPU sitting in the bootrom is identified upstream of this function, by
    // its port on the board's internal hub (see identifyCpus), which reaches
    // here as MappedToTarget -- NOT as a ForeignMounted drive to be reasoned
    // about from who else is running.
    //
    // ForeignMounted with a silent target therefore stays the prompt. An
    // elimination arm here would be both redundant and unsound: it reasons
    // about CPUs and not about boards, so with two FreeWilis attached it would
    // claim a drive that may belong to the other one.
    CHECK(decideAction(VolumeState::ForeignMounted, kTargetSilent, kOtherRunning)
          == GuardAction::RequireTypedConfirmation);
    CHECK(decideAction(VolumeState::MappedToTarget, kTargetSilent, kOtherRunning)
          == GuardAction::WriteMappedVolume);
}

TEST_CASE("a foreign volume with NEITHER CPU running still demands typed confirmation") {
    // The case the prompt was actually written for, and the only one it still
    // fires in: nothing running anywhere, so no elimination is available and
    // the mount really does carry no evidence of whose it is.
    CHECK(decideAction(VolumeState::ForeignMounted, kTargetSilent, kOtherSilent)
          == GuardAction::RequireTypedConfirmation);
}

TEST_CASE("two volumes refuse unless the target is demonstrably not either of them") {
    // Neither CPU running: both drives are candidates, they present identical
    // bootrom serials, and nothing can separate them. Unchanged refusal.
    CHECK(decideAction(VolumeState::Ambiguous, kTargetSilent, kOtherSilent)
          == GuardAction::RefuseAmbiguous);
    CHECK(decideAction(VolumeState::Ambiguous, kTargetSilent, kOtherRunning)
          == GuardAction::RefuseAmbiguous);
    // Target running: it is neither of the mounted drives, so they are not
    // rival candidates at all -- they are somebody else's. Touch, and identify
    // ours by the arrival the touch causes.
    CHECK(decideAction(VolumeState::Ambiguous, kTargetRunning, kOtherSilent)
          == GuardAction::TouchThenWait);
    CHECK(decideAction(VolumeState::Ambiguous, kTargetRunning, kOtherRunning)
          == GuardAction::TouchThenWait);
}

TEST_CASE("an expected erase reboot waits, and does not care about port identification") {
    // Not gated on portIdentified, deliberately: the CPU this plan just erased
    // has no firmware and so publishes no serial port, by construction. If
    // this consulted identification it would refuse every single time.
    CHECK(decideAction(VolumeState::ExpectedAfterErase, kTargetRunning, kOtherSilent)
          == GuardAction::WaitForEraseReboot);
    CHECK(decideAction(VolumeState::ExpectedAfterErase, kTargetSilent, kOtherSilent)
          == GuardAction::WaitForEraseReboot);
    CHECK(decideAction(VolumeState::ExpectedAfterErase, kTargetSilent, kOtherRunning)
          == GuardAction::WaitForEraseReboot);
    // And it is emphatically not a typed-confirmation prompt, nor a plain
    // Proceed that would skip waiting for a volume that has not arrived yet.
    CHECK(decideAction(VolumeState::ExpectedAfterErase, kTargetSilent, kOtherSilent)
          != GuardAction::RequireTypedConfirmation);
    CHECK(decideAction(VolumeState::ExpectedAfterErase, kTargetSilent, kOtherSilent)
          != GuardAction::Proceed);
}

TEST_CASE("a probed volume is written directly, whatever the port list says") {
    // Not gated on portIdentified, and it must not be: a CPU sitting in the
    // RP2040 bootrom publishes no serial port at all, so requiring one here
    // would refuse every single time -- in exactly the situation the probe
    // exists to resolve.
    CHECK(decideAction(VolumeState::MappedToTarget, kTargetRunning, kOtherSilent)
          == GuardAction::WriteMappedVolume);
    CHECK(decideAction(VolumeState::MappedToTarget, kTargetSilent, kOtherSilent)
          == GuardAction::WriteMappedVolume);
    // And emphatically not a confirmation prompt for a question that has been
    // answered, nor a touch of a port that does not exist.
    CHECK(decideAction(VolumeState::MappedToTarget, kTargetSilent, kOtherSilent)
          != GuardAction::RequireTypedConfirmation);
    CHECK(decideAction(VolumeState::MappedToTarget, kTargetRunning, kOtherSilent)
          != GuardAction::TouchThenWait);
}

TEST_CASE("a volume known to be the other CPU refuses only when this CPU is unreachable") {
    // Silent target: the drive in front of us is the other CPU and ours is
    // nowhere to be found. A positive refusal, never a prompt -- the question
    // has been answered and the answer is no.
    CHECK(decideAction(VolumeState::MappedToOtherCpu, kTargetSilent, kOtherSilent)
          == GuardAction::RefuseWrongCpu);
    CHECK(decideAction(VolumeState::MappedToOtherCpu, kTargetSilent, kOtherRunning)
          == GuardAction::RefuseWrongCpu);
    CHECK(decideAction(VolumeState::MappedToOtherCpu, kTargetSilent, kOtherSilent)
          != GuardAction::RequireTypedConfirmation);

    // Running target: knowing whose the mounted drive is does not mean this
    // step has to care about it. Ours is answering, so touch it and take the
    // drive it raises. Refusing here would block an install for no better
    // reason than the OTHER CPU being in BOOTSEL.
    CHECK(decideAction(VolumeState::MappedToOtherCpu, kTargetRunning, kOtherSilent)
          == GuardAction::TouchThenWait);
    CHECK(decideAction(VolumeState::MappedToOtherCpu, kTargetRunning, kOtherRunning)
          == GuardAction::TouchThenWait);
}

TEST_CASE("typed confirmation accepts only the exact CPU name") {
    CHECK(confirmationMatches("MAIN",    TargetCpu::Main));
    CHECK(confirmationMatches("DISPLAY", TargetCpu::Display));
    CHECK_FALSE(confirmationMatches("MAIN",    TargetCpu::Display));
    CHECK_FALSE(confirmationMatches("DISPLAY", TargetCpu::Main));
    CHECK_FALSE(confirmationMatches("",        TargetCpu::Main));
    CHECK_FALSE(confirmationMatches("yes",     TargetCpu::Main));
}

TEST_CASE("typed confirmation ignores case and surrounding whitespace") {
    // Typing the CPU name is the positive act; punishing a stray space or a
    // lowercase 'm' teaches the user to paste rather than read.
    CHECK(confirmationMatches("  main  ", TargetCpu::Main));
    CHECK(confirmationMatches("Display",  TargetCpu::Display));
}

TEST_CASE("typed confirmation rejects whitespace-only and short-form input") {
    // This function is the last check before writing to a volume whose CPU
    // could not be identified. Anything short of the full CPU name must fail.
    CHECK_FALSE(confirmationMatches("   ",  TargetCpu::Main));
    CHECK_FALSE(confirmationMatches("\t\n", TargetCpu::Main));
    CHECK_FALSE(confirmationMatches("y",    TargetCpu::Main));
    CHECK_FALSE(confirmationMatches("Y",    TargetCpu::Display));
}

TEST_CASE("typed confirmation rejects partial and padded CPU names") {
    // A substring, a superstring, and a name with embedded whitespace are all
    // near-misses a hurried user could produce. None may pass.
    CHECK_FALSE(confirmationMatches("MAI",     TargetCpu::Main));
    CHECK_FALSE(confirmationMatches("MAINX",   TargetCpu::Main));
    CHECK_FALSE(confirmationMatches("MA IN",   TargetCpu::Main));
    CHECK_FALSE(confirmationMatches("DISPLA",  TargetCpu::Display));
    CHECK_FALSE(confirmationMatches("DISPLAYS",TargetCpu::Display));
}
