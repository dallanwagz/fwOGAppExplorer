#pragma once

#include <string>

namespace fwog {

/// Reboot the CPU behind `port` into the UF2 bootloader.
///
/// Opening a pico_stdio_usb CDC at 1200 baud is the whole mechanism:
/// pico_usb_reset's tud_cdc_line_coding_cb() sees
/// PICO_USB_RESET_MAGIC_BAUD_RATE and calls rom_reset_usb_boot_extra(). No DTR
/// handshake is involved -- setting the line coding is enough, which is why
/// merely opening triggers it.
///
/// The "no DTR handshake" half of that is now MEASURED on Linux against a real
/// FreeWili 1-OG, on both CPUs, and it holds: the reset fires while the port is
/// still open with DTR high, and a DTR cycle at any other speed does nothing.
/// What does NOT carry over is "merely opening triggers it" -- that describes a
/// handle inheriting a remembered 1200-baud DCB, and on Linux a port opens at
/// whatever speed it was left at (B9600 on a freshly enumerated one). It is the
/// WRITE of the 1200-baud line coding that resets the CPU, so a caller must not
/// expect an open() on its own to do anything. fwSerialTouch.cpp records the
/// experiments that separate those.
///
/// The open then FAILS on Windows with "A device attached to the system is not
/// functioning", because the device disappears mid-open. That error is the
/// success indicator and is indistinguishable from a genuine failure at this
/// level, so this function reports nothing at all. The CALLER decides success
/// by waiting for a volume to appear.
void touchPort1200(const std::string& port);

} // namespace fwog
