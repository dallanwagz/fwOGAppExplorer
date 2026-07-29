#pragma once

#include "core/fwTypes.h"

#include <span>

namespace fwog {

/// Resolve which port belongs to which CPU.
///
/// Order: hub port location (authoritative), then USB product string prefix,
/// then nothing. Zero matches and more than one match both yield no result --
/// this function never guesses. A partial result (one CPU found, the other
/// not) is normal and valid.
///
/// Pure: no fwfinder, no I/O. Callers flatten Fw::FreeWiliDevices into
/// CpuPortRecord first.
CpuIdentity identifyCpus(std::span<const CpuPortRecord> records);

/// Whether this board has the wiliOGBsp display bootloader.
///
/// The bootloader is what makes an OG app runnable at all, so a board without
/// it cannot use anything in the App Explorer. This is decided from the DISPLAY
/// CPU alone, because that is the CPU the bootloader lives on:
///
///  - Missing when the DISPLAY CPU is BLANK -- it is presenting a bootrom drive
///    rather than firmware, so there is nothing installed on it, bootloader
///    included. (A display deliberately put into BOOTSEL is indistinguishable
///    from an erased one, and reports Missing too. That is the right answer for
///    both: it is also exactly when you would want to install it.)
///  - Missing when the DISPLAY CPU is running a NON-OG app -- it answers on a
///    port, but its product string does not begin `FWOG `, so what is installed
///    is not wiliOGBsp firmware.
///  - Present when the product string does begin `FWOG `. Note this covers an
///    OG APP as well as the bootloader itself: an app can only have got there
///    through the bootloader, so its presence proves the bootloader's.
///  - Unknown when the DISPLAY CPU is neither -- not answering and not mounted.
///    Nothing is claimed, and the UI says nothing.
enum class OgBootloaderState { Present, Missing, Unknown };

OgBootloaderState ogBootloaderState(const CpuIdentity& identity);

} // namespace fwog
