#include "flash/fwVolumeState.h"

#include <algorithm>
#include <cctype>

namespace fwog {

VolumeState classifyVolumes(std::span<const std::string> volumes, bool weTouched,
                            bool expectedFromPriorErase,
                            const CpuIdentity& identity, TargetCpu target)
{
    const auto mounted = [&volumes](const std::string& v) {
        return std::find(volumes.begin(), volumes.end(), v) != volumes.end();
    };
    // A STRUCTURAL location survives a second volume; a measured one does not.
    //
    // Hub position is re-derived from the live USB tree on every scan: it says
    // "this drive is enumerated on this board's own hub, at the port the MAIN
    // CPU is soldered to". A second drive appearing elsewhere cannot make that
    // false, and it is not a rival candidate -- it is a different drive at a
    // different port. So two RP2040s both sitting in BOOTSEL, the case this
    // whole file was built around as unresolvable, resolves: they publish the
    // identical bootrom serial, but they are not in the same place.
    const auto structural = [&identity](TargetCpu cpu) {
        const auto& vol = volumeForCpu(identity, cpu);
        const auto src = cpu == TargetCpu::Main ? identity.mainSource : identity.displaySource;
        return vol && src == IdentitySource::HubLocation;
    };
    if (structural(target) && mounted(*volumeForCpu(identity, target)))
        return VolumeState::MappedToTarget;

    // Ambiguity next, and ahead of everything else on purpose: "we know why ONE
    // volume is here" says nothing at all about why a SECOND one is. That
    // applies word for word to a VERIFIED PROBE mapping below -- it was
    // established against one volume, at a moment now past, and a second one
    // appearing beside it is precisely the change that voids it. The structural
    // case above is exempt for the reason given there, and only that case.
    if (volumes.size() >= 2) return VolumeState::Ambiguous;

    // Both the empty and the single-volume case answer ExpectedAfterErase,
    // and that is the point rather than an oversight. The erased CPU takes a
    // real, variable amount of time to wipe its flash and re-enumerate, so
    // "not back yet" and "back already" are two frames of the same event.
    // Answering NoneMounted for the first would send the step down the
    // touch-or-refuse path, and there is no port left to touch on a CPU whose
    // firmware this plan just destroyed -- the plan would refuse itself for
    // doing exactly what it was asked to do.
    if (expectedFromPriorErase) return VolumeState::ExpectedAfterErase;

    if (volumes.empty()) return VolumeState::NoneMounted;

    // Exactly one volume from here down.
    //
    // The mapping is consulted BEFORE weTouched, deliberately. If the two ever
    // disagreed -- we touched the MAIN port and the volume that appeared is one
    // a probe measured to be the DISPLAY CPU -- the honest answer is the
    // refusal, not the cheerful "we made this one, proceed". They cannot
    // disagree through runFlashPlan() today (it never passes weTouched), and
    // that is exactly why the ordering has to be decided here rather than left
    // to a call site that happens not to exercise it.
    const std::string& only = volumes.front();
    if (const auto& mine = volumeForCpu(identity, target); mine && *mine == only)
        return VolumeState::MappedToTarget;
    if (const auto& theirs = volumeForCpu(identity, otherCpu(target)); theirs && *theirs == only)
        return VolumeState::MappedToOtherCpu;

    return weTouched ? VolumeState::OursAfterTouch : VolumeState::ForeignMounted;
}

GuardAction decideAction(VolumeState state, bool portIdentified, bool otherPortIdentified)
{
    // otherPortIdentified is read by no arm below, and that is the ANSWER this
    // function now gives rather than an oversight: the elimination arm that was
    // the only consumer of it was deliberately removed, because it reasoned
    // about CPUs and not about boards (see the NOTE in fwVolumeState.h). The
    // parameter stays because "the other CPU is running" remains a fact the
    // caller has and this function is the place that decides what it is worth
    // -- and because the test suite asserts, state by state, that it changes
    // nothing. Drop the parameter and those assertions have nothing left to
    // assert against, so the day someone reintroduces an elimination rule there
    // would be no record that its absence was ever a decision.
    (void)otherPortIdentified;

    switch (state) {
    // Two or more drives are only an ambiguity when the target could be one of
    // them. If the target CPU is running firmware it is in none of them, and
    // touching its port produces a drive whose identity is established by the
    // touch that caused it -- so the mounted crowd is irrelevant rather than
    // disqualifying. Only when we cannot rule the target out of the crowd does
    // this stay the refusal it always was.
    case VolumeState::Ambiguous:
        return portIdentified ? GuardAction::TouchThenWait
                              : GuardAction::RefuseAmbiguous;
    case VolumeState::OursAfterTouch:     return GuardAction::Proceed;
    // The pre-existing drive used to force a typed confirmation here even when
    // the target CPU was demonstrably running -- which made the ordinary
    // touch-and-flash path unreachable for a board with its OTHER CPU sitting
    // in BOOTSEL, the single most common state a user needs this app in.
    case VolumeState::ForeignMounted:
        if (portIdentified) return GuardAction::TouchThenWait;
        return GuardAction::RequireTypedConfirmation;
    // Deliberately NOT gated on portIdentified: the CPU this plan just erased
    // has no firmware and so publishes no serial port, by construction. Its
    // volume is what identifies it here, and the evidence for that volume's
    // identity is the erase this same plan performed one step ago -- not
    // anything the port list could say.
    case VolumeState::ExpectedAfterErase: return GuardAction::WaitForEraseReboot;
    // Also deliberately NOT gated on portIdentified, and for the same shape of
    // reason: a CPU sitting in the RP2040 bootrom publishes no serial port, so
    // requiring one here would refuse every single time -- in exactly the
    // situation the CPU probe exists to resolve. The port list is not the
    // evidence in play; a measurement of the silicon is, and it is the stronger
    // of the two.
    case VolumeState::MappedToTarget:     return GuardAction::WriteMappedVolume;
    // Knowing the mounted drive is the OTHER CPU is only a refusal when there
    // is nowhere else for this step to go. If our own CPU is running, there is:
    // touch it, and write to the drive it raises. The other CPU's drive is then
    // simply not this step's business -- refusing on its account would block an
    // install for the sole reason that some unrelated CPU happens to be sitting
    // in BOOTSEL, which is the ordinary state a user reaches for this app in.
    case VolumeState::MappedToOtherCpu:
        return portIdentified ? GuardAction::TouchThenWait
                              : GuardAction::RefuseWrongCpu;
    case VolumeState::NoneMounted:
        // No elimination arm here, deliberately: elimination names a drive,
        // and there is no drive to name. With nothing mounted and no port to
        // touch there is genuinely nothing to work with.
        return portIdentified ? GuardAction::TouchThenWait
                              : GuardAction::RefuseUnidentified;
    }
    return GuardAction::RefuseAmbiguous;  // unreachable; refuse by default
}

bool confirmationMatches(std::string_view typed, TargetCpu cpu)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    auto first = std::find_if(typed.begin(), typed.end(), notSpace);
    auto last  = std::find_if(typed.rbegin(), typed.rend(), notSpace).base();
    if (first >= last) return false;

    std::string t(first, last);
    for (char& c : t) c = char(std::toupper((unsigned char)c));

    return t == (cpu == TargetCpu::Main ? "MAIN" : "DISPLAY");
}

} // namespace fwog
