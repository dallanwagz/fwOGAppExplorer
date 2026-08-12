find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(FWOG_FIRMWARE_DIR "${CMAKE_SOURCE_DIR}/firmware"
    CACHE PATH "Directory holding the UF2 images and manifest.json to embed")

set(_gen_dir "${CMAKE_BINARY_DIR}/generated")
set(_gen_src "${_gen_dir}/fwEmbeddedFirmware.cpp")
set(_gen_hdr "${_gen_dir}/fwEmbeddedFirmware.h")

# ---------------------------------------------------------------------------
# WHY THIS IS AN ALWAYS-RUN TARGET AND NOT add_custom_command(OUTPUT ...)
#
# The obvious form -- OUTPUT ${_gen_src}, DEPENDS the images -- cannot express
# the dependency this step actually has, because DEPENDS is an mtime
# comparison. An image replaced in place (same filename) with an mtime OLDER
# than the already-generated blob leaves the whole build looking up to date:
# `cmake --build` prints "ninja: no work to do.", the executable keeps
# embedding bytes that are no longer on disk, and NOTHING catches it -- the
# test suite validates the generated blob against the hashes recorded beside
# it in that same generated file, and those two agree with each other no
# matter how stale they both are. That is not hypothetical; it is what happens
# whenever images are restored from a backup, unpacked from an archive that
# preserves timestamps, checked out by a tool that sets mtimes, or copied with
# `cp -p`. The symptom is the worst kind: green build, green tests, wrong
# firmware written to a real board.
#
# A configure-time hash of the images does NOT fix this, and it is worth
# spelling out why, because it looks like it should: CONFIGURE_DEPENDS
# re-globs only to decide whether the glob's RESULT -- the set of names --
# changed. Replacing a file in place does not change the name set, so CMake is
# never re-run, so a configure-time hash is never recomputed, so the stamp it
# writes never moves. It helps exactly one person: whoever happens to run
# `cmake --preset` by hand before building.
#
# So the check has to happen at BUILD time, on every build. This target has no
# real OUTPUT for ninja to stat, so its command runs unconditionally, and
# tools/embed_firmware.py decides for itself whether there is work:
#
#   * it hashes its inputs (this is the content dependency mtimes cannot be)
#     and returns immediately if they match the stamp it wrote last time.
#     SHA-256 over all ~21 MB costs ~0.04 s; the deflate it skips costs ~9 s;
#   * it writes fwEmbeddedFirmware.{h,cpp} only when their content changes,
#     so the 32 MB translation unit downstream is NOT recompiled and nothing
#     is relinked on an ordinary build. BYPRODUCTS makes CMake emit
#     `restat = 1` on this edge, which is what lets ninja notice the outputs
#     did not move and leave everything downstream alone.
#
# The honest cost, measured: one command that always executes, ~0.05 s of
# wall time, and one line of build output that a genuinely idle build would
# not otherwise print. `ninja: no work to do.` is therefore no longer the
# no-op message for this project -- that is the price of the guarantee, and
# it is deliberate.
#
# Inputs are enumerated by the script, not here: it hashes itself,
# manifest.json, and every image manifest.json names. That set is exactly what
# it reads, so it covers ../probe/probe.uf2 -- which lives outside
# FWOG_FIRMWARE_DIR, is committed to git, and which no glob of this directory
# could ever see -- on identical terms to the rest.
# ---------------------------------------------------------------------------
add_custom_target(fwog_generate_firmware
    BYPRODUCTS ${_gen_src} ${_gen_hdr}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/embed_firmware.py
            ${FWOG_FIRMWARE_DIR} ${_gen_dir}
    COMMENT "Checking embedded firmware inputs"
    VERBATIM)

# ${_gen_src} is listed in fwog_core's sources. BYPRODUCTS already marks both
# files GENERATED, which is what keeps configure from failing on a clean tree
# where neither exists yet; saying it again here is not redundancy for its own
# sake, it is so that moving or rewriting the target above cannot silently take
# the property with it. Build ORDER comes from a different place again --
# `add_dependencies(fwog_core fwog_generate_firmware)` in CMakeLists.txt -- and
# both are load-bearing on a clean build.
set_source_files_properties(${_gen_src} ${_gen_hdr} PROPERTIES GENERATED TRUE)
