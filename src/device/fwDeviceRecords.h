#pragma once

#include "core/fwTypes.h"

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

/// Flatten a whole FreeWili device into the records identifyCpus consumes.
std::vector<CpuPortRecord> toCpuPortRecords(const Fw::FreeWiliDevice& device);

} // namespace fwog
#endif
