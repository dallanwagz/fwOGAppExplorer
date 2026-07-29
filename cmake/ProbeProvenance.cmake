# ---------------------------------------------------------------------------
# Drift gate for the committed CPU-prober image.
#
# probe/probe.uf2 is the one .uf2 in this repository that is checked in (see
# .gitignore for why: single self-contained executable, and no ARM
# cross-toolchain required to build the DESKTOP app). The risk that buys is a
# binary quietly drifting away from the source it claims to be built from --
# and this is the image the app writes to a CPU it has NOT identified, on a
# board whose DISPLAY CPU has no BOOTSEL button. So the drift is made loud here.
#
# WHERE THE SOURCE LIVES NOW. It is no longer in this repository. The prober is
# `apps/cpuprobe` in the wiliOGBsp BSP, and it is built there so that it
# inherits the board header every FreeWili OG binary needs: 16 MB QSPI flash,
# PICO_FLASH_SPI_CLKDIV 4, clk_peri following clk_sys, and the 200 MHz
# operating point. The previous incarnation lived here, was built for
# PICO_BOARD=pico, and did not boot on this silicon. Keeping a second copy of
# the source here purely so this file could hash it would recreate exactly the
# drift this gate exists to catch, one level up.
#
# So the gate is now in two halves, with different strengths, deliberately:
#
#   ALWAYS ENFORCED -- probe/probe.uf2 against kProbeImageSha256, byte for
#   byte. This is the load-bearing one: it is the artefact that ships, the
#   artefact the app writes to a board, and the artefact the GPIO-29
#   disassembly evidence is about. identifyCpus() re-checks the same constant
#   at run time before a single byte reaches a CPU, so the two cannot disagree.
#
#   ENFORCED WHEN THE BSP IS PRESENT -- apps/cpuprobe/main.c and
#   apps/cpuprobe/CMakeLists.txt against kProbeSourceSha256 and
#   kProbeBuildScriptSha256. FWOG_BSP_DIR defaults to a sibling checkout of
#   wiliOGBsp. When it is absent the desktop build must still succeed, because
#   "building this app needs no ARM toolchain and no firmware checkout" is a
#   requirement -- so that case reports STATUS and moves on rather than
#   failing. When it IS present, an edit to the prober's source or its build
#   settings that has not been rebuilt and re-recorded fails the configure.
#
# The build script is hashed alongside the source because several of the safety
# properties are decided there and nowhere else: the 1200-baud reset being
# compiled in (the only way back for a DISPLAY CPU), UART stdio being off, the
# ABSENCE of PICO_STDIO_USB_RESET_BOOTSEL_ACTIVITY_LED (which would make the
# SDK drive a GPIO on the way into BOOTSEL), and -- the reason the app links
# fwog_common and NOT fwog_main_bsp/fwog_display_bsp in the first place.
#
# All three expectations are recorded in src/flash/fwCpuProbe.h so they live in
# reviewed C++ source next to the code that uses them rather than in a side
# file nobody reads. The two text files are normalised CRLF -> LF because a
# line-ending conversion on checkout must not be able to fail this build; the
# .uf2 is binary and is hashed exactly.
#
# On a mismatch this fails the configure with the ACTUAL hash, so the fix is a
# copy-and-paste once the image has genuinely been rebuilt. Rebuilding is the
# first half of that fix and is not optional -- see probe/README.md.
# ---------------------------------------------------------------------------

set(_probe_dir    "${CMAKE_SOURCE_DIR}/probe")
set(_probe_header "${CMAKE_SOURCE_DIR}/src/flash/fwCpuProbe.h")

# A sibling checkout by default -- the layout both repositories actually sit in.
set(FWOG_BSP_DIR "${CMAKE_SOURCE_DIR}/../wiliOGBsp" CACHE PATH
    "wiliOGBsp checkout holding apps/cpuprobe, the prober's source. \
Optional: when absent the source half of the drift gate is skipped.")
set(_probe_src_dir "${FWOG_BSP_DIR}/apps/cpuprobe")

if(NOT EXISTS "${_probe_header}")
    message(FATAL_ERROR "src/flash/fwCpuProbe.h is missing; the probe drift gate cannot run.")
endif()
file(READ "${_probe_header}" _probe_header_text)

# Pull one `inline constexpr const char* <name> = "<hex>";` out of the header.
function(_fwog_expected_hash name out)
    string(REGEX MATCH "${name}[ \t\r\n]*=[ \t\r\n]*\"([0-9a-fA-F]+)\""
           _matched "${_probe_header_text}")
    if(NOT _matched)
        message(FATAL_ERROR
            "Could not find ${name} in src/flash/fwCpuProbe.h. The probe drift gate reads "
            "its expectations from that header; do not remove or reformat those constants.")
    endif()
    string(TOLOWER "${CMAKE_MATCH_1}" _lower)
    set(${out} "${_lower}" PARENT_SCOPE)
endfunction()

# The hash of a TEXT file with CRLF normalised to LF.
function(_fwog_text_hash path out)
    file(READ "${path}" _text)
    string(REPLACE "\r\n" "\n" _text "${_text}")
    string(SHA256 _hash "${_text}")
    string(TOLOWER "${_hash}" _hash)
    set(${out} "${_hash}" PARENT_SCOPE)
endfunction()

function(_fwog_check_probe_hash label path expected actual what_to_do)
    if(NOT "${expected}" STREQUAL "${actual}")
        message(FATAL_ERROR
            "\n"
            "The committed CPU-prober image and its source have drifted apart.\n"
            "\n"
            "  ${label}\n"
            "    file:     ${path}\n"
            "    expected: ${expected}\n"
            "    actual:   ${actual}\n"
            "\n"
            "${what_to_do}\n"
            "\n"
            "This image is the ONE thing this app writes to a CPU it has not identified, "
            "and it is only safe because probe/README.md's disassembly evidence says THAT "
            "EXACT BINARY never touches GPIO 29. A hash that no longer matches means that "
            "evidence is no longer about the file on disk, so the build stops here rather "
            "than shipping an unverified image to a board with no recovery path.\n")
    endif()
endfunction()

# --- half 1: the image itself. Always enforced. ----------------------------

if(NOT EXISTS "${_probe_dir}/probe.uf2")
    message(FATAL_ERROR
        "probe/probe.uf2 is missing. It is committed to this repository on purpose; "
        "see probe/README.md for how to rebuild it from wiliOGBsp's apps/cpuprobe.")
endif()

_fwog_expected_hash("kProbeImageSha256" _expect_uf2)
file(SHA256 "${_probe_dir}/probe.uf2" _actual_uf2)
string(TOLOWER "${_actual_uf2}" _actual_uf2)

_fwog_check_probe_hash("probe.uf2 (the committed image)" "${_probe_dir}/probe.uf2"
    "${_expect_uf2}" "${_actual_uf2}"
    "probe.uf2 is not the image this build expects. If you rebuilt it deliberately, update \
kProbeImageSha256 in src/flash/fwCpuProbe.h to the actual value above; the app checks the \
same hash again at run time before writing the image to a board, so both must agree.")

# Re-run whenever the image or the expectations change, not only when something
# else happens to trigger a reconfigure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_probe_header}"
    "${_probe_dir}/probe.uf2")

# --- half 2: the source it was built from. Enforced when reachable. --------

if(EXISTS "${_probe_src_dir}/main.c" AND EXISTS "${_probe_src_dir}/CMakeLists.txt")
    _fwog_expected_hash("kProbeSourceSha256"      _expect_src)
    _fwog_expected_hash("kProbeBuildScriptSha256" _expect_bld)
    _fwog_text_hash("${_probe_src_dir}/main.c"         _actual_src)
    _fwog_text_hash("${_probe_src_dir}/CMakeLists.txt" _actual_bld)

    _fwog_check_probe_hash("apps/cpuprobe/main.c (the prober's source)"
        "${_probe_src_dir}/main.c" "${_expect_src}" "${_actual_src}"
        "The prober's source in wiliOGBsp has changed. Rebuild it there (fw build cpuprobe), \
copy the new cpuprobe.uf2 over probe/probe.uf2, and update kProbeSourceSha256 in \
src/flash/fwCpuProbe.h to the actual value above.")

    _fwog_check_probe_hash("apps/cpuprobe/CMakeLists.txt (the prober's build settings)"
        "${_probe_src_dir}/CMakeLists.txt" "${_expect_bld}" "${_actual_bld}"
        "The prober's build settings in wiliOGBsp have changed -- which can change its safety \
properties on its own (the 1200-baud way back, UART stdio, the activity-LED define, and which \
BSP libraries it links). Rebuild it there (fw build cpuprobe), copy the new cpuprobe.uf2 over \
probe/probe.uf2, and update kProbeBuildScriptSha256 in src/flash/fwCpuProbe.h to the actual \
value above.")

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_probe_src_dir}/main.c"
        "${_probe_src_dir}/CMakeLists.txt")

    message(STATUS
        "CPU prober: probe.uf2 verified against ${_probe_src_dir} (${_expect_uf2})")
else()
    # Not an error. Building this app must not require a firmware checkout.
    message(STATUS
        "CPU prober: probe.uf2 verified by hash (${_expect_uf2}). Its source was NOT "
        "checked: no wiliOGBsp checkout at ${FWOG_BSP_DIR}. Set -DFWOG_BSP_DIR=<path> "
        "to enable the source half of the drift gate.")
endif()
