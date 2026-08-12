# Binary resources compiled into the executable, alongside the firmware images
# EmbedFirmware.cmake handles. Same tool shape, same reason: this app ships as a
# single .exe, and the Recovery tab's board diagram is the one asset that must
# be present exactly when somebody's board will not enumerate.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_res_gen_dir "${CMAKE_BINARY_DIR}/generated")
set(_res_board_src "${_res_gen_dir}/fwEmbeddedResource_boardRecovery.cpp")
set(_res_board_bmp "${CMAKE_SOURCE_DIR}/resources/board-recovery.bmp")

# The window icon, embedded ONLY where something has to decode it at runtime.
#
# Windows does not: linking resources/app.rc is the whole of the icon wiring
# there (see that file, and src/ui/fwWindowIcon.h). Embedding these bytes in
# the Windows executable as well would put ~110 KB of a second copy of the same
# drawing in it for nothing. It would not confuse SDL_RegisterApp's "first
# RT_GROUP_ICON wins" rule that app.rc depends on -- a deflated byte array in
# .rodata is not an icon resource -- but "harmless" is not a reason to ship it.
#
# Emscripten is excluded for a different reason: SDL's Emscripten backend
# implements SDL_SetWindowIcon as a favicon, which would be a nice thing to
# have, but the wasm preset has never been compiled (see CMakePresets.json) and
# an untested code path plus its payload is not what that build needs first.
set(_res_icon_src "")
if(NOT WIN32 AND NOT EMSCRIPTEN)
    set(_res_icon_src "${_res_gen_dir}/fwEmbeddedResource_appIcon.cpp")
endif()
set(_res_icon_ico "${CMAKE_SOURCE_DIR}/resources/product.ico")

# BMP, not PNG: SDL3's core ships SDL_LoadBMP and no PNG decoder, so this is the
# one raster format the app can read without adding a dependency. It is bulky on
# disk and that is fine -- embed_resource.py deflates it, and only the deflated
# bytes reach the binary.
#
# ---------------------------------------------------------------------------
# ALWAYS-RUN, AND WHY THIS FILE NOW MATCHES EmbedFirmware.cmake
#
# This used to be add_custom_command(OUTPUT ... DEPENDS ${_res_board_bmp}),
# which EmbedFirmware.cmake's header explains at length is an mtime comparison
# and therefore cannot express a content dependency. That argument is about the
# MECHANISM, not about what is being embedded, so it applies here word for word,
# and it was reproduced against this exact step before this was changed:
# board-recovery.bmp was edited in place and given a 2020 mtime, a plain
# `cmake --build` was run, and the generated .cpp came back byte-identical and
# with its old mtime. The build was green and the embedded diagram was not the
# one on disk. Having the file next door document that hole while this one still
# had it was the inconsistency worth removing.
#
# THE STAKES ARE NOT THE SAME, and pretending otherwise would overstate the
# case. EmbedFirmware.cmake is guarding bytes that get written to silicon on a
# CPU with no recovery path; this is a UI diagram, so a stale one shows somebody
# an out-of-date picture on the Recovery tab. The unification is for one
# convention across two adjacent files doing one job -- not a claim that the
# consequences are equal.
#
# The cost is correspondingly smaller too. SHA-256 over a 1.2 MB BMP is well
# under a millisecond, against ~0.04 s for the firmware step's ~21 MB, and
# embed_resource.py writes the .cpp only when its content changes, so the 3.7 MB
# translation unit downstream is not recompiled and the app is not relinked on
# an ordinary build. What it does add is one more command that always executes
# and one more line of build output on an otherwise idle build -- the same price
# EmbedFirmware.cmake already pays, and for the same reason.
add_custom_target(fwog_generate_resources
    BYPRODUCTS ${_res_board_src}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/embed_resource.py
            ${_res_board_bmp} boardRecovery ${_res_gen_dir}
    COMMENT "Checking embedded resource inputs"
    VERBATIM)

# Same target, same always-run reasoning, one more input. A separate COMMAND
# rather than a second target so there is still exactly one thing for
# add_dependencies() in CMakeLists.txt to hang off; embed_resource.py keys its
# stamp on the symbol, so the two invocations cannot overwrite each other's
# output.
#
# product.ico is embedded WHOLE and byte-for-byte, not converted to a BMP
# first. app.rc's comment is explicit that this file is a byte-for-byte copy of
# fwcom's icon so the two apps present one product rather than "two
# near-identical drawings that drift apart" -- and a converted BMP checked in
# beside it would be a third copy with exactly that problem. The runtime cost
# of the choice is one small parser (src/ui/fwIcoImage.cpp) and the ~30 KB of
# PNG-compressed 256x256 entry that decodeLargestIcoImage() skips.
if(_res_icon_src)
    set_property(TARGET fwog_generate_resources APPEND PROPERTY BYPRODUCTS ${_res_icon_src})
    add_custom_command(TARGET fwog_generate_resources POST_BUILD
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/embed_resource.py
                ${_res_icon_ico} appIcon ${_res_gen_dir}
        VERBATIM)
    set_source_files_properties(${_res_icon_src} PROPERTIES GENERATED TRUE)
endif()

# ${_res_board_src} is listed in fwOGAppExplorer's sources. BYPRODUCTS already
# marks it GENERATED, which is what keeps configure from failing on a clean tree
# where it does not exist yet; saying it again here is so that moving or
# rewriting the target above cannot silently take the property with it. Build
# ORDER comes from `add_dependencies(fwOGAppExplorer fwog_generate_resources)`
# in CMakeLists.txt, and both are load-bearing on a clean build. Same division
# of labour as EmbedFirmware.cmake.
set_source_files_properties(${_res_board_src} PROPERTIES GENERATED TRUE)
