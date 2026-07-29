#pragma once

#include <optional>
#include <string>
#include <vector>

namespace fwog {

/// One serial port, as the OS reports it RIGHT NOW.
struct SerialPortInfo {
    /// "COM69", "/dev/ttyACM0". Unique within one listSerialPortInfo() call
    /// on Windows, because a COM name identifies at most one attached device.
    std::string port;
    /// The device instance id, upper-cased -- on Windows
    /// "USB\VID_2E8A&PID_000A&MI_00\7&29198214&0&0000", which carries the USB
    /// vendor/product/interface the port belongs to. EMPTY when the platform
    /// cannot say (every non-Windows build today). Callers must treat empty as
    /// "unknown", never as "not a match".
    std::string usbId;
};

/// Every serial port that is PRESENTLY ATTACHED, with the USB identity of the
/// device behind it where the OS will say.
///
/// WHY THIS IS NOT HKLM\HARDWARE\DEVICEMAP\SERIALCOMM, which is what this file
/// used to read. That key is not a list of ports that exist; it is a list of
/// port names that have ever been handed out and not always cleaned up.
/// Captured on the developer's machine while the bug this API exists to fix
/// was reproducing, SERIALCOMM held 29 values naming 5 distinct COM ports --
/// COM69 four times over, COM68 twice, COM59 and COM24 many times each -- while
/// exactly TWO serial ports were attached to the machine. Windows also reuses
/// COM numbers, so those stale names are not merely noise: a name in that list
/// can name a device that is gone AND a device that is present, at the same
/// time. Anything that reasons about ports by comparing two SERIALCOMM
/// snapshots is therefore reasoning about a list that does not describe the
/// machine.
///
/// SetupDiGetClassDevs over GUID_DEVCLASS_PORTS with DIGCF_PRESENT asks the
/// question that was actually meant -- "which ports are plugged in" -- and
/// hands back the device instance id as well, which is what lets a caller
/// recognise a specific device rather than merely notice that a name appeared.
///
/// Deliberately NOT routed through fwfinder. The CPU-identification flow runs
/// on a worker thread, DeviceModel/fwFinderManager belong to the UI thread
/// (see makeProductionFlashIo's comment in fwFlashController.h), and the
/// device the prober enumerates is not a FreeWili device at all as far as
/// fwfinder is concerned -- it is a bare CDC port with the stock Raspberry Pi
/// VID:PID. A plain OS-level port list is both the correct question and the
/// only one that can be asked safely from a worker.
///
/// Returns empty on any failure, and on the web build. A caller that cannot
/// tell "no ports" from "could not look" must fail closed on both, and every
/// caller here does.
std::vector<SerialPortInfo> listSerialPortInfo();

/// The names from listSerialPortInfo(), deduplicated.
///
/// Order is whatever the OS reports and must not be relied on. The duplicate
/// removal is not tidiness: a list that names one port twice makes "how many
/// ports are there" and "how many appeared" both wrong, and a caller counting
/// them reaches a confident wrong conclusion rather than an obvious failure.
std::vector<std::string> listSerialPorts();

/// Drop repeated names, keeping the first occurrence of each and the relative
/// order of what survives. Exposed so that the deduplication listSerialPorts()
/// depends on is testable without a machine in a particular state.
std::vector<std::string> dedupePortNames(std::vector<std::string> ports);

/// Open `port`, read ONE newline-terminated line, close it again. Returns
/// nullopt if the port could not be opened, or if no complete line arrived
/// within `timeoutMs`.
///
/// The line is returned WITHOUT its terminator and without trailing CR, but is
/// otherwise unmodified -- interpreting it is the caller's job (see
/// parseProbeLine in fwCpuProbe.h, which is strict about what it accepts).
///
/// THE BAUD RATE IS NOT ARBITRARY. This opens at 115200, and the one value it
/// must never use is 1200: opening a pico_stdio_usb CDC at 1200 baud reboots
/// that CPU into its bootloader (that is exactly what touchPort1200() in
/// fwSerialTouch.h exists to do). A read that silently rebooted the device it
/// was reading from would destroy the very evidence it was opened to collect.
///
/// DTR is asserted on open, and that is load-bearing rather than cosmetic:
/// pico_stdio_usb's out_chars() only writes when tud_cdc_connected() is true,
/// which on the device side means the host has raised DTR. Without it the
/// prober runs, probes, and prints into a void, and this function times out on
/// a perfectly healthy board.
std::optional<std::string> readSerialLine(const std::string& port, int timeoutMs);

} // namespace fwog
