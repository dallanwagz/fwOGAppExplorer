#include "platform/fwSerialTouch.h"

#if defined(_WIN32)
  #include <windows.h>
#elif !defined(__EMSCRIPTEN__)
  #include <fcntl.h>
  #include <termios.h>
  #include <unistd.h>
#endif

// UNVERIFIED ON LINUX. The POSIX branch of touchPort1200() below compiles and
// links -- the linux-gcc-release preset builds it warning-clean -- and it has
// not been run. Nothing in this tree records a 1200-baud touch having been
// performed on Linux. It is not a branch anyone can exercise casually:
// succeeding means rebooting the CPU on the other end into its bootloader.
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
