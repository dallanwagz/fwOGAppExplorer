#include <doctest/doctest.h>
#include "device/fwCpuIdentify.h"

#include <vector>

using namespace fwog;

namespace {
CpuPortRecord hubMain(std::string port)    { return { std::move(port), "",  true,  false }; }
CpuPortRecord hubDisplay(std::string port) { return { std::move(port), "",  false, true  }; }
CpuPortRecord plain(std::string port, std::string product) {
    return { std::move(port), std::move(product), false, false };
}
/// A CPU sitting in the RP2040 bootrom, located by which internal-hub port its
/// mass-storage device is on.
CpuPortRecord volMain(std::string vol) {
    CpuPortRecord r; r.volume = std::move(vol); r.isMassStorageMain = true; return r;
}
CpuPortRecord volDisplay(std::string vol) {
    CpuPortRecord r; r.volume = std::move(vol); r.isMassStorageDisplay = true; return r;
}
} // namespace

TEST_CASE("a bootrom drive is identified by hub position, with no port and no probe") {
    // MAIN is in BOOTSEL (a drive, no port), DISPLAY is running (a port). Both
    // are located, from the one structural source, with nothing measured and
    // nobody asked. This is the state a user is in every time they hold BOOTSEL
    // to recover a board, and it used to report MAIN as "not identified".
    std::vector<CpuPortRecord> r{ volMain("G:/"), hubDisplay("COM73") };
    auto id = identifyCpus(r);
    CHECK(id.mainVolume  == "G:/");
    CHECK_FALSE(id.mainPort.has_value());
    CHECK(id.displayPort == "COM73");
    CHECK(id.mainSource    == IdentitySource::HubLocation);
    CHECK(id.displaySource == IdentitySource::HubLocation);
}

TEST_CASE("both CPUs in the bootrom are told apart by hub position") {
    // Two mounted RPI-RP2 drives publish the IDENTICAL bootrom USB serial, so
    // nothing they say distinguishes them -- but they are soldered to different
    // ports of the board's own hub, and that is not something firmware can
    // change. The pair that the app otherwise has to refuse as indistinguishable
    // is, structurally, not indistinguishable at all.
    std::vector<CpuPortRecord> r{ volMain("G:/"), volDisplay("H:/") };
    auto id = identifyCpus(r);
    CHECK(id.mainVolume    == "G:/");
    CHECK(id.displayVolume == "H:/");
    CHECK(id.mainSource    == IdentitySource::HubLocation);
    CHECK(id.displaySource == IdentitySource::HubLocation);
}

TEST_CASE("a CPU claimed by both a port and a drive is identified as neither") {
    // Physically impossible: a CPU runs firmware and presents a CDC port, or
    // sits in the bootrom and presents mass storage. Both at once means the
    // scan is describing a board that has already moved on -- most likely a
    // stale port record beside a live mount -- and a guess between two
    // contradicting claims is exactly what this pass must never make.
    std::vector<CpuPortRecord> r{ hubMain("COM60"), volMain("G:/") };
    auto id = identifyCpus(r);
    CHECK_FALSE(id.mainPort.has_value());
    CHECK_FALSE(id.mainVolume.has_value());
    CHECK(id.mainSource == IdentitySource::None);
}

TEST_CASE("two drives claiming the same CPU identify neither") {
    // The same exactly-one-or-nothing rule the port pass has always used,
    // applied to drives: two boards each with a MAIN CPU in BOOTSEL cannot say
    // which drive belongs to the board in front of the user.
    std::vector<CpuPortRecord> r{ volMain("G:/"), volMain("H:/") };
    auto id = identifyCpus(r);
    CHECK_FALSE(id.mainVolume.has_value());
    CHECK(id.mainSource == IdentitySource::None);
}

TEST_CASE("hub location identifies both CPUs") {
    std::vector<CpuPortRecord> r{ hubMain("COM60"), hubDisplay("COM65") };
    auto id = identifyCpus(r);
    CHECK(id.mainPort    == "COM60");
    CHECK(id.displayPort == "COM65");
    CHECK(id.mainSource    == IdentitySource::HubLocation);
    CHECK(id.displaySource == IdentitySource::HubLocation);
}

TEST_CASE("hub location wins over a contradicting product string") {
    // Structural position is authoritative; a stale product string must not
    // override where the device physically sits on the hub.
    std::vector<CpuPortRecord> r{
        { "COM60", "FWOG display bl", true, false },
    };
    auto id = identifyCpus(r);
    CHECK(id.mainPort == "COM60");
    CHECK(id.mainSource == IdentitySource::HubLocation);
    CHECK_FALSE(id.displayPort.has_value());
}

TEST_CASE("product strings identify when hub location is unavailable") {
    std::vector<CpuPortRecord> r{
        plain("COM60", "FWOG display bl"),
        plain("COM65", "FWOG main template"),
    };
    auto id = identifyCpus(r);
    CHECK(id.displayPort == "COM60");
    CHECK(id.mainPort    == "COM65");
    CHECK(id.mainSource    == IdentitySource::ProductString);
    CHECK(id.displaySource == IdentitySource::ProductString);
}

TEST_CASE("\"Pico\" identifies nothing") {
    // A CPU running an app that sets no USBD_PRODUCT reports "Pico", exactly
    // like the other one. Inferring from it is the misidentification that
    // ends with an image on the wrong CPU.
    std::vector<CpuPortRecord> r{ plain("COM60", "Pico"), plain("COM65", "Pico") };
    auto id = identifyCpus(r);
    CHECK_FALSE(id.mainPort.has_value());
    CHECK_FALSE(id.displayPort.has_value());
}

TEST_CASE("a lone unlabelled port is never inferred") {
    std::vector<CpuPortRecord> r{ plain("COM60", "") };
    auto id = identifyCpus(r);
    CHECK_FALSE(id.mainPort.has_value());
    CHECK_FALSE(id.displayPort.has_value());
}

TEST_CASE("the product prefix requires its trailing space") {
    std::vector<CpuPortRecord> r{ plain("COM60", "FWOG displayfoo") };
    auto id = identifyCpus(r);
    CHECK_FALSE(id.displayPort.has_value());
}

TEST_CASE("two ports claiming the same CPU identify neither") {
    std::vector<CpuPortRecord> r{ hubMain("COM60"), hubMain("COM61") };
    auto id = identifyCpus(r);
    CHECK_FALSE(id.mainPort.has_value());
}

TEST_CASE("an empty record list identifies nothing") {
    auto id = identifyCpus({});
    CHECK_FALSE(id.mainPort.has_value());
    CHECK_FALSE(id.displayPort.has_value());
    CHECK(id.mainSource == IdentitySource::None);
}

TEST_CASE("an unrelated USB serial device never matches") {
    std::vector<CpuPortRecord> r{ plain("COM3", "FTDI FT232R USB UART") };
    auto id = identifyCpus(r);
    CHECK_FALSE(id.mainPort.has_value());
    CHECK_FALSE(id.displayPort.has_value());
}

TEST_CASE("one CPU identified and the other not is a valid partial result") {
    // Common in practice: main is running an identifiable app, the display is
    // in the bootloader's pre-console window. The main-targeted plan must
    // still be allowed to proceed.
    std::vector<CpuPortRecord> r{ hubMain("COM60") };
    auto id = identifyCpus(r);
    CHECK(id.mainPort == "COM60");
    CHECK_FALSE(id.displayPort.has_value());
}

TEST_CASE("ambiguous hub-main candidates reject contradicting product string") {
    // Two ports both flagged as SerialMain by fwfinder: hub position is ambiguous
    // so mainPort is correctly unresolved. But even with a contradicting product
    // string, those ports must NEVER be reassigned to display role. A port carrying
    // ANY structural signal has its role decided by hub position (or ambiguous) --
    // a product string must never override it.
    std::vector<CpuPortRecord> r{
        { "COM60", "FWOG display bl", true, false },
        { "COM61", "",                true, false },
    };
    auto id = identifyCpus(r);
    CHECK_FALSE(id.mainPort.has_value());
    CHECK_FALSE(id.displayPort.has_value());
}

TEST_CASE("ambiguous hub-display candidates reject contradicting product string") {
    // Mirror case: two ports both flagged as SerialDisplay by fwfinder.
    // displayPort is ambiguous and unresolved; neither port is eligible for
    // identification as main, even if one carries "FWOG main ..." product string.
    std::vector<CpuPortRecord> r{
        { "COM60", "FWOG main template", false, true },
        { "COM61", "",                   false, true },
    };
    auto id = identifyCpus(r);
    CHECK_FALSE(id.mainPort.has_value());
    CHECK_FALSE(id.displayPort.has_value());
}

// ---------------------------------------------------------------------------
// ogBootloaderState: does this board have the wiliOGBsp display bootloader?
// Decided from the DISPLAY CPU alone, because that is where it lives.

TEST_CASE("a display running OG firmware has the bootloader") {
    // An OG APP counts, not just the bootloader itself: an app can only have
    // reached the display CPU through the bootloader, so its presence proves it.
    CpuIdentity id;
    id.displayPort    = "COM59";
    id.displayProduct = "FWOG display wilidoro 001";
    CHECK(ogBootloaderState(id) == OgBootloaderState::Present);

    id.displayProduct = "FWOG display bl 001";
    CHECK(ogBootloaderState(id) == OgBootloaderState::Present);
}

TEST_CASE("a display running non-OG firmware has no bootloader") {
    CpuIdentity id;
    id.displayPort    = "COM59";
    id.displayProduct = "FreeWili Display";
    CHECK(ogBootloaderState(id) == OgBootloaderState::Missing);
}

TEST_CASE("the FWOG prefix requires its trailing space") {
    // Same rule the product-string identification uses: without the space,
    // "FWOGGLE" would pass for OG firmware.
    CpuIdentity id;
    id.displayPort    = "COM59";
    id.displayProduct = "FWOGGLE display thing";
    CHECK(ogBootloaderState(id) == OgBootloaderState::Missing);
}

TEST_CASE("a blank display -- a bootrom drive -- has no bootloader") {
    // Nothing is installed on it at all, bootloader included.
    CpuIdentity id;
    id.displayVolume = "G:/";
    id.displaySource = IdentitySource::HubLocation;
    CHECK(ogBootloaderState(id) == OgBootloaderState::Missing);
}

TEST_CASE("a display that is neither answering nor mounted is Unknown") {
    // Nothing is claimed, so the UI says nothing. Reporting Missing here would
    // nag about every board mid-reboot.
    CHECK(ogBootloaderState(CpuIdentity{}) == OgBootloaderState::Unknown);
}

TEST_CASE("a display port reporting no product string at all is Unknown, never Missing") {
    // Some hosts simply do not report one. Claiming a working board has no
    // bootloader would send a user to erase and reflash a CPU that was fine.
    CpuIdentity id;
    id.displayPort = "COM59";
    CHECK(ogBootloaderState(id) == OgBootloaderState::Unknown);
}

TEST_CASE("a running display outranks a stale volume claim") {
    // What a CPU is RUNNING is direct evidence; a volume field is evidence that
    // nothing is installed. If both are somehow set, the port wins.
    CpuIdentity id;
    id.displayPort    = "COM59";
    id.displayProduct = "FWOG display bl 001";
    id.displayVolume  = "G:/";
    CHECK(ogBootloaderState(id) == OgBootloaderState::Present);
}

TEST_CASE("identifyCpus records the display port's product string") {
    // The field ogBootloaderState() reads has to actually get filled in.
    std::vector<CpuPortRecord> r{ hubMain("COM60"),
                                  { "COM59", "FWOG display bl 001", false, true } };
    auto id = identifyCpus(r);
    REQUIRE(id.displayPort == "COM59");
    CHECK(id.displayProduct == "FWOG display bl 001");
    CHECK(ogBootloaderState(id) == OgBootloaderState::Present);
}
