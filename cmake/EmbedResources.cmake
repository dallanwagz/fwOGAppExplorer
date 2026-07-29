# Binary resources compiled into the executable, alongside the firmware images
# EmbedFirmware.cmake handles. Same tool shape, same reason: this app ships as a
# single .exe, and the Recovery tab's board diagram is the one asset that must
# be present exactly when somebody's board will not enumerate.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_res_gen_dir "${CMAKE_BINARY_DIR}/generated")
set(_res_board_src "${_res_gen_dir}/fwEmbeddedResource_boardRecovery.cpp")
set(_res_board_bmp "${CMAKE_SOURCE_DIR}/resources/board-recovery.bmp")

# BMP, not PNG: SDL3's core ships SDL_LoadBMP and no PNG decoder, so this is the
# one raster format the app can read without adding a dependency. It is bulky on
# disk and that is fine -- embed_resource.py deflates it, and only the deflated
# bytes reach the binary.
add_custom_command(
    OUTPUT  ${_res_board_src}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/embed_resource.py
            ${_res_board_bmp} boardRecovery ${_res_gen_dir}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/embed_resource.py
            ${_res_board_bmp}
    COMMENT "Embedding board recovery diagram"
    VERBATIM)

add_custom_target(fwog_generate_resources DEPENDS ${_res_board_src})
