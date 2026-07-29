#include "device/fwDeviceRecords.h"

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

        if (auto r = usbDeviceToRecord(fromFwfinder(usb.kind), port, usb.name,
                                       usb.location, volume))
            out.push_back(std::move(*r));
    }
    return out;
}

} // namespace fwog
#endif
