#include "flash/fwCpuProbe.h"

#include "catalog/fwUf2Header.h"
#include "core/fwSha256.h"

#include <algorithm>
#include <cctype>

namespace fwog {
namespace {

IdentifyResult fail(IdentifyOutcome outcome, std::string message)
{
    IdentifyResult r;
    r.outcome = outcome;
    r.message = std::move(message);
    return r;
}

std::string trimmedLower(std::string_view s)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    auto first = std::find_if(s.begin(), s.end(), notSpace);
    auto last  = std::find_if(s.rbegin(), s.rend(), notSpace).base();
    if (first >= last) return {};
    std::string t(first, last);
    for (char& c : t) c = char(std::tolower((unsigned char)c));
    return t;
}

bool contains(std::span<const std::string> haystack, const std::string& needle)
{
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

/// Ports present now whose NAME was not present in `before`, each listed once.
///
/// An ORDERING HINT, not an identification -- see fwCpuProbe.h. A port whose
/// name was already in `before` may still be the prober (Windows reuses COM
/// numbers), and a name that is new may belong to anything at all. The
/// deduplication is defensive: listSerialPorts() already deduplicates, and this
/// must not start counting one port as two if some other source ever does not.
std::vector<std::string> newPorts(std::span<const std::string> before,
                                  std::span<const std::string> now)
{
    std::vector<std::string> added;
    for (const auto& p : now)
        if (!contains(before, p) && !contains(added, p)) added.push_back(p);
    return added;
}

/// "COM7"; "COM7 and COM8"; "COM7, COM8 and COM9".
std::string listOf(std::span<const std::string> items)
{
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += (i + 1 == items.size()) ? " and " : ", ";
        out += items[i];
    }
    return out;
}

/// What one questioning of one port established. Ranked: a later questioning of
/// the same port may only REPLACE a note with a more informative one.
enum class PortVerdict {
    Silent,        ///< could not be opened, or produced no complete line
    Unconfirmed,   ///< one reading arrived; a second to confirm it never did
    Foreign,       ///< said something that is neither "main" nor "display"
    Unstable,      ///< two readings, and they disagreed
    Answered,      ///< two readings, and they agreed
    Cancelled,     ///< the user asked this to stop
};

struct PortQuestion {
    PortVerdict                verdict = PortVerdict::Silent;
    std::optional<ProbeAnswer> answer;   ///< set iff Answered
    std::string                said;     ///< the line, iff Foreign
};

/// Ask one port which CPU it is on, and require it to say so TWICE.
///
/// The prober re-probes and reprints once per second rather than caching one
/// boot-time result, specifically so that a marginal SPI read shows up as an
/// UNSTABLE answer instead of a confident wrong one. Taking one line would
/// throw that property away.
///
/// Bounded by a FIXED NUMBER of reads, each with a fixed slice of the budget,
/// rather than by a running total: the time a successful read consumes is
/// decided inside io.readLine and is not reported back, so a total-based loop
/// could overshoot its own deadline by an unbounded amount while still looking
/// bounded here. kProbeLineAttempts * the slice is the real ceiling, and it is
/// the number this comment can honestly quote.
PortQuestion questionPort(const ProbeIo& io, const std::string& port)
{
    constexpr int kSliceMs = kProbeLineWaitMs / kProbeLineAttempts;

    std::optional<ProbeAnswer> firstAnswer;
    for (int attempt = 0; attempt < kProbeLineAttempts; ++attempt) {
        // Cancellation poll. A zero interval means "do not sleep, just tell me
        // whether the user has cancelled" -- the sleeping in this loop is done
        // by io.readLine's own timeout, which is what is actually waiting.
        if (!io.waitTick(0)) return PortQuestion{ PortVerdict::Cancelled, std::nullopt, {} };

        const auto line = io.readLine(port, kSliceMs);
        if (!line) continue;                          // nothing complete in this slice
        if (trimmedLower(*line).empty()) continue;    // blank line: not an answer, not an error

        const auto answer = parseProbeLine(*line);
        if (!answer)
            // Not a refusal of the whole flow any more, and that is the point
            // of the rework: an unexplained device saying an unexplained thing
            // means THIS PORT is not the prober, which is a fact about one
            // candidate. Somewhere else may still be the prober, and the caller
            // goes and asks. What has not changed is that nothing is ever
            // concluded FROM this line.
            return PortQuestion{ PortVerdict::Foreign, std::nullopt, *line };

        if (!firstAnswer) { firstAnswer = answer; continue; }
        if (*firstAnswer != *answer)
            return PortQuestion{ PortVerdict::Unstable, std::nullopt, {} };
        return PortQuestion{ PortVerdict::Answered, answer, {} };
    }

    return PortQuestion{ firstAnswer.has_value() ? PortVerdict::Unconfirmed
                                                 : PortVerdict::Silent,
                         std::nullopt, {} };
}

TargetCpu cpuOf(ProbeAnswer answer)
{
    return answer == ProbeAnswer::Main ? TargetCpu::Main : TargetCpu::Display;
}

} // namespace

const char* probeCpuName(TargetCpu cpu)
{
    return cpu == TargetCpu::Main ? "MAIN" : "DISPLAY";
}

bool looksLikeProberUsbId(std::string_view deviceInstanceId)
{
    // Empty means the platform could not say. That is not a match: absence of
    // evidence is the one thing this whole flow refuses to treat as evidence.
    if (deviceInstanceId.empty()) return false;

    std::string id;
    id.reserve(deviceInstanceId.size());
    for (char c : deviceInstanceId) id.push_back(char(std::toupper((unsigned char)c)));

    if (id.find(kProbeUsbVid) == std::string::npos) return false;
    if (id.find(kProbeUsbPid) == std::string::npos) return false;

    // A composite device's instance id names the interface it belongs to. The
    // CDC is interface 0; interface 2 is pico_stdio_usb's Reset interface,
    // which is not a serial port and cannot answer anything. An id that names
    // no interface at all is accepted -- the vendor/product pair is what
    // identifies the device, and demanding a field that need not be present
    // would refuse a port for a reason that says nothing about it.
    const auto mi = id.find("MI_");
    if (mi == std::string::npos) return true;
    return id.compare(mi, 5, "MI_00") == 0;
}

std::optional<ProbeAnswer> parseProbeLine(std::string_view line)
{
    const std::string t = trimmedLower(line);
    if (t == "main")    return ProbeAnswer::Main;
    if (t == "display") return ProbeAnswer::Display;
    return std::nullopt;
}

IdentifyResult identifyCpus(const ProbeIo& io, const ProbeProgressFn& progress)
{
    const auto say = [&progress](std::string s) { if (progress) progress(std::move(s)); };

    // --- 1. Exactly two volumes, or there is nothing here to disambiguate ---
    std::vector<std::string> volumes = io.findVolumes();
    if (volumes.size() != 2)
        return fail(IdentifyOutcome::NotExactlyTwoVolumes,
                    "this needs exactly two RPI-RP2 volumes mounted, and " +
                    std::to_string(volumes.size()) + (volumes.size() == 1 ? " is" : " are") +
                    " mounted. With one volume, the flash dialog already asks you to type "
                    "the CPU name instead. With three or more, they cannot all be this "
                    "board's two CPUs, so nothing could be concluded by elimination. "
                    "Nothing was written.");

    // Either volume works -- which one is written to is exactly the thing that
    // does not matter, and that is the point of the mechanism, not a shortcut.
    const std::string proberVolume    = volumes[0];
    const std::string remainingVolume = volumes[1];

    // Snapshotted BEFORE anything reboots, so the port that appears afterwards
    // can be identified by difference rather than by guesswork.
    const std::vector<std::string> portsBefore = io.listPorts();

    // --- 2. The image, checked to destruction before the board is touched ---
    say("loading the CPU prober image");
    auto bytes = io.loadProbeImage();
    if (!bytes)
        return fail(IdentifyOutcome::ProbeImageRejected,
                    "the CPU prober image could not be loaded: " + bytes.error() +
                    ". Nothing was written.");

    const std::string actualHash = sha256Hex(*bytes);
    if (actualHash != kProbeImageSha256)
        // This is the runtime half of the drift gate described in
        // fwCpuProbe.h. It cannot fire in a build that passed
        // cmake/ProbeProvenance.cmake, and that is precisely why it is here:
        // the one image this project will write to an UNIDENTIFIED CPU does
        // not get written on the strength of a build step nobody re-ran.
        return fail(IdentifyOutcome::ProbeImageRejected,
                    "the embedded CPU prober image does not match the one this build "
                    "expects (expected " + std::string(kProbeImageSha256) + ", got " +
                    actualHash + "). It was NOT written. This means the embedded image "
                    "and the source it is supposed to have been built from have drifted "
                    "apart -- see probe/README.md.");

    if (const auto info = parseUf2(*bytes); !info)
        return fail(IdentifyOutcome::ProbeImageRejected,
                    "the CPU prober image failed UF2 validation (" +
                    uf2ErrorMessage(info.error()) + "). Nothing was written.");

    // --- 3. Write the prober to one of the two unknown volumes --------------
    //
    // THIS IS A WRITE TO A CPU WE CANNOT IDENTIFY, AND IT IS THE ONLY ONE THIS
    // PROJECT PERMITS. It is safe for exactly one reason, and if that reason
    // ever stops being true this whole flow must be deleted rather than
    // adjusted:
    //
    //   probe.uf2 NEVER CONFIGURES OR DRIVES GPIO 29.
    //
    // On the DISPLAY CPU, GPIO 29 is MIC_SIG -- the OUTPUT of a PDM microphone.
    // A MAIN application image drives GPIO 29 (it is FPGA_RESET there), and on
    // the DISPLAY CPU that means two outputs fighting over one pin, which can
    // physically damage the board. That -- not the app's guesswork, and not
    // the user's confidence -- is what makes writing an image to a coin-flip
    // CPU acceptable here. The prober touches GPIOs 4, 6, 7 and 18 only, all
    // of which the shipping FreeWili firmware already drives on BOTH CPUs for
    // this very check, and probe/README.md carries the exhaustive
    // disassembly-level evidence for that claim against this exact image.
    //
    // The hash check immediately above is what ties "this exact image" to the
    // evidence. It is not a formality: without it, "the prober is safe on an
    // unidentified CPU" would be a statement about a file nobody verified.
    //
    // Nothing else in this project may be written to an unidentified CPU. The
    // ordinary flash path still refuses two volumes outright (classifyVolumes
    // -> Ambiguous -> RefuseAmbiguous), still demands a typed CPU name for a
    // single foreign volume, and none of that is relaxed by this function
    // existing. Do not generalise this into a "write to unknown volume"
    // helper, and do not route any other image through it.
    say("writing the CPU prober to " + proberVolume +
        " (safe on either CPU: it never touches GPIO 29)");
    auto staged = io.stageFile(*bytes, "fwog_probe.uf2");
    if (!staged)
        return fail(IdentifyOutcome::CopyFailed,
                    "could not prepare the CPU prober image: " + staged.error() +
                    ". Nothing was written.");
    if (auto copied = io.copyToVolume(*staged, proberVolume); !copied)
        return fail(IdentifyOutcome::CopyFailed,
                    "could not write the CPU prober to " + proberVolume + ": " +
                    copied.error() + ". That CPU may hold a partial image; it is still "
                    "in its bootloader and can be written again.");

    // --- 4. Wait for that volume, and only that volume, to go away ----------
    //
    // The mapping is derived by ELIMINATION, so the elimination has to be
    // observed rather than assumed. Anything other than "the pair became
    // exactly the other one" refuses.
    say("waiting for " + proberVolume + " to reboot into the prober");
    int waited = 0;
    // Only used to word the timeout accurately. "Both volumes went away" is
    // NOT refused on sight, deliberately: a spurious refusal here is expensive
    // in a way most of them are not -- the prober has already been written, so
    // there is no longer a second volume to retry with, and the user would be
    // pushed towards the manual port picker for no reason. Letting the bounded
    // wait run its course costs twenty seconds and refuses anyway if the state
    // really did change.
    bool everSawRemainingGone = false;
    for (;;) {
        volumes = io.findVolumes();

        const bool proberGone    = !contains(volumes, proberVolume);
        const bool remainingHere = contains(volumes, remainingVolume);

        if (volumes.size() > 2)
            return fail(IdentifyOutcome::VolumeSetChanged,
                        "a third RPI-RP2 volume appeared while waiting for " + proberVolume +
                        " to reboot. The mapping is only valid for the two volumes it "
                        "started from, so nothing was concluded.");

        for (const auto& v : volumes)
            if (v != proberVolume && v != remainingVolume)
                return fail(IdentifyOutcome::VolumeSetChanged,
                            "an RPI-RP2 volume (" + v + ") that was not there when this "
                            "started is now mounted. The mapping is only valid for the two "
                            "volumes it started from, so nothing was concluded.");

        if (proberGone && remainingHere) break;   // exactly what was expected

        if (!remainingHere) everSawRemainingGone = true;

        if (!proberGone && !remainingHere)
            return fail(IdentifyOutcome::WrongVolumeReleased,
                        "the wrong volume went away: " + remainingVolume + " unmounted while "
                        + proberVolume + ", the one the prober was written to, is still "
                        "mounted. That is not what writing the prober does, so neither "
                        "drive can be identified. Unplug the board and start again.");

        if (waited >= kProbeReleaseWaitMs)
            return fail(IdentifyOutcome::VolumeReleaseTimeout,
                        everSawRemainingGone
                          ? ("the RPI-RP2 volumes did not settle into the one state this can "
                             "conclude anything from -- " + remainingVolume + " went away too, "
                             "so there is nothing left to identify by elimination. The prober "
                             "image did land on the other CPU; it is still in a recoverable "
                             "state.")
                          : (proberVolume + " did not unmount after the CPU prober was written "
                             "to it, so nothing could be identified. The prober image did land "
                             "on that CPU; unplug the board and start again."));
        if (!io.waitTick(kProbePollMs))
            return fail(IdentifyOutcome::Aborted, "cancelled.");
        waited += kProbePollMs;
    }

    // --- 5. Find the prober's port, and prove it by what it SAYS ------------
    //
    // POSITIVE identification, not differential. This step used to be "wait for
    // exactly one new port name, then believe it"; that gate is gone, and its
    // removal is the fix. Two things were wrong with it, both silent, both
    // invisible on a freshly booted machine and both reliably hit by the
    // repeated plug/unplug cycles a RECOVERY tool lives among:
    //
    //   * a name that was already in the "before" list can still be the
    //     prober's, because Windows REUSES COM numbers and the list this
    //     diffed retained names for devices long gone. The board owner's
    //     prober came up as COM69 while three dead COM69 entries were already
    //     in the snapshot, so the difference was empty and the app reported
    //     that no port had appeared while Device Manager was showing it;
    //   * duplicates in that list counted as several ports appearing, which
    //     tripped the "more than one new port, so which is the prober cannot be
    //     told" refusal on a machine where exactly one had.
    //
    // What replaces it is not weaker, because the gate was never the thing
    // doing the work. The CONTENT check below it always was: the port has to
    // say "main" or "display", twice, agreeing. So candidates are QUEUED --
    // best evidence first -- and asked, and the first one that answers properly
    // is the prober. Nothing is ever accepted for being the only candidate.
    //
    // Best first means:
    //   1. the prober's USB identity AND a name that was not there before;
    //   2. the prober's USB identity (this is the board owner's case);
    //   3. a name that was not there before, with no identity information --
    //      the fallback for platforms that cannot report USB identities.
    //
    // Ports NOT in that queue are never opened. Reading a port at 115200 is
    // harmless in itself, but a recovery tool that opens every serial port on
    // the machine is interfering with hardware it was not pointed at.
    say("looking for the prober's USB serial port");

    /// One port, and the most informative thing it ever did. Kept so that a
    /// refusal can say what was actually observed rather than asserting that
    /// nothing appeared.
    struct PortNote {
        std::string port;
        PortVerdict verdict = PortVerdict::Silent;
        std::string what;
    };
    std::vector<PortNote> notes;
    std::vector<std::string> writtenOff;   ///< answered, and not with our protocol

    const auto recordNote = [&notes](const std::string& p, PortVerdict v, std::string what) {
        for (auto& n : notes) {
            if (n.port != p) continue;
            // A port questioned more than once keeps its most informative
            // verdict: "it gave one reading" says more than "it went quiet the
            // second time", and the user is owed the more useful of the two.
            if (v > n.verdict) { n.verdict = v; n.what = std::move(what); }
            return;
        }
        notes.push_back(PortNote{ p, v, std::move(what) });
    };

    std::string proberPort;
    std::optional<ProbeAnswer> confirmed;
    int questions = 0;

    waited = 0;
    while (!confirmed) {
        const std::vector<std::string> added = newPorts(portsBefore, io.listPorts());
        const std::vector<std::string> byIdentity =
            io.listProberPorts ? io.listProberPorts() : std::vector<std::string>{};

        std::vector<std::string> candidates;
        const auto offer = [&candidates, &writtenOff](const std::string& p) {
            if (p.empty()) return;
            if (contains(candidates, p)) return;   // already queued, at a better priority
            if (contains(writtenOff, p)) return;   // it answered, and said it is not us
            candidates.push_back(p);
        };
        for (const auto& p : byIdentity) if (contains(added, p)) offer(p);
        for (const auto& p : byIdentity) offer(p);
        for (const auto& p : added)      offer(p);

        for (const auto& candidate : candidates) {
            if (questions >= kProbeMaxPortQuestions) break;
            ++questions;

            say("asking " + candidate + " which CPU it is on");
            const PortQuestion q = questionPort(io, candidate);

            switch (q.verdict) {
            case PortVerdict::Cancelled:
                return fail(IdentifyOutcome::Aborted, "cancelled.");

            case PortVerdict::Unstable:
                // A device that speaks this protocol and contradicts itself is
                // the prober, misreading the radio. Moving on to another
                // candidate would be looking for a second opinion after being
                // told the first one is unreliable.
                return fail(IdentifyOutcome::UnstableProbeAnswer,
                            "the prober on " + candidate + " gave two different answers in a "
                            "row. Its reading of the radio is not stable, so it cannot be "
                            "trusted to say which CPU it is on.");

            case PortVerdict::Answered:
                proberPort = candidate;
                confirmed  = q.answer;
                break;

            case PortVerdict::Foreign:
                writtenOff.push_back(candidate);
                recordNote(candidate, q.verdict, q.said);
                say(candidate + " said \"" + q.said + "\", which is not this project's "
                    "prober; it is not a candidate any more");
                break;

            case PortVerdict::Unconfirmed:
            case PortVerdict::Silent:
                // NOT written off. The window between Windows publishing a COM
                // name and the port actually opening is real, and disqualifying
                // a port for losing that race would be the same class of
                // mistake as the one being fixed here.
                recordNote(candidate, q.verdict, {});
                break;
            }

            if (confirmed) break;
        }

        if (confirmed) break;
        if (questions >= kProbeMaxPortQuestions) break;
        if (waited >= kProbePortWaitMs) break;
        if (!io.waitTick(kProbePollMs))
            return fail(IdentifyOutcome::Aborted, "cancelled.");
        waited += kProbePollMs;
    }

    // --- 5b. Refuse in the words of what was actually observed --------------
    if (!confirmed) {
        if (notes.empty())
            return fail(IdentifyOutcome::NoProbePort,
                        "no serial port that could be the prober's turned up after " +
                        proberVolume + " rebooted, so nothing could be asked which CPU it "
                        "is on. Nothing with the prober's USB identity (" +
                        std::string(kProbeUsbVid) + "&" + std::string(kProbeUsbPid) +
                        ") is attached, and no port name appeared that was not already "
                        "there. The prober image is on that CPU and is still running; "
                        "unplug and replug the board, then try again.");

        std::vector<std::string> asked;
        std::vector<std::string> what;
        bool anySpoke = false;
        for (const auto& n : notes) {
            asked.push_back(n.port);
            switch (n.verdict) {
            case PortVerdict::Foreign:
                anySpoke = true;
                what.push_back(n.port + " said \"" + n.what + "\", which is neither \"main\" "
                                                              "nor \"display\"");
                break;
            case PortVerdict::Unconfirmed:
                what.push_back(n.port + " gave one reading but never the second one needed to "
                                        "confirm it");
                break;
            case PortVerdict::Silent:
            // Unstable, Answered and Cancelled never reach a note -- each one
            // leaves the search there and then. Listed rather than defaulted so
            // that a new verdict has to be considered here instead of silently
            // being described as silence.
            case PortVerdict::Unstable:
            case PortVerdict::Answered:
            case PortVerdict::Cancelled:
                what.push_back(n.port + " never produced a complete line");
                break;
            }
        }

        const std::string preamble =
            (asked.size() == 1 ? "one serial port was asked which CPU it is on ("
                               : std::to_string(asked.size()) +
                                 " serial ports were asked which CPU they are on (") +
            listOf(asked) + "), and none gave this project's prober's answer: ";

        std::string detail;
        for (std::size_t i = 0; i < what.size(); ++i) {
            if (i > 0) detail += "; ";
            detail += what[i];
        }

        // Two outcomes, because they mean different things to the user. A port
        // that SPOKE and said something else is a wrong device on the line;
        // ports that only ever went quiet are a prober that has not been heard
        // from, and trying again is the reasonable next move.
        return fail(anySpoke ? IdentifyOutcome::UnrecognisedProbeLine
                             : IdentifyOutcome::NoProbeLine,
                    preamble + detail + ". Nothing was concluded. The prober image is on the "
                    "CPU that " + proberVolume + " belonged to and is still running; unplug "
                    "and replug the board, then try again.");
    }

    const TargetCpu proberCpu = cpuOf(*confirmed);
    const TargetCpu remaining = otherCpu(proberCpu);

    // --- 6. Refuse on contradictory evidence --------------------------------
    //
    // Both CPUs were sitting in BOOTSEL when this started -- that is the
    // precondition, two RPI-RP2 volumes -- and a CPU in BOOTSEL publishes no
    // FreeWili serial port. So the app simultaneously reporting a healthy port
    // for the very CPU the prober claims to be running on is a direct
    // contradiction: it means a second board is involved, or the scan is
    // stale, or the answer is wrong. Any of those makes the mapping unsound,
    // and the prober's answer being authoritative does not extend to
    // overruling evidence that it cannot be right.
    //
    // Note what this check is NOT reading. `identity` is fwfinder's own
    // enumeration of attached FreeWili devices, snapshotted on the UI thread --
    // it has never had anything to do with the port-name list whose staleness
    // broke the step above, so no dead COM entry can either satisfy this check
    // or defeat it. The port it is compared against, `proberPort`, is now a
    // port that answered rather than a name that appeared, which makes the
    // comparison stronger than it was, not weaker.
    const CpuIdentity identity = io.identify();
    const auto& claimedPort = (proberCpu == TargetCpu::Main) ? identity.mainPort
                                                              : identity.displayPort;
    if (claimedPort.has_value() && *claimedPort != proberPort)
        return fail(IdentifyOutcome::ContradictedByPorts,
                    std::string("the prober says it is running on the ") + probeCpuName(proberCpu) +
                    " CPU, but this app can also see a working " + probeCpuName(proberCpu) +
                    " serial port (" + *claimedPort + ") that is not the prober's. A CPU in "
                    "its bootloader has no such port, so something here is not what it "
                    "appears to be -- most likely a second board. Nothing was concluded.");

    // --- 7. The volumes must STILL be what the conclusion rests on ----------
    const auto volumesNow = io.findVolumes();
    if (volumesNow.size() != 1 || volumesNow.front() != remainingVolume)
        return fail(IdentifyOutcome::VolumeSetChanged,
                    "the mounted RPI-RP2 volumes changed while the prober was being read, so "
                    "the conclusion no longer describes what is plugged in. Nothing was "
                    "concluded. Start again.");

    IdentifyResult r;
    r.outcome         = IdentifyOutcome::Success;
    r.proberVolume    = proberVolume;
    r.remainingVolume = remainingVolume;
    r.proberPort      = proberPort;
    r.proberCpu       = proberCpu;
    r.remainingCpu    = remaining;
    r.message = std::string("the prober is running on the ") + probeCpuName(proberCpu) +
                " CPU (it was written to " + proberVolume + "), so the RPI-RP2 volume still "
                "mounted, " + remainingVolume + ", is the " + probeCpuName(remaining) + " CPU.";
    say(r.message);
    return r;
}

MappingCheck mappingStillFresh(const IdentifyResult& result,
                                std::span<const std::string> volumesNow,
                                std::optional<std::chrono::milliseconds> age,
                                std::chrono::milliseconds maxAge)
{
    if (result.outcome != IdentifyOutcome::Success) return MappingCheck::NoIdentification;

    // The volume comparison first, and reported as itself rather than folded
    // into staleness: both refuse, so the safety outcome is identical either
    // way, but "the drives changed" tells the user what to do next and
    // "it is too old" does not. Same split, for the same reason, as
    // selectionUnchangedFresh() passing a non-Unchanged answer straight
    // through instead of overwriting it with StaleSnapshot.
    if (volumesNow.size() != 1 || volumesNow.front() != result.remainingVolume)
        return MappingCheck::VolumesChanged;

    // nullopt before the comparison, deliberately: an unknown age is the least
    // verified state there is and must never read as fresh.
    if (!age.has_value()) return MappingCheck::StaleIdentification;
    if (*age > maxAge)    return MappingCheck::StaleIdentification;
    return MappingCheck::Fresh;
}

VerifiedVolume verifiedVolumeFrom(const IdentifyResult& result,
                                   std::span<const std::string> volumesNow,
                                   std::optional<std::chrono::milliseconds> age,
                                   std::chrono::milliseconds maxAge)
{
    // The whole of the gate, in one line, delegating rather than re-deriving:
    // there must be exactly one rule for whether a mapping still describes the
    // board in front of the user, and mappingStillFresh() is it.
    if (mappingStillFresh(result, volumesNow, age, maxAge) != MappingCheck::Fresh)
        return {};
    return VerifiedVolume{ result.remainingVolume, result.remainingCpu };
}

std::string mappingCheckMessage(MappingCheck check)
{
    switch (check) {
    case MappingCheck::Fresh:
        return {};
    case MappingCheck::NoIdentification:
        return "No CPU identification has been completed, so there is nothing to act on.";
    case MappingCheck::VolumesChanged:
        return "The mounted RPI-RP2 volumes are no longer the ones this identification was "
               "made against, so it says nothing about what is plugged in now. Run it again.";
    case MappingCheck::StaleIdentification:
        return "This identification is too old to still be trusted -- a drive letter is a "
               "reusable name, and a board that has been replugged since can hand the same "
               "letter to the other CPU. Run it again.";
    }
    return {};
}

} // namespace fwog
