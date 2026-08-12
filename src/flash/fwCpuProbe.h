#pragma once

#include "core/fwTypes.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fwog {

// ---------------------------------------------------------------------------
// The both-CPUs-erased recovery flow.
//
// THE DEAD END this exists to close. Both RP2040s on a FreeWili OG present an
// identical RPI-RP2 volume in BOOTSEL mode. With two mounted, classifyVolumes()
// answers Ambiguous and every flash path refuses -- correctly, because a MAIN
// application image on the DISPLAY CPU drives GPIO 29 against a PDM microphone
// and can damage the board, and the DISPLAY CPU has no BOOTSEL button and no
// other way back. Both CPUs erased is reachable with documented actions alone
// (erase MAIN from the Default Firmware tab; the deprecated-firmware install's
// first step erases DISPLAY), and once there, the refusal blocks the very path
// that would fix it.
//
// THE WAY OUT. The CC1101 sub-GHz radio is wired to MAIN only, so the drives
// are not actually indistinguishable -- only their labels are. The prober
// (wiliOGBsp's apps/cpuprobe, committed here as probe/probe.uf2) is a ~49 kB
// RP2040 image that probes for the CC1101 and prints "main" or
// "display" over USB CDC once per second, forever, and honours the 1200-baud
// touch back into BOOTSEL. Writing it to ONE of the two volumes removes that
// volume from the picture (its CPU reboots into the prober and stops being
// mass storage) and names the CPU it landed on; the volume still mounted is
// therefore the other CPU, by elimination.
//
// WHAT THIS DOES NOT DO. An ordinary flash with two volumes mounted still
// refuses exactly as before -- classifyVolumes() answers Ambiguous ahead of
// everything else, including any mapping, because a mapping established against
// one volume says nothing about a second one that has appeared beside it. This
// is a separate, explicitly user-initiated action whose entire product is
// KNOWLEDGE: which CPU the one remaining volume belongs to.
//
// WHAT THAT KNOWLEDGE IS THEN GOOD FOR, and why it is not a bypass. The
// knowledge is fed back in as an identity source (IdentitySource::VerifiedProbe
// and CpuIdentity's volume fields, fwTypes.h) via verifiedVolumeFrom() below.
// A step whose CPU the mapping names writes DIRECTLY to that volume:
// VolumeState::MappedToTarget -> GuardAction::WriteMappedVolume, with no
// 1200-baud touch and no typed confirmation.
//
// That is not ForeignMounted being relaxed; it is ForeignMounted's premise no
// longer holding. The typed confirmation exists because -- in its own words --
// "which CPU that is cannot be determined from the mount". A probe result
// determines it, by measurement of the silicon, which is strictly stronger
// evidence than a USB product string this project already accepts without a
// confirmation. Re-asking a question that has been answered does not add
// safety; it trains the user to type CPU names without reading them, and every
// other guard in this project depends on them not having that habit.
//
// The same knowledge REFUSES in the other direction, which is the half that
// makes this a net tightening: a mounted volume the mapping names as the OTHER
// CPU produces VolumeState::MappedToOtherCpu ->
// FlashOutcome::RefusedWrongCpu. Before, that case reached the confirmation
// prompt, and a user who typed the CPU name they were asked for got a wrong-CPU
// write.
//
// And it expires aggressively. See verifiedVolumeFrom() and mappingStillFresh()
// below: any change to the mounted volume set, and any age past
// kMaxMappingAge, and there is no mapping at all -- and classifyVolumes()
// re-checks the volume set itself on every step, so a stale one cannot
// authorise a write even if a caller hands one over.
// ---------------------------------------------------------------------------

/// The embedded id of the prober image (firmware/manifest.json's "images").
inline constexpr const char* kProbeImageId = "fwog_probe";

// --- Where the prober comes from -------------------------------------------
//
// The prober is NOT built here and its source does not live here. It is
// `apps/cpuprobe` in the wiliOGBsp BSP, and it is built there so that it
// inherits the FreeWili OG board header: 16 MB QSPI flash,
// PICO_FLASH_SPI_CLKDIV 4, clk_peri following clk_sys, and the BSP's 200 MHz
// operating point.
//
// That is not housekeeping. The previous prober lived in this repository, was
// built for PICO_BOARD=pico, and DID NOT BOOT on this silicon -- it ran about
// 5.6 s, never enumerated a CDC port, and fell back to BOOTSEL. The two board
// headers select the same boot2 by name (boot2_w25q080), but
// PICO_FLASH_SPI_CLKDIV is compiled INTO boot2, and pico.h sets 2 where
// freewili_og.h sets 4; the two 256-byte boot2 blocks differ at exactly one
// code byte. Being built against the board's own BSP is a correctness
// requirement for this image, not a tidiness preference.
//
// It is also deliberately NOT a display app and NOT a main app: it links
// fwog_common only, never fwog_main_bsp or fwog_display_bsp, because a
// per-CPU BSP's board_init() would bring up the inter-CPU link, the watchdog,
// the display or the FPGA on a processor that may be the other one. That is
// the same hazard this whole flow exists to avoid.
//
// --- Drift gate ------------------------------------------------------------
//
// probe/probe.uf2 is COMMITTED, unlike every other .uf2 in this repo. Two hard
// requirements collide: the app must stay a single self-contained executable
// (so the prober has to be embedded), and building the desktop app must not
// require an ARM cross-toolchain or a firmware checkout (so it cannot be built
// from source here). 49 kB in git is the price of both being true.
//
// The objection to committing it is real and is the one that matters: a binary
// that drifts out of sync with the source it claims to be built from is the
// worst possible failure mode for the one image this project will write to an
// unidentified CPU. So drift is DETECTED rather than assumed away:
//
//   1. Build time, by cmake/ProbeProvenance.cmake. probe/probe.uf2 is hashed
//      against kProbeImageSha256 ALWAYS. The prober's source and build script
//      are hashed against the two constants below WHEN a wiliOGBsp checkout is
//      reachable (FWOG_BSP_DIR, a sibling directory by default) -- and skipped
//      with a STATUS message when it is not, because requiring a firmware
//      checkout to build a desktop app would break the second requirement
//      above. Either way the failure prints the actual value to paste in.
//   2. Build time again, by tools/embed_firmware.py, which records the SHA-256
//      of the bytes it actually embedded.
//   3. Run time, in identifyCpus(): the loaded image is hashed and compared
//      against kProbeImageSha256 before a single byte of it reaches a board.
//      A build that somehow embedded something else refuses to write it.
//
// Note what the source hashes can and cannot do now that the source is in
// another repository: they prove the image matches a source tree THIS MACHINE
// can see, not that the sibling checkout is at the commit named below. The
// image hash is the load-bearing one, and it is the one the runtime re-checks.
//
// The two source hashes are not read by any C++ code -- they exist so that the
// build-time gate's expectations live in reviewed source next to the image
// hash, rather than in a side file nobody reads. Text files are hashed with
// CRLF normalised to LF, so a line-ending conversion on checkout cannot break
// the build; probe.uf2 is hashed byte-for-byte.
//
// To change the prober: edit wiliOGBsp/apps/cpuprobe, rebuild it there
// (`fw build cpuprobe`), copy the new cpuprobe.uf2 over probe/probe.uf2, then
// build the desktop app and paste the "actual" values the failure reports into
// the constants below. Re-derive the GPIO-29 evidence too -- the hashes prove
// the binary matches the source, not that the new source is still safe.

// --- THE IMAGE IS BYTE-REPRODUCIBLE, so a mismatch is always a real change --
//
// This matters more than it sounds, because it is what makes the three hashes
// below worth having. Until wiliOGBsp 330ded5 the prober was NOT reproducible:
// the pico-sdk stamps the wall-clock build date into the binary-info block
// (standard_binary_info.c defaults PICO_PROGRAM_BUILD_DATE to __DATE__), so the
// image changed by one byte -- offset 38433, the day-of-month digit -- the
// instant the clock rolled past midnight. Every gate here then failed the next
// morning with nothing whatsoever having changed.
//
// That is not a cosmetic annoyance, it is a safety regression. A gate that
// cries wolf nightly is a gate people learn to route around, and re-recording
// the hashes to make it quiet is exactly the "bless a build nobody checked"
// move this gate exists to stop. So the cause was removed rather than the
// symptom: apps/cpuprobe/CMakeLists.txt now pins PICO_PROGRAM_BUILD_DATE to a
// fixed string, and that is the ONLY use of __DATE__/__TIME__/__TIMESTAMP__
// reachable from this image in either the SDK or the BSP.
//
// Evidence, recorded because the claim is the whole point: three fresh build
// trees at three different paths produce identical bytes, and a build with
// __DATE__ and __TIME__ forcibly redefined on the command line produces those
// same bytes, while the same injection with the pin removed changes them. So
// the image no longer depends on the clock, the day, or the build directory.
//
// WHAT THAT MEANS FOR YOU, if a hash below has just failed: it is NOT a clock
// tick, and it is not the build directory you used. Something in
// wiliOGBsp/apps/cpuprobe, in the BSP it links, or in the SDK/toolchain version
// genuinely changed. Rebuild, diff the disassembly against the old image, and
// re-derive probe/README.md's GPIO-29 evidence before pasting anything here.

/// The wiliOGBsp commit probe/probe.uf2 was built from. Documentation, not a
/// gate: the file hashes below are what is actually checked. Recorded so a
/// reader can `git -C ../wiliOGBsp show` the exact source of this image.
inline constexpr const char* kProbeBspCommit =
    "330ded5ecd2f83ea1a3e6d5cc1d16e3e58d38f34";

/// SHA-256 of probe/probe.uf2 (49,664 bytes), byte-for-byte.
///
/// Moved 23c99a9 -> 330ded5 ("fix: pin cpuprobe's build date so its image is
/// byte-reproducible"). NO INSTRUCTION CHANGED across that rebuild, which is
/// what lets probe/README.md's disassembly evidence carry over: objdump of the
/// two images differs in exactly five .word literal-pool constants -- two in
/// .text, three in the RAM-resident .data code -- and each of the five is a
/// .rodata string address in 0x100049b8-0x10004ea4, moved because the pinned
/// date string is a different length from "Jul 28 2026". Zero mnemonic lines
/// differ; .boot2 and .binary_info are byte-identical; every section's size and
/// VMA is unchanged. Re-running the README's GPIO-29 sweep on the new binary
/// produces output identical to the old one, character for character.
inline constexpr const char* kProbeImageSha256 =
    "d3f5eb07a5dc60e8cfa3854fa1c0736923cbcb5b98b38f74c09cef468583a6b6";

/// SHA-256 of wiliOGBsp/apps/cpuprobe/main.c, CRLF normalised to LF.
///
/// UNCHANGED since f2e086e, the commit that added the prober. The build-date
/// fix above touched only the build script; the C source has never moved.
inline constexpr const char* kProbeSourceSha256 =
    "8a3def7e7a2caf28419670337dab42dab2ea8f64bf168fe3d4285f11249fbe3e";

/// SHA-256 of wiliOGBsp/apps/cpuprobe/CMakeLists.txt, CRLF normalised to LF.
/// Included because the safety properties are not all in main.c: the 1200-baud
/// reset, the USB identity, the ABSENCE of
/// PICO_STDIO_USB_RESET_BOOTSEL_ACTIVITY_LED (which would make the SDK drive a
/// GPIO on the way into BOOTSEL), and the choice to link fwog_common rather
/// than either per-CPU BSP are all decided by that file.
///
/// Moved twice since the last recorded value. 518c4c2 ("one folder per app")
/// edited ONE WORD OF A COMMENT -- template_main -> template -- and nothing
/// else; 330ded5 added the PICO_PROGRAM_BUILD_DATE pin described above. Both
/// diffs were read, not inferred from their commit subjects.
inline constexpr const char* kProbeBuildScriptSha256 =
    "2d93122da15e2f6d79bbe14ef2dbcc470087be12fdf730feb6b02998be3530cf";

// --- How the prober's port is recognised -----------------------------------
//
// The prober enumerates as a composite USB device with the stock Raspberry Pi
// RP2 vendor id and the pico_stdio_usb product id: VID 2E8A, PID 000A, with the
// CDC on interface 0 and pico_stdio_usb's 1200-baud Reset interface on
// interface 2. Windows binds usbser.sys to interface 0, so the COM port's
// device instance id looks like
//
//     USB\VID_2E8A&PID_000A&MI_00\7&29198214&0&0000
//
// (captured from the board this was debugged against, on COM69).
//
// WHY IDENTITY RATHER THAN "WHICH PORT IS NEW". This flow used to find the
// prober by diffing a port list taken before the write against one taken
// after, and that is broken on Windows in a way that is invisible until it
// bites: COM numbers are REUSED, and the list it diffed
// (HKLM\HARDWARE\DEVICEMAP\SERIALCOMM) retains names for devices that are long
// gone. On the machine this was fixed on, COM69 was already in the "before"
// list three times over from previous plug cycles -- so when the prober came up
// AS COM69, the difference was empty and the app declared that no port had
// appeared while Device Manager was showing the prober plainly. A name that is
// both stale and current at once cannot carry the answer; the device's own
// identity can.

/// USB vendor id the prober enumerates with (Raspberry Pi RP2), spelled the way
/// a Windows device instance id spells it.
inline constexpr const char* kProbeUsbVid = "VID_2E8A";
/// USB product id the prober enumerates with (pico_stdio_usb CDC + reset).
inline constexpr const char* kProbeUsbPid = "PID_000A";
/// The same two numbers, bare, as sysfs and lsusb print them. The Linux form of
/// SerialPortInfo::usbId carries no "VID_"/"PID_" prefixes to match against --
/// see fwSerialPorts.h for why it does not pretend to. A test asserts these
/// stay the same numbers as the two above, because two spellings of one fact
/// are two things that can drift.
inline constexpr const char* kProbeUsbVidHex = "2E8A";
inline constexpr const char* kProbeUsbPidHex = "000A";

/// Could a port with this USB identity be the prober's CDC?
///
/// Requires the vendor and product ids, and -- when the id names a composite
/// interface at all -- requires it to be interface 0, the CDC. Interface 2 is
/// pico_stdio_usb's Reset interface and is not a serial port; matching it would
/// mean offering something that cannot be read as a candidate to read.
///
/// UNDERSTANDS BOTH SHAPES listSerialPortInfo() produces, and the reason it has
/// to is argued at SerialPortInfo::usbId in fwSerialPorts.h:
///
///   Windows  USB\VID_2E8A&PID_000A&MI_00\7&29198214&0&0000
///   Linux    usb:v2E8Ap000Ain00:3-4.1.1:1.0
///
/// The Linux shape is recognised by parsing it, not by looking for substrings
/// in it, and a string that does not parse as that shape is judged by the
/// Windows rules exactly as it was before the shape existed. So the two
/// branches cannot answer for each other's inputs. Both are compiled and tested
/// on every platform -- this is a pure function, and a platform-gated branch is
/// a branch that rots.
///
/// This is a NECESSARY condition, never a sufficient one. Any other RP2040
/// running pico_stdio_usb presents exactly this identity, so a match only earns
/// a port a place in the queue of candidates to question. What settles it is
/// the port answering "main" or "display" -- see identifyCpus().
///
/// Case-insensitive. An empty id is NOT a match: a platform that cannot report
/// an identity has told us nothing, and nothing is not evidence.
bool looksLikeProberUsbId(std::string_view deviceInstanceId);

/// What the prober said about the CPU it is running on.
enum class ProbeAnswer { Main, Display };

/// Interpret one line read from the prober's CDC port.
///
/// Accepts exactly two values -- "main" and "display" -- ignoring surrounding
/// whitespace and letter case, and NOTHING else. No prefix matching, no
/// "contains", no nearest match. A line this does not recognise is an
/// unexplained device saying an unexplained thing on a port we opened
/// expecting our own firmware, and the only safe reading of that is "stop".
std::optional<ProbeAnswer> parseProbeLine(std::string_view line);

/// Every way identifyCpus() can end. Only Success produces a mapping; every
/// other value is a refusal, and each says what specifically was not proved.
enum class IdentifyOutcome {
    Success,
    /// Not exactly two RPI-RP2 volumes when the flow started. One volume needs
    /// no identification (the engine's existing typed-confirmation path covers
    /// it); three or more cannot come from one two-CPU board at all, so the
    /// by-elimination step -- the whole mechanism -- would be a guess.
    NotExactlyTwoVolumes,
    /// The prober image could not be loaded, did not match kProbeImageSha256,
    /// or is not a valid RP2040 UF2. Nothing was written.
    ProbeImageRejected,
    CopyFailed,
    /// The volume we wrote the prober to is still mounted and the OTHER one
    /// went away. Whatever happened, it is not what writing the prober does,
    /// and nothing can be concluded about either drive.
    WrongVolumeReleased,
    /// The set of mounted volumes became something other than "exactly the
    /// other one of the original pair" -- a third volume, an unknown volume, or
    /// the pair changing underfoot. The mapping is only ever valid for the
    /// volumes it was derived from.
    VolumeSetChanged,
    /// The prober's volume never released within the budget.
    VolumeReleaseTimeout,
    /// There was no port to question at all: nothing with the prober's USB
    /// identity was attached, and no port appeared that was not there before.
    NoProbePort,
    /// Every port that was questioned failed to produce two agreeing readings
    /// within the budget -- silence, or a single unconfirmed reading.
    NoProbeLine,
    /// A questioned port said something that is neither "main" nor "display",
    /// and no other candidate answered correctly either.
    UnrecognisedProbeLine,
    /// Two readings disagreed. The prober repeats its answer once per second
    /// precisely so that a marginal SPI read shows up as an unstable answer
    /// instead of a confident wrong one.
    UnstableProbeAnswer,
    /// The prober's answer is contradicted by the ports the app can see -- see
    /// identifyCpus()'s comment. Contradictory evidence is never resolved in
    /// favour of one side; it refuses.
    ContradictedByPorts,
    Aborted,
};

/// The product of a successful identification, and the evidence it rests on.
///
/// `remainingVolume`/`remainingCpu` are the answer the user needs: the drive
/// still mounted, and which CPU it is. `proberVolume`/`proberCpu`/`proberPort`
/// are what proved it, and `proberPort` is also what the user needs afterwards
/// to touch that CPU back into BOOTSEL.
struct IdentifyResult {
    IdentifyOutcome outcome = IdentifyOutcome::NotExactlyTwoVolumes;
    std::string     message;

    // Set only when outcome == Success.
    std::string proberVolume;      ///< the volume the prober was written to; now gone
    std::string remainingVolume;   ///< the volume still mounted
    std::string proberPort;        ///< the prober's CDC port
    TargetCpu   proberCpu    = TargetCpu::Main;
    TargetCpu   remainingCpu = TargetCpu::Display;
};

/// Every piece of I/O the identification performs. Production supplies the
/// real platform functions (makeProductionProbeIo, fwCpuProbeController.h);
/// tests supply fakes. Nothing in identifyCpus() touches a device or a disk
/// except through here -- the same discipline, and the same reason, as
/// FlashIo in fwFlashEngine.h.
struct ProbeIo {
    std::function<std::vector<std::string>()> findVolumes;

    /// Every serial port the OS reports, by name. Used for the ORDERING HINT
    /// only -- a snapshot before the write, compared against one after, tells
    /// us which names are new, and a new name is a good reason to question a
    /// port first. It is not used as a gate: see the "how the prober's port is
    /// recognised" comment above for why "the name is new" is not something
    /// Windows can be asked reliably.
    std::function<std::vector<std::string>()> listPorts;

    /// The ports of PRESENTLY ATTACHED devices whose USB identity is the
    /// prober's (looksLikeProberUsbId). This is the primary way the prober's
    /// port is found; the returned ports are questioned first.
    ///
    /// May be empty because there is genuinely no such device, or because the
    /// platform cannot report USB identities at all. identifyCpus() cannot and
    /// does not distinguish those, and it must not: both mean "this told me
    /// nothing", and it falls back to the ordering hint either way.
    std::function<std::vector<std::string>()> listProberPorts;

    /// A SNAPSHOT of the CPU identity, captured on the UI thread before the
    /// worker starts, exactly as makeProductionFlashIo() does it -- DeviceModel
    /// is not thread-safe. Used only for the contradiction check.
    std::function<CpuIdentity()> identify;

    std::function<std::expected<std::vector<uint8_t>, std::string>()> loadProbeImage;

    std::function<std::expected<std::filesystem::path, std::string>(
        std::span<const uint8_t>, const std::string& filename)> stageFile;

    std::function<std::expected<void, std::string>(
        const std::filesystem::path&, const std::string& volume)> copyToVolume;

    /// Read one line from `port`, waiting at most `timeoutMs`. nullopt for
    /// "could not open" and for "nothing complete arrived" alike -- the caller
    /// treats both as no answer, which is the only safe reading of either.
    std::function<std::optional<std::string>(const std::string& port, int timeoutMs)> readLine;

    /// Called once per poll interval while waiting. Return false to abort.
    /// The delay itself belongs to the caller, so tests run instantly.
    std::function<bool(int intervalMs)> waitTick;
};

using ProbeProgressFn = std::function<void(const std::string&)>;

/// How long to wait for the prober's volume to release. Writing a 49 kB UF2 to
/// an RP2040 bootrom volume and rebooting off it takes about a second; a
/// longer budget only delays the error.
inline constexpr int kProbeReleaseWaitMs = 20000;
/// How long to wait for the prober's CDC port to appear. Longer than the
/// release wait on purpose: on Windows the port shows up only once usbser.sys
/// has been bound to the newly enumerated device, which routinely lags the
/// volume disappearing by a second or more, and can be slower on first sight
/// of a device.
inline constexpr int kProbePortWaitMs = 25000;
/// How many times, in total, a candidate port may be questioned.
///
/// Not "how many ports": a port that stayed silent is questioned AGAIN on a
/// later round rather than being written off, because the window between a COM
/// name being published and the port actually opening is real, and treating a
/// port as disqualified for losing that race would reintroduce the failure this
/// whole rework exists to remove. A port that ANSWERED with something other
/// than "main"/"display" is written off, because that is a device saying it is
/// not the prober, which is information rather than a race.
///
/// The honest ceiling this puts on the search is
/// kProbeMaxPortQuestions * kProbeLineWaitMs of reading (32 s) plus at most
/// kProbePortWaitMs of waiting between rounds; in practice the prober is the
/// first candidate and answers in about two seconds.
inline constexpr int kProbeMaxPortQuestions = 4;
/// How long to spend collecting lines, in total, and in how many attempts.
/// The prober speaks once per second and two agreeing readings are required,
/// so four two-second attempts is four chances at a two-second job. A hung
/// read in a recovery tool is its own trap, so the ceiling is a fixed number
/// of fixed-length reads rather than a running total -- see the reading loop
/// in identifyCpus() for why a running total would not actually be a ceiling.
inline constexpr int kProbeLineWaitMs  = 8000;
inline constexpr int kProbeLineAttempts = 4;
inline constexpr int kProbePollMs = 250;

/// Run the whole flow. See this header's opening comment for the mechanism.
///
/// Ordered so that everything which can refuse without touching the board
/// happens first: volume count, image load, hash, UF2 validation. The single
/// irreversible act is the copy, and by the time it runs the only unknown left
/// is the board itself.
///
/// FAILS CLOSED at every step. It never falls back to "probably the other
/// one" and never treats absence of evidence as evidence. Every outcome other
/// than Success leaves the caller with no mapping at all rather than a weak
/// one.
///
/// It DOES try several candidate ports, which is not the same as picking
/// between them: a candidate is only ever accepted for answering "main" or
/// "display" twice over, and a port that is the only candidate earns nothing by
/// being the only one. Being un-eliminated is not a qualification anywhere in
/// this function.
IdentifyResult identifyCpus(const ProbeIo& io, const ProbeProgressFn& progress);

/// Result of re-checking a mapping against the world as it is NOW.
enum class MappingCheck {
    Fresh,               ///< still valid; safe to act on
    NoIdentification,    ///< there is no successful identification to check
    VolumesChanged,      ///< the mounted volumes are no longer exactly the one identified
    StaleIdentification, ///< too old to still be about the board in front of the user
};

/// The oldest an identification may be and still be acted on.
///
/// Why any limit at all, and why this mirrors kMaxSnapshotAgeForFlash's
/// reasoning (fwFlashController.h) rather than inventing a second idea: the
/// mapping is a claim about which physical CPU a DRIVE LETTER belongs to, and
/// a drive letter is a reusable name. Unplug the board, plug it back in, and
/// Windows hands out the same letters again -- to volumes that may now be the
/// other way round, or belong to a different board entirely. The volume-set
/// comparison below cannot see that, because the letters match. Only the age
/// can bound it.
///
/// Two minutes rather than the flash gate's 1.5 seconds because the two are
/// bounding different things: that one gates a button that must stay live
/// frame by frame while a background scanner refreshes underneath it, whereas
/// this bounds a human reading a result and then acting on it in another tab.
/// Long enough to be usable, short enough that a replug cycle in between is a
/// deliberate act the user cannot have failed to notice.
inline constexpr std::chrono::milliseconds kMaxMappingAge{120000};

/// Is `result` still something to act on?
///
/// Deliberately shaped like selectionUnchangedFresh() (fwFlashController.h),
/// and for the same reasons:
///  - it takes the freshness input as an OPTIONAL age and treats nullopt as
///    stale, never as fresh: an unknown age is the least verified state there
///    is;
///  - only the answer that PERMITS action is gated on freshness. A mapping
///    already invalidated by the volumes changing is reported as such, because
///    that tells the user what actually happened;
///  - it is a free function over plain values, so it is testable without a
///    controller, a thread, or a board.
///
/// `volumesNow` must be a CURRENT reading of findRpiRp2Volumes(). The mapping
/// is fresh only when exactly one RPI-RP2 volume is mounted and it is the one
/// the identification named. Anything else -- none, two again, a different
/// letter -- means the world moved and the mapping is void.
MappingCheck mappingStillFresh(const IdentifyResult& result,
                                std::span<const std::string> volumesNow,
                                std::optional<std::chrono::milliseconds> age,
                                std::chrono::milliseconds maxAge = kMaxMappingAge);

/// Human-readable explanation of a non-Fresh MappingCheck (empty for Fresh).
std::string mappingCheckMessage(MappingCheck check);

/// The one fact `result` contributes to the rest of the app, or a default-
/// constructed (`known() == false`) VerifiedVolume when it contributes none.
///
/// This is the ONLY door between the recovery flow and everything that can
/// write to a board, and it is a door with mappingStillFresh() bolted to it:
/// anything other than MappingCheck::Fresh yields nothing at all. A caller
/// cannot get a mapping out of here without the freshness rule having been
/// applied, which is what makes "a stale mapping never authorises a write" a
/// property of the code rather than of everyone remembering to check.
///
/// Only the still-MOUNTED half of the identification comes out, deliberately.
/// The identification establishes two facts -- the volume the prober was
/// written to is the CPU it measured itself to be, and the volume still mounted
/// is the other CPU by elimination -- and both are recorded in `result`, which
/// is what the Recovery tab reads to explain itself. But the first names a
/// volume that no longer exists (writing the prober is what stopped that CPU
/// being mass storage), and a drive letter that has GONE is the one kind of
/// name most likely to come back attached to something else. Letting it
/// authorise a future write would be authorising a letter, not a CPU.
///
/// `volumesNow` must be a CURRENT reading of findRpiRp2Volumes() and `age` a
/// current identificationAge(); both are passed in rather than read here so
/// this stays a pure function testable without a board or a clock, matching
/// selectionUnchangedFresh() (fwFlashController.h).
VerifiedVolume verifiedVolumeFrom(const IdentifyResult& result,
                                   std::span<const std::string> volumesNow,
                                   std::optional<std::chrono::milliseconds> age,
                                   std::chrono::milliseconds maxAge = kMaxMappingAge);

/// "MAIN" / "DISPLAY". Shared so the probe flow's messages and the flash
/// engine's cannot word the same CPU two different ways.
const char* probeCpuName(TargetCpu cpu);

} // namespace fwog
