#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fwog {

/// One serial port, as the OS reports it RIGHT NOW.
struct SerialPortInfo {
    /// "COM69", "/dev/ttyACM0". Unique within one listSerialPortInfo() call
    /// on Windows, because a COM name identifies at most one attached device.
    std::string port;
    /// The USB identity of the device this port belongs to, in whatever
    /// notation the platform natively speaks. The ONLY defined operation on it
    /// is looksLikeProberUsbId() (fwCpuProbe.h), which understands every shape
    /// produced here; nothing else may parse it, and the two shapes are NOT
    /// comparable to each other.
    ///
    ///   Windows: the device instance id, upper-cased, e.g.
    ///            "USB\VID_2E8A&PID_000A&MI_00\7&29198214&0&0000".
    ///   Linux:   "usb:v2E8Ap000Ain00:3-4.1.1:1.0" -- the vendor id, product id
    ///            and bInterfaceNumber sysfs reports for the USB interface the
    ///            tty hangs off, followed by that interface's sysfs bus id.
    ///
    /// WHY THE LINUX FORM IS NOT MADE TO LOOK LIKE THE WINDOWS ONE. The tail of
    /// a Windows instance id ("7&29198214&0&0000") is a Windows devnode path.
    /// Linux has no such thing, so a string of that shape emitted here would
    /// have an invented field in it, and the "USB\...&..." notation would claim
    /// a provenance no Linux API can confirm. Every field of the form above can
    /// be checked against the running machine instead: "2e8a:000a" is what
    /// lsusb prints, and "3-4.1.1:1.0" names a real directory under
    /// /sys/bus/usb/devices. The "v<vid>p<pid>...in<interface>" spelling is
    /// borrowed from the kernel's own MODALIAS strings rather than invented.
    /// The cost is that looksLikeProberUsbId() has two branches -- but it is a
    /// pure function, so BOTH branches are compiled and tested on every
    /// platform, and neither can rot unnoticed the way a platform-gated one
    /// would.
    ///
    /// EMPTY when the platform cannot say -- the web build always, and on
    /// Linux any port whose sysfs chain does not lead to a USB interface with
    /// a readable idVendor/idProduct. Callers must treat empty as "unknown",
    /// never as "not a match".
    std::string usbId;
};

/// The sysfs attributes a Linux usbId is built out of, each exactly as the
/// kernel spells it in the corresponding file (trailing newline and all --
/// makeLinuxUsbId() trims).
struct LinuxUsbAttrs {
    /// From <usb device>/idVendor, e.g. "093c". Four hex digits.
    std::string idVendor;
    /// From <usb device>/idProduct, e.g. "2054". Four hex digits.
    std::string idProduct;
    /// From <usb interface>/bInterfaceNumber, e.g. "00". Two hex digits, and
    /// legitimately absent for a device that is not composite.
    std::string bInterfaceNumber;
    /// The USB interface's sysfs directory name, e.g. "3-4.1.1:1.0". Optional:
    /// it makes the id name one physical port rather than a class of device,
    /// which matters for reading a log, not for matching.
    std::string busId;
};

/// Build the Linux form of SerialPortInfo::usbId from sysfs attributes.
///
/// Returns EMPTY -- "unknown" -- unless both idVendor and idProduct are
/// present and are exactly four hex digits. A half-read identity is worse than
/// no identity: looksLikeProberUsbId() would be asked to judge a string whose
/// missing half it cannot see is missing, and the header's promise that empty
/// means "the platform could not say" is the only thing that keeps "unknown"
/// from being read as "not a match".
///
/// Exposed, and compiled on every platform rather than only on Linux, so that
/// the one piece of this file with a format to get wrong is tested directly
/// and is tested by the Windows build too.
std::string makeLinuxUsbId(const LinuxUsbAttrs& attrs);

/// Read the usbId for one /sys/class/tty/<name> directory, or "" if that tty
/// has no USB device behind it.
///
/// THE WALK IS NOT A FIXED NUMBER OF "..", because the two drivers that matter
/// do not put the tty at the same depth. Captured from the attached board:
///
///   ttyACM0  device -> .../3-4.1.1/3-4.1.1:1.0            (the USB interface)
///   ttyUSB0  device -> .../3-4.1.3/3-4.1.3:1.0/ttyUSB0    (one level deeper)
///
/// cdc_acm hangs the tty off the interface directory itself; usb-serial
/// inserts a port device in between. Hard-coding either depth gets the other
/// driver's ports wrong, and gets them wrong SILENTLY -- an empty usbId reads
/// as "unknown", so a wrong walk would quietly demote every FTDI-style port to
/// unidentified rather than failing anywhere anyone would look. So it climbs
/// until it finds a directory holding bInterfaceNumber -- the attribute that
/// MEANS "this is a USB interface" -- and reads idVendor/idProduct from that
/// directory's parent, which is the USB device. Bounded at four hops so that a
/// tty on some bus nobody has thought about cannot walk out of /sys/devices
/// and up into the root of the filesystem.
///
/// Takes the directory as an argument rather than reading /sys directly so
/// that the walk can be tested against a FABRICATED tree -- both layouts
/// above, and the ones that must produce "" -- without a board attached and
/// without /sys existing at all. Nothing about it is Linux-specific except
/// what it expects to find, so it is built and tested on every platform for
/// the same reason makeLinuxUsbId() is.
std::string usbIdForSysfsTtyDir(const std::filesystem::path& sysClassTtyEntry);

/// Is `name` the name of a USB serial port device node -- "ttyACM" or "ttyUSB"
/// followed by at least one digit and nothing else?
///
/// EXACT rather than a prefix test, which is what listSerialPortInfo() used to
/// do. A prefix test accepts "ttyACMfoo" and "ttyUSB" with no number at all,
/// and while nothing registers such a name today, the reason to be exact is
/// that this predicate decides what gets OPENED. Both "ttyACM1" and "ttyACM10"
/// are accepted, because both are real ports; there is no collision between
/// them to resolve, only a distinction to preserve.
///
/// Exposed for the same reason as makeLinuxUsbId(): it is testable without a
/// machine in any particular state, and it is compiled everywhere.
bool isUsbSerialPortName(std::string_view name);

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
/// ON LINUX THE SAME DISTINCTION APPLIES, one level down. This used to scan
/// /dev for names beginning "ttyACM"/"ttyUSB", which is the same species of
/// question as reading SERIALCOMM: /dev is a list of NAMES, and a name there
/// answers "may this be opened", not "is a device behind it". /sys/class/tty is
/// the kernel's list of tty devices that EXIST, and each entry carries a
/// `device` symlink into the device tree -- which is both the more direct
/// question and the only way to reach the USB identity at all. Asking /dev and
/// then deriving the sysfs path anyway would be asking the same question twice
/// and believing the weaker answer.
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
///
/// EVERY platform branch now ASKS for DTR rather than hoping for it. Windows
/// does it twice (DTR_CONTROL_ENABLE in the DCB, then EscapeCommFunction
/// SETDTR); Linux does it with TIOCMBIS. On the machine this was measured on,
/// Linux happened to raise DTR by itself on every open of a cdc_acm port --
/// but tcsetattr() was measured NOT to restore it once it was low, so nothing
/// in the sequence this function performs would repair a port that came up
/// without it. A contract the header states is one the code should assert, not
/// one it should inherit from behaviour it never requested.
std::optional<std::string> readSerialLine(const std::string& port, int timeoutMs);

} // namespace fwog
