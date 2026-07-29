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
/// The open then FAILS on Windows with "A device attached to the system is not
/// functioning", because the device disappears mid-open. That error is the
/// success indicator and is indistinguishable from a genuine failure at this
/// level, so this function reports nothing at all. The CALLER decides success
/// by waiting for a volume to appear.
void touchPort1200(const std::string& port);

} // namespace fwog
