#include "platform/fwSerialPorts.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
  #include <windows.h>
  #include <devguid.h>
  #include <setupapi.h>
#elif !defined(__EMSCRIPTEN__)
  #include <fcntl.h>
  #include <termios.h>
  #include <unistd.h>
  #include <chrono>
  #include <filesystem>
  #include <thread>
#endif

namespace fwog {
namespace {

/// Strip the line terminator a caller must never see. Only CR and LF are
/// removed, and only from the END: leading or interior junk is left alone so
/// that parseProbeLine() can reject it, rather than being tidied into
/// something that parses.
std::string stripEol(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

} // namespace

std::vector<std::string> dedupePortNames(std::vector<std::string> ports)
{
    // First occurrence wins and the surviving order is preserved, rather than
    // sort-and-unique: callers document their order as "whatever the OS said",
    // and quietly re-sorting it would make that comment false.
    std::unordered_set<std::string> seen;
    std::vector<std::string> out;
    out.reserve(ports.size());
    for (auto& p : ports)
        if (seen.insert(p).second) out.push_back(std::move(p));
    return out;
}

std::vector<std::string> listSerialPorts()
{
    std::vector<std::string> names;
    for (auto& info : listSerialPortInfo()) names.push_back(std::move(info.port));
    return dedupePortNames(std::move(names));
}

#if defined(_WIN32)

std::vector<SerialPortInfo> listSerialPortInfo()
{
    // GUID_DEVCLASS_PORTS is Device Manager's "Ports (COM & LPT)" category, and
    // DIGCF_PRESENT restricts it to devnodes that are ATTACHED RIGHT NOW. That
    // is the whole difference from the SERIALCOMM key this used to read -- see
    // the header for the captured evidence of what that key actually contains.
    std::vector<SerialPortInfo> ports;
    const HDEVINFO set =
        SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return ports;   // could not look: fail closed

    SP_DEVINFO_DATA dev = {};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        // The COM name lives in the devnode's own "Device Parameters" subkey,
        // under PortName. This is the string Device Manager shows in brackets,
        // and it is written by the port's driver when the name is assigned.
        const HKEY key =
            SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;

        char  nameBuf[64] = {};
        DWORD nameLen = sizeof(nameBuf);
        DWORD type = 0;
        const LSTATUS rc = RegQueryValueExA(key, "PortName", nullptr, &type,
                                            reinterpret_cast<LPBYTE>(nameBuf), &nameLen);
        RegCloseKey(key);
        if (rc != ERROR_SUCCESS || type != REG_SZ) continue;
        // nameLen counts the NUL for values the drivers write, but the API does
        // not promise the data is NUL-terminated at all, so build from an
        // explicit length and trim.
        std::string name(nameBuf, nameLen ? nameLen - 1 : 0);
        while (!name.empty() && name.back() == '\0') name.pop_back();
        // This class also holds LPT ports, which are not serial and cannot be
        // read; nothing here has any use for them.
        if (name.rfind("COM", 0) != 0) continue;

        std::string id;
        char  idBuf[512] = {};
        DWORD idLen = 0;
        if (SetupDiGetDeviceInstanceIdA(set, &dev, idBuf, DWORD(sizeof(idBuf)), &idLen)) {
            id.assign(idBuf);
            // Upper-cased once, here, so that every comparison downstream can
            // be a plain one. Windows is inconsistent about the case of these
            // strings and a case-sensitive match against them is a latent bug.
            for (char& c : id) c = char(std::toupper(static_cast<unsigned char>(c)));
        }
        // An id we could not read is left EMPTY rather than guessed at: the
        // callers treat empty as "unknown", which is the truth.

        ports.push_back(SerialPortInfo{ std::move(name), std::move(id) });
    }
    SetupDiDestroyDeviceInfoList(set);
    return ports;
}

std::optional<std::string> readSerialLine(const std::string& port, int timeoutMs)
{
    // The "\\.\COMxx" form is required for port numbers above 9 -- same reason
    // touchPort1200() uses it.
    const std::string path = "\\\\.\\" + port;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        return std::nullopt;
    }
    // 115200, NOT 1200. See readSerialLine()'s header comment: 1200 would
    // reboot the CPU we are trying to read from.
    dcb.BaudRate    = CBR_115200;
    dcb.ByteSize    = 8;
    dcb.Parity      = NOPARITY;
    dcb.StopBits    = ONESTOPBIT;
    // DTR asserted, or the prober never writes anything -- see the header.
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX  = FALSE;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        return std::nullopt;
    }
    EscapeCommFunction(h, SETDTR);

    // Short per-read timeouts, with the OVERALL budget enforced by the loop
    // below against a monotonic clock. A single long ReadFile timeout would be
    // simpler and wrong: it could not distinguish "nothing yet" from "a
    // partial line", and it would overshoot the caller's budget by up to one
    // whole read.
    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout        = 50;
    timeouts.ReadTotalTimeoutConstant   = 100;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    SetCommTimeouts(h, &timeouts);

    const ULONGLONG start = GetTickCount64();
    std::string line;
    for (;;) {
        char  buf[128];
        DWORD read = 0;
        if (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) {
            for (DWORD i = 0; i < read; ++i) {
                if (buf[i] == '\n') {
                    CloseHandle(h);
                    return stripEol(line);
                }
                // Bounded: a device that never sends a newline must not be
                // able to grow this without limit. 4 KB is far more than any
                // legitimate line and far less than a memory problem.
                if (line.size() < 4096) line.push_back(buf[i]);
            }
        }
        if (GetTickCount64() - start >= static_cast<ULONGLONG>(timeoutMs > 0 ? timeoutMs : 0))
            break;
    }
    CloseHandle(h);
    return std::nullopt;   // no COMPLETE line arrived: a partial one is not an answer
}

#elif defined(__EMSCRIPTEN__)

// The web build cannot reach serial ports at all (kDeviceSupportAvailable is
// false there and every device affordance is already disabled), so these are
// the honest answers rather than stubs that pretend.
std::vector<SerialPortInfo> listSerialPortInfo() { return {}; }

std::optional<std::string> readSerialLine(const std::string&, int) { return std::nullopt; }

#else

// UNVERIFIED: no Linux toolchain exists on the machine this was written on, so
// nothing below has been compiled or run. Same status as the other POSIX
// branches in this project (see fwSerialTouch.cpp, fwVolume.cpp) and flagged
// for the same reason.
//
// usbId is left EMPTY here, deliberately and not as an oversight. Deriving it
// would mean walking /sys/class/tty/<name>/device/.. for idVendor/idProduct,
// and this branch has never been compiled, let alone run against a board --
// writing unverifiable code whose whole job is to identify which CPU may be
// written to would be the wrong trade. The consequence is bounded and
// documented at the caller: with no USB identity, identifyCpus() falls back to
// ordering candidates by "appeared since we wrote", and still refuses unless
// one of them speaks the prober's protocol. /dev names are also not reused the
// way COM numbers are, so the failure this fix exists to close does not arise
// here in the same form.
std::vector<SerialPortInfo> listSerialPortInfo()
{
    std::vector<SerialPortInfo> ports;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/dev", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("ttyACM", 0) == 0 || name.rfind("ttyUSB", 0) == 0)
            ports.push_back(SerialPortInfo{ entry.path().string(), std::string{} });
    }
    return ports;
}

std::optional<std::string> readSerialLine(const std::string& port, int timeoutMs)
{
    int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return std::nullopt;

    termios tio = {};
    if (tcgetattr(fd, &tio) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    cfmakeraw(&tio);
    // 115200, NOT 1200 -- see the header comment.
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        ::close(fd);
        return std::nullopt;
    }

    const auto start = std::chrono::steady_clock::now();
    std::string line;
    for (;;) {
        char buf[128];
        const auto n = ::read(fd, buf, sizeof(buf));
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                ::close(fd);
                return stripEol(line);
            }
            if (line.size() < 4096) line.push_back(buf[i]);
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeoutMs) break;
        if (n <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::close(fd);
    return std::nullopt;
}

#endif

} // namespace fwog
