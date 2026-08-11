#include "platform/fwVolume.h"

#include "core/fwTypes.h"   // platformLimitationNotice (the Emscripten branch below)

#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
  #include <windows.h>
#endif

// UNVERIFIED ON LINUX, in the part that talks to the machine. detail::
// unescapeMount() below is pure and is covered by tests/test_fwVolume.cpp on
// every platform; findRpiRp2Volumes()'s /proc/mounts branch and copyToVolume()'s
// std::filesystem path compile and link warning-clean on the linux-gcc-release
// preset and have not been run. Nothing in this tree records an RPI-RP2 volume
// having been discovered or written on Linux. (docs/linux-port-progress.json
// tracks that work; this comment deliberately does not restate its status,
// only what this file's own branches have done.)
//
// Recorded here for the same reason as the equivalent note in fwSerialTouch.cpp:
// the one sentence that used to flag both of these files lived in
// fwSerialPorts.cpp and went away when that file's own Linux branch was
// measured. Each branch states its own status now.

namespace fwog {
namespace detail {

std::string unescapeMount(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            s[i+1] >= '0' && s[i+1] <= '7' &&
            s[i+2] >= '0' && s[i+2] <= '7' &&
            s[i+3] >= '0' && s[i+3] <= '7') {
            out.push_back(char((s[i+1] - '0') * 64 + (s[i+2] - '0') * 8 + (s[i+3] - '0')));
            i += 3;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

} // namespace detail

namespace {
#if !defined(__EMSCRIPTEN__)
// Unreferenced on Emscripten, where there is no mass storage to enumerate;
// guarded so that branch does not warn about an unused constant the first
// time it actually compiles.
constexpr const char* kLabel = "RPI-RP2";
#endif

#if defined(_WIN32)
// SetErrorMode is a process-global setting, and findRpiRp2Volumes runs in a
// ~250ms poll loop while waiting for a board to appear. A scope guard, not a
// manual save/restore pair, ensures a future early `return` added inside the
// loop can never silently leave the suppressed mode in effect.
struct ErrorModeGuard {
    UINT prev;
    explicit ErrorModeGuard(UINT mode) : prev(SetErrorMode(mode)) {}
    ~ErrorModeGuard() { SetErrorMode(prev); }
};
#endif
} // namespace

std::vector<std::string> findRpiRp2Volumes()
{
    std::vector<std::string> out;
#if defined(_WIN32)
    // An empty card reader or CD drive must never be allowed to raise the
    // Windows "There is no disk in the drive" modal -- that would block the
    // worker thread indefinitely behind a dialog the user may not even
    // connect to this app. Suppress it for the duration of the enumeration.
    ErrorModeGuard errorModeGuard(SEM_FAILCRITICALERRORS);

    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        char root[] = { char('A' + i), ':', '\\', '\0' };

        // Network and CD drives can make GetVolumeInformationA block for
        // seconds (e.g. a disconnected network share), which would stall the
        // flash worker inside this poll loop. The RP2040 bootrom volume
        // always enumerates as removable, so skip anything else without
        // touching it.
        if (GetDriveTypeA(root) != DRIVE_REMOVABLE) continue;

        char label[MAX_PATH + 1] = {};
        if (!GetVolumeInformationA(root, label, MAX_PATH, nullptr, nullptr,
                                   nullptr, nullptr, 0))
            continue;
        if (std::string(label) == kLabel) out.emplace_back(root);
    }
#elif defined(__EMSCRIPTEN__)
    // No mass storage in a browser.
#else
    std::ifstream mounts("/proc/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream ls(line);
        std::string dev, rawMountPoint;
        if (!(ls >> dev >> rawMountPoint)) continue;
        // The kernel escapes space, tab, newline and backslash in this field
        // as octal; decode before using it as a filesystem path, or a mount
        // point containing a space (e.g. a display name) yields a path that
        // does not exist on disk.
        const std::string mountPoint = detail::unescapeMount(rawMountPoint);
        // udisks mounts the bootrom volume at .../RPI-RP2, by label.
        const auto pos = mountPoint.rfind('/');
        if (pos != std::string::npos && mountPoint.substr(pos + 1) == kLabel)
            out.push_back(mountPoint);
    }
#endif
    return out;
}

std::expected<void, std::string> copyToVolume(const std::filesystem::path& src,
                                              const std::string& volume)
{
#if defined(__EMSCRIPTEN__)
    // Unreachable in practice -- findRpiRp2Volumes() returns nothing here, so
    // no plan can ever reach a copy, and every Flash button is disabled with
    // platformLimitationNotice() before that. It is still a hard refusal
    // rather than a fall-through to the std::filesystem path below: MEMFS
    // would happily "succeed" at copying a UF2 into a directory inside the
    // browser's own sandbox, and this function reporting success is exactly
    // the "told the user a board was written when it was not" failure the
    // comment further down calls the worst thing it can do.
    (void)src;
    (void)volume;
    return std::unexpected(std::string(platformLimitationNotice()));
#else
    std::error_code ec;
    const auto srcSize = std::filesystem::file_size(src, ec);
    if (ec) return std::unexpected("cannot read " + src.string() + ": " + ec.message());

    const auto dst = std::filesystem::path(volume) / src.filename();
    std::filesystem::copy_file(src, dst,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return std::unexpected("copy to " + volume + " failed: " + ec.message());

    // The bootrom volume disappears the instant it accepts the image, so a
    // destination that is now missing means success. Any OTHER error from the
    // size check is a genuine failure and must not be reported as a completed
    // flash -- telling the user a board was written when it was not is the
    // worst thing this function can do.
    const auto dstSize = std::filesystem::file_size(dst, ec);
    if (!ec) {
        if (dstSize != srcSize)
            return std::unexpected("the copied image is the wrong size; the write did not complete");
    } else if (ec != std::errc::no_such_file_or_directory) {
        return std::unexpected("could not verify the written image: " + ec.message());
    }

    return {};
#endif
}

} // namespace fwog
