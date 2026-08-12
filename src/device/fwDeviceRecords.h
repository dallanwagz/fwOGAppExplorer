#pragma once

#include "core/fwTypes.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fwog {

/// Mirror of Fw::USBDeviceType. Duplicated deliberately so the mapping below
/// stays a pure function testable without fwfinder, and so the web build --
/// where fwfinder does not exist -- still compiles this header.
enum class UsbKind { Hub, Serial, SerialMain, SerialDisplay, MassStorage, ESP32, FTDI, Other };

/// The FreeWili Classic hub port each CPU is soldered to. Mirrors
/// Fw::USBHubPortLocation, duplicated for the same reason UsbKind is: this
/// header must compile on the web build, where fwfinder does not exist.
constexpr uint32_t kHubPortMain    = 1;
constexpr uint32_t kHubPortDisplay = 2;

/// Flatten one USB device into a CpuPortRecord, or nothing when it carries
/// nothing that locates a CPU.
///
/// `location` is the device's port number on the board's internal hub, and
/// `volume` its mounted mass-storage path (empty unless `kind` is MassStorage).
///
/// Mass storage is NOT skipped, and that is the point of this function's
/// current shape. It used to be dropped along with hubs and FTDI as "not
/// touchable", which conflated two different things: a bootrom drive indeed
/// cannot be touched at 1200 baud, but it is still one of the two CPUs, sitting
/// at a known hub port, with a drive letter to write to. Discarding it is what
/// made a CPU in the bootrom show as "not identified" and pushed the app into
/// asking the user, or running a probe, for a fact the USB tree already stated.
std::optional<CpuPortRecord> usbDeviceToRecord(UsbKind kind,
                                               const std::string& port,
                                               const std::string& product,
                                               uint32_t location = 0,
                                               const std::string& volume = {});

} // namespace fwog

#ifndef __EMSCRIPTEN__
#include <fwfinder.hpp>

namespace fwog {

/// Map fwfinder's device-type enum onto our mirror. Exposed (not file-local)
/// so its exhaustiveness can be unit tested directly: this table is the most
/// safety-critical piece of new logic here -- a transposed entry (e.g.
/// SerialMain mapped to UsbKind::SerialDisplay) would route a main-CPU image
/// at a display CPU with nothing in the diff to catch it.
UsbKind fromFwfinder(Fw::USBDeviceType t);

/// The bare USB product string of one enumerated device -- the thing
/// CpuPortRecord::product is documented to hold, and on Linux was not.
///
/// WHY THIS EXISTS. Two prefix tests read that field, and both are anchored at
/// position 0: identifyCpus()' rule-5 signal ("FWOG main " / "FWOG display ")
/// and ogBootloaderState()'s "FWOG " test. fwfinder does not hand us a product
/// string, it hands us a DISPLAY NAME, and the two are only the same thing on
/// one of the three platforms:
///
///   Linux   `manufacturer + " " + product`      (fwfinder_linux.cpp)
///   macOS   `manuName + " " + productName`      (fwfinder_mac.cpp)
///   Windows `busDescription + " (" + description + ")"`, where busDescription
///           is DEVPKEY_Device_BusReportedDeviceDesc -- the USB product string
///
/// So the prefix survives at position 0 on Windows and is pushed off it
/// everywhere else. This is not hypothetical and it is not about firmware that
/// does not exist yet: the display bootloader this app installs declares
/// manufacturer "FreeWili OG" and product "FWOG display bl 001" (measured in
/// firmware/bl_display.uf2's device descriptor, 093C:2055), so on Linux it
/// arrives as "FreeWili OG FWOG display bl 001". Neither prefix matches.
///
/// What that costs, stated narrowly, because the obvious answer is wrong: it is
/// NOT that identification signal 2 stops working. Signal 2 cannot fire for a
/// 1-OG's own CPUs on ANY platform, Windows included -- fwfinder maps
/// 093C:2054/2055 straight to SerialMain/SerialDisplay, and identifyCpus()'s
/// second pass skips structurally-flagged records by design
/// (fwCpuIdentify.cpp). Fixing the string does not make that pass reachable and
/// this function should not be read as claiming it does.
///
/// The cost is the other one, and it is enough on its own: a board that HAS the
/// OG bootloader was reported as Missing. That is the false negative
/// ogBootloaderState()'s own comment singles out as the expensive direction to
/// be wrong in, because it sends the user to erase and reflash a CPU that was
/// fine. Measured on the attached board, both directions -- with the fix the
/// banner is absent, and with only the wiring line reverted the same board
/// renders "No OG bootloader on this board". See docs/hardware-verification.md.
///
/// So ask the kernel instead of parsing the display name back apart, which
/// cannot be done: nothing in "FreeWili OG FWOG display bl 001" says where the
/// manufacturer ends.
std::string productStringOf(const Fw::USBDevice& usb);

/// Flatten a whole FreeWili device into the records identifyCpus consumes.
std::vector<CpuPortRecord> toCpuPortRecords(const Fw::FreeWiliDevice& device);

namespace detail {

/// Read a whole (small) file, or nullopt. The seam productStringOf() binds to
/// the real filesystem and tests bind to a table.
using ReadFileFn = std::function<std::optional<std::string>(const std::string& path)>;

/// productStringOf()'s decision, with the filesystem lifted out.
///
/// `rawSyspath` is fwfinder's `USBDevice::_raw`, which is the udev syspath on
/// Linux and a device instance id on Windows. That difference is the guard:
/// only a path under `/sys/` is followed, so the Windows string
/// ("USB\\VID_093C&PID_2055\\...") can never be turned into a file read and the
/// Windows branch provably keeps the behaviour it already had. It is a value
/// test rather than an `#ifdef` precisely so that both halves are reachable
/// from a test on either platform.
///
/// FALLS BACK RATHER THAN BLANKING, at every step -- not a path under /sys, no
/// reader, an unreadable file, an empty one. `finderName` is what this field
/// has always contained, so falling back to it is a return to the status quo;
/// returning nothing would newly make ogBootloaderState() answer Unknown for
/// boards it used to answer for at all.
std::string productFromSysfs(const std::string& rawSyspath,
                             const std::string& finderName,
                             const ReadFileFn& readFile);

} // namespace detail

} // namespace fwog
#endif
