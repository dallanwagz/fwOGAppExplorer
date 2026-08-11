# Repairs applied to the fetched freewili-finder source before it is compiled.
# Both are upstream defects rather than anything this project can express on its
# own side of the dependency, and both are one-line edits -- kept here as a
# PATCH_COMMAND rather than as a fork of freewili-finder so that following
# upstream stays a one-line GIT_TAG bump instead of a merge.
#
# FetchContent runs this script via PATCH_COMMAND in the fetched source tree, so
# the working directory is fwfinder's source root.
#
# In `cmake -P` script mode, CMAKE_CURRENT_BINARY_DIR resolves to the working
# directory at invocation time. FetchContent runs PATCH_COMMAND from the
# populated source root, so the targets below land inside it.
#
# Both patches are idempotent: re-running is a no-op. That matters more than it
# looks. FetchContent does NOT re-run PATCH_COMMAND against an already-populated
# tree, so anyone adding a patch here has to apply it by hand to their existing
# build tree as well -- and will then quite reasonably run this script by hand
# to check that it does the same thing. Doubling the edit at that moment is the
# failure mode being designed out.
#
# Each patch lives in its own function so that "the file is not there" and
# "already patched" can stay early `return()`s, which is what they read as. At
# file scope a `return()` ends the whole SCRIPT, and would therefore silently
# skip every patch declared after it. Since both fwfinder_windows.cpp and
# fwfinder_linux.cpp exist in every checkout but only one of them is ever
# compiled, the patch that got skipped would be the one that mattered about
# half the time -- and it would present as a compile error in a dependency,
# which is the last place anyone looks.

# Strip the C++23 `#include <print>` from fwfinder_windows.cpp so the project
# builds on MSVC < 14.37. The include is unused in the .cpp (only referenced
# in doxygen comments).
function(_fwfinder_patch_windows_print)
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
endfunction()

# Add the missing `#include <functional>` to fwfinder_linux.cpp. That file
# declares parameters of type `std::optional<std::reference_wrapper<const
# DiskInfo>>` and `std::reference_wrapper<const SerialInfo>` but includes no
# header that defines std::reference_wrapper, so under libstdc++ the template is
# an incomplete type and the translation unit does not compile at all. Nothing
# upstream is wrong with the CODE -- it is a missing include and only a missing
# include -- which is exactly why patching it here is honest and forking would
# not be. MSVC never surfaced it because the Windows build compiles a different
# file entirely and its own headers happen to drag <functional> in.
#
# The insertion is anchored on <expected> rather than appended after the last
# include for two reasons. It puts <functional> among the C++ standard headers
# instead of down among the platform ones (libudev.h, mntent.h), which is where
# a reader would go looking for it. And <expected> cannot quietly stop being
# there: the file's own functions return std::expected, so an upstream that
# removed that include would already be failing to compile for its own reasons
# and the missing anchor would not be the thing anyone had to diagnose.
function(_fwfinder_patch_linux_functional)
    set(_target "${CMAKE_CURRENT_BINARY_DIR}/src/fwfinder_linux.cpp")

    if(NOT EXISTS "${_target}")
        message(WARNING "patch_fwfinder.cmake: ${_target} not found; skipping")
        return()
    endif()

    file(READ "${_target}" _contents)

    # Already patched? Bail, on the same marker-string principle as the Windows
    # patch above.
    if(_contents MATCHES "patched in: std::reference_wrapper")
        return()
    endif()

    # And bail if upstream has since fixed this themselves. A second
    # `#include <functional>` would be harmless -- the header is idempotent --
    # but it would sit there implying a defect that no longer exists, and the
    # next person to bump GIT_TAG deserves to find this patch already inert
    # rather than to find two includes and have to work out which one is ours.
    if(_contents MATCHES "#include[ \t]*<functional>")
        return()
    endif()

    string(REGEX REPLACE
        "([ \t]*)#include[ \t]+<expected>"
        "\\1#include <functional>  // patched in: std::reference_wrapper, used below\n\\1#include <expected>"
        _patched
        "${_contents}")

    if(NOT "${_patched}" STREQUAL "${_contents}")
        file(WRITE "${_target}" "${_patched}")
        message(STATUS "patch_fwfinder.cmake: added <functional> include to ${_target}")
    else()
        # The anchor was not found, so nothing was inserted and the file will
        # fail to compile in a way that points at std::reference_wrapper rather
        # than at this script. Say so here, while there is still context.
        message(WARNING
            "patch_fwfinder.cmake: no <expected> include found in ${_target}; "
            "the <functional> include was NOT added and the Linux build will fail")
    endif()
endfunction()

_fwfinder_patch_windows_print()
_fwfinder_patch_linux_functional()
