#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fwog {

// --------------------------------------------------------------------------
// Platform capability
// --------------------------------------------------------------------------

/// False where device detection and flashing cannot work at all (the web
/// build). Guards UI affordances so nothing is offered that cannot happen.
#if defined(__EMSCRIPTEN__)
inline constexpr bool kDeviceSupportAvailable = false;
#else
inline constexpr bool kDeviceSupportAvailable = true;
#endif

/// The one sentence shown wherever flashing is unavailable because the
/// platform itself cannot reach the hardware: the device bar's notice and
/// every Flash button's disabled reason both read from here, so the two can
/// never drift apart.
///
/// The em dash is spelled as its UTF-8 bytes rather than as a literal
/// character, matching what fwTabDefaultFirmware.cpp already does, so the
/// copy cannot be changed by a source-encoding accident. Fonts::initialize()
/// loads General Punctuation (see fwFonts.cpp) so it renders rather than
/// falling back to '?'.
constexpr std::string_view platformLimitationNotice()
{
    return "Device detection and flashing need the desktop app \xE2\x80\x94 a browser "
           "cannot reach USB mass storage or serial ports. Everything else on this "
           "page works.";
}

// --------------------------------------------------------------------------
// Device identification
// --------------------------------------------------------------------------

enum class TargetCpu { Main, Display };

/// How a CPU was identified. Surfaced in the UI so the user can see *how* the
/// app decided, not only what it decided.
enum class IdentitySource {
    None,           ///< not identified
    HubLocation,    ///< fwfinder USBDeviceType::SerialMain / SerialDisplay
    ProductString,  ///< "FWOG main " / "FWOG display " USB product prefix
    /// The Recovery tab's CPU prober was written to one of two mounted RPI-RP2
    /// volumes and reported, over its own USB CDC, which CPU it had landed on;
    /// the other volume of the pair is therefore the other CPU, by elimination
    /// (see fwCpuProbe.h). This names a VOLUME rather than a port -- a CPU
    /// sitting in the RP2040 bootrom publishes no serial port at all, which is
    /// exactly why the other two sources can never speak for it.
    ///
    /// It is the STRONGEST of the three, not a fallback dressed up as one. Hub
    /// position and a USB product string are both statements a device makes
    /// about itself over USB; a probe result is a physical measurement of the
    /// silicon -- the CC1101 sub-GHz radio is wired to the MAIN CPU only, and
    /// the prober went and looked. It is also the shortest-lived: it is a claim
    /// about a DRIVE LETTER, and a drive letter is a reusable name, so it is
    /// carried only while the mounted volume set is exactly what it was
    /// established against and only for kMaxMappingAge (fwCpuProbe.h).
    VerifiedProbe,
};

/// One serial port belonging to a candidate CPU, flattened out of fwfinder so
/// that fwCpuIdentify stays pure and testable without a board.
struct CpuPortRecord {
    std::string port;      ///< "COM60" or "/dev/ttyACM0"
    std::string product;   ///< USB product string; empty when unavailable
    bool isSerialMain    = false;  ///< fwfinder said USBDeviceType::SerialMain
    bool isSerialDisplay = false;  ///< fwfinder said USBDeviceType::SerialDisplay

    /// The mounted mass-storage path of an RP2040 sitting in its bootrom
    /// ("G:/"), empty for a serial record. A record has EITHER a port or a
    /// volume, never both: a CPU running firmware presents a CDC port, a CPU in
    /// the bootrom presents mass storage, and no CPU does both at once.
    std::string volume;
    /// The bootrom drive above, on the FreeWili's internal hub's MAIN/DISPLAY
    /// port respectively. This is the SAME structural fact as isSerialMain /
    /// isSerialDisplay -- position on the board's own hub -- just arriving on a
    /// device fwfinder types as MassStorage rather than as a serial port, so
    /// the port number has to be read here instead of coming pre-resolved.
    ///
    /// It is what makes a CPU in the bootrom identifiable at all. Both RP2040s
    /// present the identical bootrom USB serial, so the drives cannot be told
    /// apart from anything they themselves report -- but they are soldered to
    /// different ports of the internal hub, and that does not change with what
    /// firmware is (or is not) on them.
    bool isMassStorageMain    = false;
    bool isMassStorageDisplay = false;

    /// The USB serial string of the CDC device behind `port`, empty when the
    /// host did not report one or for a volume record. Under the pico-sdk
    /// (both the OG BSP and the original firmware) this is the RP2040's flash
    /// unique ID -- a per-chip constant that survives reflashing -- and it is
    /// what identifies the BOARD when the FTDI serial fwfinder normally reads
    /// is absent (see BoardFingerprint, fwDeviceModel.h). Deliberately not
    /// taken from a bootrom drive: every RP2040's bootrom publishes the same
    /// serial, so that one names nothing.
    std::string serial;

    bool operator==(const CpuPortRecord&) const = default;
};

struct CpuIdentity {
    std::optional<std::string> mainPort;
    std::optional<std::string> displayPort;
    IdentitySource mainSource    = IdentitySource::None;
    IdentitySource displaySource = IdentitySource::None;

    /// The mounted RPI-RP2 volume a VERIFIED CPU probe named for this CPU, or
    /// nullopt. Set only by withVerifiedVolume() (fwDeviceModel.h), and only
    /// from a probe result that is still fresh -- see IdentitySource's
    /// VerifiedProbe comment and mappingStillFresh() (fwCpuProbe.h).
    ///
    /// This is a SEPARATE field from the port rather than another way of
    /// filling it in, because the two are not interchangeable: a port is
    /// something to TOUCH at 1200 baud, a volume is something to COPY TO, and
    /// confusing the two would have the engine open a drive letter as a serial
    /// port. Both are, however, the same KIND of fact -- "this CPU is over
    /// there" -- which is why they share `mainSource`/`displaySource` and are
    /// rendered by the one describeIdentity() line.
    ///
    /// At most one of mainVolume/displayVolume is ever set, because the mapping
    /// it comes from names exactly one currently-mounted volume; the CPU whose
    /// volume the prober took over is no longer mass storage at all.
    std::optional<std::string> mainVolume;
    std::optional<std::string> displayVolume;

    /// The USB serial of the CDC device each CPU's PORT was resolved from --
    /// the RP2040 flash unique ID, a per-chip constant -- or empty when that
    /// CPU is not answering on a port (a bootrom drive carries no usable
    /// serial). Carried so the BOARD can be recognised across a scan when the
    /// FTDI serial is missing, which on an OG board running OG firmware is
    /// always: see BoardFingerprint (fwDeviceModel.h).
    std::string mainChipSerial;
    std::string displayChipSerial;

    /// The USB product string of the resolved DISPLAY port, empty when that CPU
    /// is not answering on one.
    ///
    /// Carried because it is the only thing that says WHAT the display CPU is
    /// running. Under the wiliOGbsp contract the product string is
    /// `FWOG <cpu> <name> <version>`, so a display port that does not begin
    /// `FWOG ` is running something that is not OG firmware at all -- which
    /// means the board has no OG bootloader. See ogBootloaderState().
    std::string displayProduct;
};

/// The one fact a completed, still-valid CPU identification contributes to the
/// rest of the app: THIS mounted RPI-RP2 volume belongs to THAT CPU, measured.
///
/// Deliberately a bare pair of values with no freshness of its own. Producing
/// one is where the freshness rule is applied (verifiedVolumeFrom(),
/// fwCpuProbe.h, which delegates to mappingStillFresh()); holding one is not.
/// A default-constructed instance -- `volume` empty -- means "there is no such
/// fact", which is what every consumer must treat as the safe default and what
/// clearing an expired identification produces.
struct VerifiedVolume {
    std::string volume;
    TargetCpu   cpu = TargetCpu::Main;

    /// True when this actually names something. Never assume a non-empty
    /// `cpu` alone means anything -- TargetCpu has no "none".
    bool known() const { return !volume.empty(); }

    bool operator==(const VerifiedVolume&) const = default;
};

/// The volume a verified probe named for `cpu`, or nullopt. Free function
/// rather than a member so the CPU-selecting ternary this replaces exists in
/// exactly one place; the engine, the plan builder and the disabled-reason all
/// ask the same question and must not each spell it out.
inline const std::optional<std::string>& volumeForCpu(const CpuIdentity& id, TargetCpu cpu)
{
    return cpu == TargetCpu::Main ? id.mainVolume : id.displayVolume;
}

/// The port identified for `cpu`, or nullopt. Companion to volumeForCpu().
inline const std::optional<std::string>& portForCpu(const CpuIdentity& id, TargetCpu cpu)
{
    return cpu == TargetCpu::Main ? id.mainPort : id.displayPort;
}

/// The other CPU. There are two.
constexpr TargetCpu otherCpu(TargetCpu cpu)
{
    return cpu == TargetCpu::Main ? TargetCpu::Display : TargetCpu::Main;
}

// --------------------------------------------------------------------------
// Catalog
// --------------------------------------------------------------------------

enum class FlashScheme {
    OgApp,              ///< one UF2 -> MAIN; display image rides the link
    DisplayBootloader,  ///< bl_display.uf2 -> DISPLAY; once per board
    LegacyDirect,       ///< main UF2 -> MAIN, then display UF2 -> DISPLAY
};

enum class CatalogSource { Embedded, Local, Remote, Unlisted };

/// Where a UF2's bytes come from. Exactly one of the three is non-empty.
struct ImageRef {
    std::string embeddedId;  ///< key into the generated embedded manifest
    std::string localPath;
    std::string url;

    bool operator==(const ImageRef&) const = default;
};

struct Uf2Asset {
    TargetCpu   cpu = TargetCpu::Main;
    ImageRef    ref;
    std::string sha256;      ///< lowercase hex; empty when unknown
    uint64_t    size = 0;    ///< bytes; 0 when unknown
    int         order = 0;   ///< ascending flash order within a plan

    bool operator==(const Uf2Asset&) const = default;
};

struct CatalogEntry {
    std::string slug, name, tagline, description, author, github, category;
    std::vector<std::string> tags;
    std::string version, updated;

    FlashScheme scheme = FlashScheme::OgApp;
    bool        defaultFirmware = false;
    std::vector<Uf2Asset> uf2;
    CatalogSource source = CatalogSource::Remote;

    /// True when `scheme` was defaulted rather than declared. Only Unlisted
    /// entries may be retargeted to the display CPU, and only these are.
    bool schemeInferred = false;
};

// --------------------------------------------------------------------------
// Flashing
// --------------------------------------------------------------------------

/// What a step does to the CPU it targets. The distinction is not cosmetic:
/// an Erase step writes the standard Pico flash-erase image, which leaves the
/// target RP2040 with blank flash -- and a blank RP2040 re-enumerates RPI-RP2
/// BY ITSELF, with no button and no 1200-baud touch. The next step of the same
/// plan therefore meets a volume nobody touched, which is the whole reason the
/// engine has to be able to tell the two kinds of step apart. See
/// runFlashPlan()'s erase-reboot handling in fwFlashEngine.cpp.
enum class StepAction {
    Write,   ///< copy firmware (an app, a display image, a bootloader)
    Erase,   ///< copy flash_nuke.uf2: wipes flash, reboots into the bootrom
};

struct FlashStep {
    ImageRef    image;
    TargetCpu   cpu = TargetCpu::Main;
    std::string sha256;
    std::string description;
    /// Defaults to Write, which is the conservative answer: a step that is
    /// wrongly labelled Write only ever costs a typed confirmation, whereas
    /// one wrongly labelled Erase would let the engine skip one.
    StepAction  action = StepAction::Write;

    bool operator==(const FlashStep&) const = default;
};

/// What the engine must do about the volumes currently mounted.
enum class VolumeState {
    NoneMounted,      ///< normal: touch the port, then wait
    OursAfterTouch,   ///< we created it; proceed
    ForeignMounted,   ///< present before we touched; demands typed confirmation
    Ambiguous,        ///< two or more; refuse, they cannot be told apart
    /// The immediately preceding step of THIS plan erased THIS SAME CPU and
    /// we then watched every volume go away. Whatever is (or is about to be)
    /// mounted now is that CPU coming back on its own -- an erased RP2040
    /// re-enumerates RPI-RP2 unprompted. Distinct from ForeignMounted, which
    /// stays exactly as strict as it was: this state is only ever produced
    /// when the plan itself caused the reboot, one step ago, on the same CPU.
    ExpectedAfterErase,
    /// Exactly one volume is mounted, and a still-valid CPU probe result names
    /// it as THE CPU THIS STEP TARGETS (CpuIdentity::mainVolume/displayVolume).
    ///
    /// This is NOT a relaxation of ForeignMounted; it is ForeignMounted's
    /// precondition no longer holding. The typed confirmation exists because a
    /// mount carries no evidence of which CPU it belongs to -- that is what the
    /// prompt says, in as many words. A probe result is exactly that evidence,
    /// and stronger than anything the port list can offer: the CC1101 radio is
    /// wired to the MAIN CPU only, and the prober went and looked. Once the
    /// question the confirmation asks has been answered by measurement, asking
    /// it again teaches the user to type CPU names without reading them, which
    /// is the one habit every guard in this project depends on them not having.
    MappedToTarget,
    /// Exactly one volume is mounted, and a still-valid CPU probe result names
    /// it as THE OTHER CPU. Strictly stronger than ForeignMounted in the
    /// refusing direction: the app is not unsure which CPU this drive is, it
    /// knows, and it knows this step must not be written there. Before this
    /// state existed, that case reached RequireTypedConfirmation and a user who
    /// typed the name they were prompted for got a wrong-CPU write.
    MappedToOtherCpu,
};

} // namespace fwog
