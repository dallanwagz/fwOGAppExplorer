#include "platform/fwSerialTouch.h"

#if defined(_WIN32)
  #include <windows.h>
#elif !defined(__EMSCRIPTEN__)
  #include <fcntl.h>
  #include <termios.h>
  #include <unistd.h>
#endif

// VERIFIED ON LINUX, against the POSIX branch below exactly as it is written:
// it needed no change. Measured on an attached FreeWili 1-OG -- MainCPU v87
// (093c:2054, /dev/ttyACM0) and DisplayCPU v67 (093c:2055, /dev/ttyACM1) --
// which was rebooted into the RP2040 ROM bootloader and brought back again,
// MAIN once and DISPLAY twice. DISPLAY is the case that had to work: it has no
// BOOTSEL button, so FwOGapp Contract rule 3 makes this its only way back, and
// a Linux branch that only worked on MAIN would have been a latent trap.
//
// Every success was confirmed by OBSERVATION rather than inference: the 093c
// device disconnected and a Raspberry Pi 2e8a:0003 "RP2 Boot" device appeared
// on the same USB hub port ~0.3 s later, carrying an RPI-RP2 volume. (The
// same-hub-port half was confirmed from sysfs by a separate reviewer; the
// original instrument for this note only watched lsusb for VID:PID and could
// not have seen a port. Noted because a measurement is only as good as what
// was actually watching.)
//
// WHAT TRIGGERS THE RESET, from experiments built to tell the candidates apart
// rather than to confirm one:
//   * The tcsetattr() below is the trigger -- not the open(), and not the
//     close(). With the fd deliberately HELD OPEN after tcsetattr, the device
//     detached within one second, while TIOCMGET still read DTR HIGH and before
//     any close() had run. So neither closing the port nor the DTR drop that
//     closing it would cause is part of the mechanism.
//   * DTR is not involved at all. A deliberate DTR low (300 ms) -> high cycle
//     at B9600 followed by close() left both CPUs running, untouched. So did
//     every plain open/close at B9600 or B115200 -- and open() raises DTR by
//     itself here (the same thing C5 measured for readSerialLine), so those
//     were DTR transitions too. Baud is what matters; DTR is not.
// That is exactly what the header says the mechanism is -- the 1200-baud line
// coding alone, seen by pico_usb_reset's tud_cdc_line_coding_cb(). This branch
// is the minimum that delivers it, which is why it is this short.
//
// TWO THINGS IT DELIBERATELY DOES NOT DO, both queried during integration and
// both unnecessary by measurement rather than by argument:
//   * It does not assert DTR. The explicit TIOCMBIS in readSerialLine()
//     (fwSerialPorts.cpp) belongs to the READ path, where DTR must be up or
//     pico_stdio_usb never writes a byte. This function never reads one, and
//     the reset does not depend on DTR anyway.
//   * It does not call cfmakeraw() or force CLOCAL|CREAD. Those govern the line
//     discipline applied to bytes travelling through the port, and no byte ever
//     does: the port is opened, its line coding is set, and it is closed. The
//     open is O_NONBLOCK, so it cannot block waiting for carrier either.
//     (Measured in passing: on this kernel a freshly enumerated port already
//     had CLOCAL and CREAD set in its inherited termios.)
//
// Reading the Windows branch as "the DCB dance Linux is missing" is a trap: it
// does nothing extra for DTR either. A DCB is set as one whole struct, so
// the whole DCB is set as one struct, so restating ByteSize/Parity/StopBits
// alongside BaudRate is the conventional shape rather than a requirement
// (GetCommState pre-fills them), while
// cfsetospeed() sets only the speed. Both branches say the same thing -- "the
// line coding is now 1200" -- and differ only in how much of the frame format
// each API makes them repeat while saying it.
//
// NOT VERIFIED, and not claimed either way: whether a SECOND touch of a port
// already left at B1200 by a FAILED touch still reaches the device, or is
// swallowed because the requested line coding did not change. That state could
// not be produced here -- every touch succeeded and took the port node with it.
// It is not reachable after a SUCCESSFUL touch: a re-enumerated port comes back
// at B9600, the driver default, measured, and not at the 1200 it was left at.
// (docs/linux-port-progress.json tracks that work; this comment deliberately
// does not restate its status, only what this file's own branch has done.)
//
// This note is here rather than anywhere else because the sentence that used to
// carry it was in fwSerialPorts.cpp -- "same status as the other POSIX branches
// in this project (see fwSerialTouch.cpp, fwVolume.cpp)" -- and it went away
// when THAT file's Linux branch was measured and rewrote its own header. A
// status recorded only in a neighbour's file disappears the moment the
// neighbour's status changes, so each branch now states its own.

namespace fwog {

void touchPort1200(const std::string& port)
{
#if defined(_WIN32)
    // The "\\.\COMxx" form is required: a bare "COM10" does not open for port
    // numbers above 9.
    const std::string path = "\\\\.\\" + port;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;   // expected; see the header

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (GetCommState(h, &dcb)) {
        dcb.BaudRate = 1200;
        dcb.ByteSize = 8;
        dcb.Parity   = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        SetCommState(h, &dcb);   // this is what triggers the reset
    }
    CloseHandle(h);
#elif defined(__EMSCRIPTEN__)
    (void)port;
#else
    int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return;
    termios tio = {};
    if (tcgetattr(fd, &tio) == 0) {
        cfsetispeed(&tio, B1200);
        cfsetospeed(&tio, B1200);
        tcsetattr(fd, TCSANOW, &tio);
    }
    ::close(fd);
#endif
}

} // namespace fwog
