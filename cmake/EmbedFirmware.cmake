find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(FWOG_FIRMWARE_DIR "${CMAKE_SOURCE_DIR}/firmware"
    CACHE PATH "Directory holding the UF2 images and manifest.json to embed")

set(_gen_dir "${CMAKE_BINARY_DIR}/generated")
set(_gen_src "${_gen_dir}/fwEmbeddedFirmware.cpp")
set(_gen_hdr "${_gen_dir}/fwEmbeddedFirmware.h")

# Re-run when the script, the manifest, OR any image changes. Without the
# images in DEPENDS, replacing a .uf2 in place (same filename) leaves the
# previously generated blob in the build tree and the app silently embeds
# stale firmware -- undetectable, because the round-trip test only proves
# the generated file is internally consistent with itself, not that it
# matches what is currently on disk. CONFIGURE_DEPENDS re-globs on every
# configure, so a newly *added* image (a new filename referenced by an
# edited manifest.json) is picked up too; a manifest-only edit already
# retriggers configure via the DEPENDS entry below.
file(GLOB _fwog_firmware_images CONFIGURE_DEPENDS "${FWOG_FIRMWARE_DIR}/*.uf2")

# The CPU prober is the one embedded image that does NOT live in the firmware
# directory -- it is committed to git and sits beside the probe/probe.c it was
# built from (see .gitignore and firmware/manifest.json's "../probe/probe.uf2").
# The glob above therefore cannot see it, and without naming it explicitly here
# a rebuilt probe.uf2 would leave the previously generated blob in the build
# tree and the app would silently embed the stale one -- exactly the failure
# the DEPENDS list above exists to prevent for the other images.
set(_fwog_probe_image "${CMAKE_SOURCE_DIR}/probe/probe.uf2")

add_custom_command(
    OUTPUT  ${_gen_src} ${_gen_hdr}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/embed_firmware.py
            ${FWOG_FIRMWARE_DIR} ${_gen_dir}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/embed_firmware.py
            ${FWOG_FIRMWARE_DIR}/manifest.json
            ${_fwog_firmware_images}
            ${_fwog_probe_image}
    COMMENT "Embedding firmware images"
    VERBATIM)

add_custom_target(fwog_generate_firmware DEPENDS ${_gen_src} ${_gen_hdr})
