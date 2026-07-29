#include <doctest/doctest.h>
#include "device/fwDeviceRecords.h"

using namespace fwog;

TEST_CASE("a SerialMain USB device becomes a main record") {
    auto r = usbDeviceToRecord(UsbKind::SerialMain, "COM60", "Pico");
    REQUIRE(r.has_value());
    CHECK(r->port == "COM60");
    CHECK(r->isSerialMain == true);
    CHECK(r->isSerialDisplay == false);
}

TEST_CASE("a SerialDisplay USB device becomes a display record") {
    auto r = usbDeviceToRecord(UsbKind::SerialDisplay, "COM65", "");
    REQUIRE(r.has_value());
    CHECK(r->isSerialDisplay == true);
    CHECK(r->isSerialMain == false);
}

TEST_CASE("a plain Serial device becomes a record with neither flag") {
    // Still useful: the product-string fallback in identifyCpus reads these.
    auto r = usbDeviceToRecord(UsbKind::Serial, "COM3", "FWOG main template");
    REQUIRE(r.has_value());
    CHECK(r->isSerialMain == false);
    CHECK(r->isSerialDisplay == false);
    CHECK(r->product == "FWOG main template");
}

TEST_CASE("non-serial USB devices produce no record") {
    CHECK_FALSE(usbDeviceToRecord(UsbKind::Hub, "", "").has_value());
    CHECK_FALSE(usbDeviceToRecord(UsbKind::MassStorage, "", "").has_value());
    CHECK_FALSE(usbDeviceToRecord(UsbKind::FTDI, "", "").has_value());
    CHECK_FALSE(usbDeviceToRecord(UsbKind::ESP32, "", "").has_value());
}

TEST_CASE("a mounted bootrom drive on the hub's MAIN port becomes a main volume record") {
    // The fact that used to be thrown away. A CPU sitting in the RP2040 bootrom
    // publishes no serial port, so it can only ever be located this way.
    auto r = usbDeviceToRecord(UsbKind::MassStorage, "", "RP2 Boot", kHubPortMain, "G:/");
    REQUIRE(r.has_value());
    CHECK(r->volume == "G:/");
    CHECK(r->isMassStorageMain == true);
    CHECK(r->isMassStorageDisplay == false);
    // A volume record is not a port record: nothing here may be touched at
    // 1200 baud, and a port field filled in by accident would invite exactly
    // that.
    CHECK(r->port.empty());
    CHECK(r->isSerialMain == false);
}

TEST_CASE("a mounted bootrom drive on the hub's DISPLAY port becomes a display volume record") {
    auto r = usbDeviceToRecord(UsbKind::MassStorage, "", "", kHubPortDisplay, "H:/");
    REQUIRE(r.has_value());
    CHECK(r->volume == "H:/");
    CHECK(r->isMassStorageDisplay == true);
    CHECK(r->isMassStorageMain == false);
}

TEST_CASE("a bootrom drive with no mount point yet produces no record") {
    // Enumerated but not yet assigned a drive letter. There is nothing to copy
    // to, so there is nothing to say.
    CHECK_FALSE(usbDeviceToRecord(UsbKind::MassStorage, "", "", kHubPortMain, "").has_value());
}

TEST_CASE("mass storage on a hub port that is neither CPU is never attributed") {
    // An SD card reader, a FREE-WILi2 layout, a drive on a plain external hub.
    // The structural pass says where a CPU is or says nothing; it must never
    // promote an unrelated drive into being one of the two CPUs.
    CHECK_FALSE(usbDeviceToRecord(UsbKind::MassStorage, "", "", 0, "G:/").has_value());
    CHECK_FALSE(usbDeviceToRecord(UsbKind::MassStorage, "", "", 3, "G:/").has_value());
    CHECK_FALSE(usbDeviceToRecord(UsbKind::MassStorage, "", "", 6, "G:/").has_value());
}

TEST_CASE("a serial device with no port produces no record") {
    // A port is the only thing that makes a record actionable -- it is what
    // gets touched at 1200 baud.
    CHECK_FALSE(usbDeviceToRecord(UsbKind::SerialMain, "", "Pico").has_value());
}

#ifndef __EMSCRIPTEN__
// fromFwfinder is the most safety-critical piece of new logic in this task:
// a transposed entry here (e.g. SerialMain mapped to UsbKind::SerialDisplay)
// would route a main-CPU image at a display CPU, and nothing else in the
// diff would catch it. One assertion per enumerator, naming both sides, so a
// swap is visible directly in the failing assertion text.
TEST_CASE("fromFwfinder maps every Fw::USBDeviceType to its matching UsbKind") {
    CHECK(fromFwfinder(Fw::USBDeviceType::Hub)           == UsbKind::Hub);
    CHECK(fromFwfinder(Fw::USBDeviceType::Serial)        == UsbKind::Serial);
    CHECK(fromFwfinder(Fw::USBDeviceType::SerialMain)    == UsbKind::SerialMain);
    CHECK(fromFwfinder(Fw::USBDeviceType::SerialDisplay) == UsbKind::SerialDisplay);
    CHECK(fromFwfinder(Fw::USBDeviceType::MassStorage)   == UsbKind::MassStorage);
    CHECK(fromFwfinder(Fw::USBDeviceType::ESP32)         == UsbKind::ESP32);
    CHECK(fromFwfinder(Fw::USBDeviceType::FTDI)          == UsbKind::FTDI);
    CHECK(fromFwfinder(Fw::USBDeviceType::Other)         == UsbKind::Other);
}
#endif
