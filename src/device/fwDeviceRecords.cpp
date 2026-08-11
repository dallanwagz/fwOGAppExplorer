#include "device/fwDeviceRecords.h"

#include <array>
#include <fstream>
#include <string_view>

namespace fwog {

std::optional<CpuPortRecord> usbDeviceToRecord(UsbKind kind,
                                               const std::string& port,
                                               const std::string& product,
                                               uint32_t location,
                                               const std::string& volume)
{
    if (kind == UsbKind::MassStorage) {
        // A drive letter is what makes a mass-storage record actionable -- it
        // is what gets copied to. An RP2040 in its bootrom with no mount yet
        // (still enumerating, or no letter assigned) tells us nothing usable.
        if (volume.empty()) return std::nullopt;

        // An unrecognised hub port is left unattributed rather than guessed.
        // This is the whole safety property of the structural pass: it says
        // where a CPU is or it says nothing, and a drive on some port that is
        // neither MAIN nor DISPLAY -- an SD card reader, a FREE-WILi2 layout,
        // a device on a plain external hub -- must not be mistaken for a CPU.
        if (location != kHubPortMain && location != kHubPortDisplay)
            return std::nullopt;

        CpuPortRecord r;
        r.volume  = volume;
        r.product = product;
        r.isMassStorageMain    = (location == kHubPortMain);
        r.isMassStorageDisplay = (location == kHubPortDisplay);
        return r;
    }

    switch (kind) {
    case UsbKind::Serial:
    case UsbKind::SerialMain:
    case UsbKind::SerialDisplay:
        break;
    default:
        return std::nullopt;   // hubs, FTDI, ESP32: not a CPU we can locate
    }

    // A port is the only thing that makes a serial record actionable -- it is
    // what gets opened at 1200 baud.
    if (port.empty()) return std::nullopt;

    CpuPortRecord r;
    r.port    = port;
    r.product = product;
    r.isSerialMain    = (kind == UsbKind::SerialMain);
    r.isSerialDisplay = (kind == UsbKind::SerialDisplay);
    return r;
}

} // namespace fwog

#ifndef __EMSCRIPTEN__
namespace fwog {

UsbKind fromFwfinder(Fw::USBDeviceType t)
{
    switch (t) {
    case Fw::USBDeviceType::Hub:           return UsbKind::Hub;
    case Fw::USBDeviceType::Serial:        return UsbKind::Serial;
    case Fw::USBDeviceType::SerialMain:    return UsbKind::SerialMain;
    case Fw::USBDeviceType::SerialDisplay: return UsbKind::SerialDisplay;
    case Fw::USBDeviceType::MassStorage:   return UsbKind::MassStorage;
    case Fw::USBDeviceType::ESP32:         return UsbKind::ESP32;
    case Fw::USBDeviceType::FTDI:          return UsbKind::FTDI;
    default:                               return UsbKind::Other;
    }
}

namespace detail {

std::string productFromSysfs(const std::string& rawSyspath,
                             const std::string& finderName,
                             const ReadFileFn& readFile)
{
    constexpr std::string_view kSysPrefix = "/sys/";
    if (std::string_view(rawSyspath).substr(0, kSysPrefix.size()) != kSysPrefix)
        return finderName;
    if (!readFile) return finderName;

    const auto raw = readFile(rawSyspath + "/product");
    if (!raw) return finderName;

    // sysfs string attributes come back newline-terminated. Trailing CR is
    // trimmed with it so the comparison cannot depend on which the kernel
    // happened to emit -- the prefix tests downstream are exact.
    std::string value = *raw;
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();

    // A device that publishes an EMPTY iProduct has told us nothing, which is
    // not an improvement on the display name.
    return value.empty() ? finderName : value;
}

} // namespace detail

std::string productStringOf(const Fw::USBDevice& usb)
{
    return detail::productFromSysfs(usb._raw, usb.name,
        [](const std::string& path) -> std::optional<std::string> {
            std::ifstream f(path, std::ios::binary);
            if (!f) return std::nullopt;
            // Bounded, like fwVolume.cpp's INFO_UF2.TXT read and for the same
            // reason: this is a file whose size is decided by a device the user
            // plugged in. A USB string descriptor cannot exceed 126 characters,
            // so 256 bytes is the whole of any honest answer.
            std::array<char, 256> buf{};
            f.read(buf.data(), buf.size());
            return std::string(buf.data(), static_cast<std::size_t>(f.gcount()));
        });
}

std::vector<CpuPortRecord> toCpuPortRecords(const Fw::FreeWiliDevice& device)
{
    std::vector<CpuPortRecord> out;
    for (const auto& usb : device.getUSBDevices()) {
        const std::string port = usb.port.value_or(std::string{});
        // fwfinder reports mass-storage mounts as a LIST (a device can expose
        // several). Only a single unambiguous mount is taken: two mounts on one
        // bootrom device is not something an RP2040 does, and picking one of
        // them would be exactly the guess this whole path exists to avoid.
        std::string volume;
        if (usb.paths.has_value() && usb.paths->size() == 1)
            volume = usb.paths->front();

        // productStringOf(usb), NOT usb.name: the display name fwfinder builds
        // is not the USB product string on every platform, and the prefix tests
        // downstream are anchored at position 0. See productStringOf().
        if (auto r = usbDeviceToRecord(fromFwfinder(usb.kind), port, productStringOf(usb),
                                       usb.location, volume))
            out.push_back(std::move(*r));
    }
    return out;
}

} // namespace fwog
#endif
