#include "platform/fwPaths.h"

#include <cstdlib>
#include <iterator>

#if defined(_WIN32)
  #include <windows.h>
#elif !defined(__EMSCRIPTEN__)
  #include <unistd.h>
  #include <limits.h>
#endif

namespace fwog {
namespace {
constexpr const char* kAppName = "fwOGAppExplorer";
} // namespace

std::filesystem::path exeDir()
{
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::filesystem::path(buf).parent_path();
#elif defined(__EMSCRIPTEN__)
    return std::filesystem::path("/");
#else
    char buf[PATH_MAX] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::filesystem::current_path();
    return std::filesystem::path(std::string(buf, size_t(n))).parent_path();
#endif
}

std::filesystem::path userDataDir()
{
    std::filesystem::path base;
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable: 4996)   // getenv: the returned pointer is copied
#endif                             // into a path immediately; no ownership kept
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA")) base = local;
    else base = exeDir();
#elif defined(__EMSCRIPTEN__)
    base = "/data";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) base = xdg;
    else if (const char* home = std::getenv("HOME"))
        base = std::filesystem::path(home) / ".local" / "share";
    else base = exeDir();
#endif
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
    auto dir = base / kAppName;
    // create_directories is best-effort: a failure here (e.g. a read-only
    // parent) is not reported, because the first real write into this
    // directory will fail loudly on its own.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path tempDir()
{
    std::error_code ec;
    auto p = std::filesystem::temp_directory_path(ec);
    return ec ? userDataDir() : p;
}

std::filesystem::path catalogDir()
{
    return exeDir() / "catalog";
}

} // namespace fwog
