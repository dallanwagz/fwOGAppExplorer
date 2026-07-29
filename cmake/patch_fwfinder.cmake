# Strip the C++23 `#include <print>` from fwfinder_windows.cpp so the project
# builds on MSVC < 14.37. The include is unused in the .cpp (only referenced
# in doxygen comments). FetchContent runs this script via PATCH_COMMAND in the
# fetched source tree, so the working directory is fwfinder's source root.
#
# Idempotent: a no-op if the line is already commented out.

# In `cmake -P` script mode, CMAKE_CURRENT_BINARY_DIR resolves to the working
# directory at invocation time. FetchContent runs PATCH_COMMAND from the
# populated source root, so this lands on src/fwfinder_windows.cpp inside it.
set(_target "${CMAKE_CURRENT_BINARY_DIR}/src/fwfinder_windows.cpp")

if(NOT EXISTS "${_target}")
    message(WARNING "patch_fwfinder.cmake: ${_target} not found; skipping")
    return()
endif()

file(READ "${_target}" _contents)

# Already patched? Bail. The marker string keeps re-runs no-op even if the file
# is read back through the same regex.
if(_contents MATCHES "patched out: requires MSVC")
    return()
endif()

string(REGEX REPLACE
    "([ \t]*)#include[ \t]+<print>"
    "\\1// #include <print>  // patched out: requires MSVC >= 14.37"
    _patched
    "${_contents}")

if(NOT "${_patched}" STREQUAL "${_contents}")
    file(WRITE "${_target}" "${_patched}")
    message(STATUS "patch_fwfinder.cmake: stripped <print> include from ${_target}")
endif()
