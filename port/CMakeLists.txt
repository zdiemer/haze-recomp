cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" HAZE_RELEASE_VERSION LIMIT_COUNT 1)
project(HazeRecomp
    VERSION ${HAZE_RELEASE_VERSION}
    LANGUAGES C CXX
    DESCRIPTION "Experimental native Haze port (ps3recomp)"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/ps3recomp" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../generated/ppu" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# All transformations share a CLI and may import other audited patch modules.
set(HAZE_PATCH_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/patch.py")
file(GLOB HAZE_PATCH_MODULES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/patches/*.py")
set(HAZE_PATCH_DEPENDS "${HAZE_PATCH_TOOL}" ${HAZE_PATCH_MODULES})

# Restore the audited local jump table in a build-local copy of the old lift.
# Fresh lifter output is deliberately rejected until its changed body is audited.
set(HAZE_DISPATCH_SOURCE "${RECOMP_DIR}/ppu_recomp_000.cpp")
if(EXISTS "${HAZE_DISPATCH_SOURCE}")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HAZE_DISPATCH_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_local_dispatch/ppu_recomp_000.cpp")
    add_custom_command(OUTPUT "${HAZE_DISPATCH_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" local-dispatch
            "${HAZE_DISPATCH_SOURCE}" "${HAZE_DISPATCH_PATCHED}"
        DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_DISPATCH_SOURCE}"
        VERBATIM)
    list(REMOVE_ITEM RECOMP_SOURCES "${HAZE_DISPATCH_SOURCE}")
    list(APPEND RECOMP_SOURCES "${HAZE_DISPATCH_PATCHED}")
    set_property(SOURCE "${HAZE_DISPATCH_PATCHED}" APPEND PROPERTY INCLUDE_DIRECTORIES "${RECOMP_DIR}")
endif()

# Keep the game-derived input untouched; fail closed on an unaudited main loop.
set(HAZE_PACER_SOURCE "${RECOMP_DIR}/ppu_recomp_003.cpp")
if(EXISTS "${HAZE_PACER_SOURCE}")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HAZE_PACER_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_guest_pacer/ppu_recomp_003.cpp")
    add_custom_command(OUTPUT "${HAZE_PACER_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" guest-pacer
            "${HAZE_PACER_SOURCE}" "${HAZE_PACER_PATCHED}"
        DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_PACER_SOURCE}"
        VERBATIM)
    set(HAZE_JOB_WAIT_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_job_wait/ppu_recomp_003.cpp")
    add_custom_command(OUTPUT "${HAZE_JOB_WAIT_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" job-wait
            "${HAZE_PACER_PATCHED}" "${HAZE_JOB_WAIT_PATCHED}"
        DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_PACER_PATCHED}"
        VERBATIM)
    set(HAZE_VP_UPLOAD_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_vp_upload/ppu_recomp_003.cpp")
    add_custom_command(OUTPUT "${HAZE_VP_UPLOAD_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" vp-upload
            "${HAZE_JOB_WAIT_PATCHED}" "${HAZE_VP_UPLOAD_PATCHED}"
        DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_JOB_WAIT_PATCHED}"
        VERBATIM)
    list(REMOVE_ITEM RECOMP_SOURCES "${HAZE_PACER_SOURCE}")
    set(HAZE_JOB_TRACE_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_job_trace/ppu_recomp_003.cpp")
    add_custom_command(OUTPUT "${HAZE_JOB_TRACE_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" job-dispatch-trace
            "${HAZE_VP_UPLOAD_PATCHED}" "${HAZE_JOB_TRACE_PATCHED}"
        DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_VP_UPLOAD_PATCHED}"
        VERBATIM)
    set(HAZE_SLOT_COPY_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_slot_copy/ppu_recomp_003.cpp")
    add_custom_command(OUTPUT "${HAZE_SLOT_COPY_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" slot-copy
            "${HAZE_JOB_TRACE_PATCHED}" --memmove-source "${RECOMP_DIR}/ppu_recomp_005.cpp"
            "${HAZE_SLOT_COPY_PATCHED}"
        DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_JOB_TRACE_PATCHED}" "${RECOMP_DIR}/ppu_recomp_005.cpp"
        VERBATIM)
    set(HAZE_PRODUCER_PROFILE_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_producer_profile/ppu_recomp_003.cpp")
    add_custom_command(OUTPUT "${HAZE_PRODUCER_PROFILE_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" producer-profile
            "${HAZE_SLOT_COPY_PATCHED}" "${HAZE_PRODUCER_PROFILE_PATCHED}"
        DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_SLOT_COPY_PATCHED}"
        VERBATIM)
    list(APPEND RECOMP_SOURCES "${HAZE_PRODUCER_PROFILE_PATCHED}")
    set_property(SOURCE "${HAZE_PRODUCER_PROFILE_PATCHED}" APPEND PROPERTY INCLUDE_DIRECTORIES "${RECOMP_DIR}")
    # Compose with the performance patches; compile one copy of this translation unit.
    set(HAZE_SAVE_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_save/ppu_recomp_003.cpp")
    add_custom_command(OUTPUT "${HAZE_SAVE_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" save-status
            "${HAZE_PRODUCER_PROFILE_PATCHED}" "${HAZE_SAVE_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" audio-tick
            "${HAZE_SAVE_PATCHED}" "${HAZE_SAVE_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" teammate-name
            "${HAZE_SAVE_PATCHED}" "${HAZE_SAVE_PATCHED}"
        DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_PRODUCER_PROFILE_PATCHED}"
        VERBATIM)
    list(REMOVE_ITEM RECOMP_SOURCES "${HAZE_PRODUCER_PROFILE_PATCHED}")
    list(APPEND RECOMP_SOURCES "${HAZE_SAVE_PATCHED}")
    set_property(SOURCE "${HAZE_SAVE_PATCHED}" APPEND PROPERTY INCLUDE_DIRECTORIES "${RECOMP_DIR}")
    set(HAZE_MAIN_LOOP_PATCHED "${HAZE_SAVE_PATCHED}")
    if(WIN32)
        set(HAZE_MOVIE_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_movie/ppu_recomp_003.cpp")
        add_custom_command(OUTPUT "${HAZE_MOVIE_PATCHED}"
            COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" movie-poll
                "${HAZE_SAVE_PATCHED}" "${HAZE_MOVIE_PATCHED}"
            COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" startup-splash
                "${HAZE_MOVIE_PATCHED}" "${HAZE_MOVIE_PATCHED}"
            DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_SAVE_PATCHED}"
            VERBATIM)
        list(REMOVE_ITEM RECOMP_SOURCES "${HAZE_SAVE_PATCHED}")
        list(APPEND RECOMP_SOURCES "${HAZE_MOVIE_PATCHED}" "${CMAKE_CURRENT_SOURCE_DIR}/haze_movie_guest.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/haze_startup_guest.cpp")
        set_property(SOURCE "${HAZE_MOVIE_PATCHED}" APPEND PROPERTY INCLUDE_DIRECTORIES "${RECOMP_DIR}")
        set(HAZE_MAIN_LOOP_PATCHED "${HAZE_MOVIE_PATCHED}")
    endif()

endif()

# Bounded native fill at an audited game boundary; original source stays local
# and untouched, and changed generated function bodies fail closed.
set(HAZE_BULK_FILL_SOURCE "${RECOMP_DIR}/ppu_recomp_006.cpp")
if(EXISTS "${HAZE_BULK_FILL_SOURCE}")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HAZE_BULK_FILL_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_bulk_fill/ppu_recomp_006.cpp")
    add_custom_command(OUTPUT "${HAZE_BULK_FILL_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" bulk-fill
            "${HAZE_BULK_FILL_SOURCE}" "${HAZE_BULK_FILL_PATCHED}"
            --caller-source "${RECOMP_DIR}/ppu_recomp_003.cpp"
        DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_BULK_FILL_SOURCE}"
            "${RECOMP_DIR}/ppu_recomp_003.cpp"
        VERBATIM)
    list(REMOVE_ITEM RECOMP_SOURCES "${HAZE_BULK_FILL_SOURCE}")
    list(APPEND RECOMP_SOURCES "${HAZE_BULK_FILL_PATCHED}")
    set_property(SOURCE "${HAZE_BULK_FILL_PATCHED}" APPEND PROPERTY INCLUDE_DIRECTORIES "${RECOMP_DIR}")
endif()

# ---------------------------------------------------------------------------
# Native GUI pointer bridge
# ---------------------------------------------------------------------------
set(HAZE_MENU_SOURCE "${RECOMP_DIR}/ppu_recomp_001.cpp")
if(EXISTS "${HAZE_MENU_SOURCE}")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HAZE_MENU_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_menu/ppu_recomp_001.cpp")
    add_custom_command(OUTPUT "${HAZE_MENU_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" menu-input
            "${HAZE_MENU_SOURCE}" "${HAZE_MENU_PATCHED}"
        DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_MENU_SOURCE}"
        VERBATIM)
    list(REMOVE_ITEM RECOMP_SOURCES "${HAZE_MENU_SOURCE}")
    list(APPEND RECOMP_SOURCES "${HAZE_MENU_PATCHED}")
    set_property(SOURCE "${HAZE_MENU_PATCHED}" APPEND PROPERTY INCLUDE_DIRECTORIES "${RECOMP_DIR}")
endif()
# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
set(HAZE_TITLE_SOURCE "${RECOMP_DIR}/ppu_recomp_002.cpp")
if(EXISTS "${HAZE_TITLE_SOURCE}")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HAZE_TITLE_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_title/ppu_recomp_002.cpp")
    add_custom_command(OUTPUT "${HAZE_TITLE_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" title-prompt
            "${HAZE_TITLE_SOURCE}" "${HAZE_TITLE_PATCHED}"
        DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_TITLE_SOURCE}"
        VERBATIM)
    list(REMOVE_ITEM RECOMP_SOURCES "${HAZE_TITLE_SOURCE}")
    list(APPEND RECOMP_SOURCES "${HAZE_TITLE_PATCHED}" "${CMAKE_CURRENT_SOURCE_DIR}/haze_title.cpp")
    set_property(SOURCE "${HAZE_TITLE_PATCHED}" APPEND PROPERTY INCLUDE_DIRECTORIES "${RECOMP_DIR}")
endif()

set(HAZE_MENU_SOURCE "${HAZE_MENU_PATCHED}")
if(EXISTS "${RECOMP_DIR}/ppu_recomp_001.cpp")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HAZE_MENU_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_menu_pages/ppu_recomp_001.cpp")
    add_custom_command(OUTPUT "${HAZE_MENU_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" menu-pages
            "${HAZE_MENU_SOURCE}" "${HAZE_MENU_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" menu-material
              "${HAZE_MENU_PATCHED}" "${HAZE_MENU_PATCHED}"
        COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" menu-lifetime
              "${HAZE_MENU_PATCHED}" "${HAZE_MENU_PATCHED}"
        DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_MENU_SOURCE}"
        VERBATIM)
    list(REMOVE_ITEM RECOMP_SOURCES "${HAZE_MENU_SOURCE}")
    list(APPEND RECOMP_SOURCES "${HAZE_MENU_PATCHED}" "${CMAKE_CURRENT_SOURCE_DIR}/haze_menu_guest.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/haze_audio_guest.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/haze_native_menu.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/haze_menu_cleanup.cpp")
    set_property(SOURCE "${HAZE_MENU_PATCHED}" APPEND PROPERTY INCLUDE_DIRECTORIES "${RECOMP_DIR}")
endif()

# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
option(PS3RECOMP_NATIVE_FLOAT_RUNTIME "Build CPU-dispatched native SPU arithmetic" ON)
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

option(HAZE_RSX_PROFILE "Collect native renderer CPU/GPU timing counters" OFF)
option(HAZE_JOB_DIAGNOSTICS "Collect Haze SPU job timing and trace diagnostics" OFF)
if(HAZE_RSX_PROFILE OR HAZE_JOB_DIAGNOSTICS)
    # Keep asynchronous SPU job hooks available without enabling the broader
    # PPU, queue, and renderer instrumentation used by ProfileRenderer.
    set_property(SOURCE "${PS3RECOMP_DIR}/runtime/spu/spu_workload.c"
        DIRECTORY "${PS3RECOMP_DIR}" APPEND PROPERTY
        COMPILE_DEFINITIONS YZ_PERF_PROFILE)
endif()
if(HAZE_RSX_PROFILE)
    set_property(SOURCE "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
        "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
        APPEND PROPERTY
        COMPILE_DEFINITIONS YZ_PERF_PROFILE)
    # PPU sources belong to HazeRecomp in this directory; the SPU and RSX
    # sources belong to the SDK target. Source properties are directory scoped.
    set_property(SOURCE "${PS3RECOMP_DIR}/libs/spurs/cellSpursQueue.c"
        DIRECTORY "${PS3RECOMP_DIR}" APPEND PROPERTY
        COMPILE_DEFINITIONS YZ_PERF_PROFILE)
    set_property(SOURCE "${PS3RECOMP_DIR}/libs/video/rsx_live_draw.c"
        "${PS3RECOMP_DIR}/libs/video/cellGcmSys.c"
        DIRECTORY "${PS3RECOMP_DIR}" APPEND PROPERTY
        COMPILE_DEFINITIONS YZ_PERF_PROFILE)
endif()

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
if(WIN32)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HAZE_SAVEDATA_SOURCE "${PS3RECOMP_DIR}/libs/system/cellSaveData.c")
    set(HAZE_SAVEDATA_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/haze_savedata/cellSaveData.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HAZE_SAVEDATA_SOURCE}" ${HAZE_PATCH_DEPENDS})
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" savedata
        "${HAZE_SAVEDATA_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}" "${HAZE_SAVEDATA_OUTPUT}"
        RESULT_VARIABLE HAZE_SAVEDATA_RESULT)
    if(NOT HAZE_SAVEDATA_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not prepare the audited Audio save transaction")
    endif()
    # The extension accesses private descriptor/resource state. Require an
    # explicit audit when the pinned renderer changes, including on LF checkouts.
    set(HAZE_RSX_SOURCE "${PS3RECOMP_DIR}/libs/video/rsx_live_draw.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HAZE_RSX_SOURCE}")
    file(READ "${HAZE_RSX_SOURCE}" HAZE_RSX_TEXT)
    string(REPLACE "\r\n" "\n" HAZE_RSX_TEXT "${HAZE_RSX_TEXT}")
    string(SHA256 HAZE_RSX_HASH "${HAZE_RSX_TEXT}")
    # Packed depth adds renderer-owned resources and descriptor slots. The live
    # settings transaction still fences before sampler/postprocess changes;
    # display rollback only replaces scanout buffers. Neither owns depth copies.
    # SDK 562bfac GPU pulling shares the existing fenced vertex-upload ring;
    # decoded geometry caches do not depend on live sampler/effect settings.
    # Dedicated packed-depth descriptors remain owned by the renderer.
    # SDK d6a543a preserves producer AA metadata and corrects packed-depth
    # sample coordinates/bounds. Resource and descriptor ownership is unchanged.
    # Terminal shader-bundle persistence detaches only source/blob caches after
    # the final draw. Its worker owns those allocations; settings transactions
    # and GPU resource/display lifetime remain on the renderer owner thread.
    # Exact-validated fragment-program plans own CPU metadata only. Mutable
    # constants, PSO keys, descriptors, GPU fences and settings stay unchanged.
    # Exact pipeline recipes retain independent PSO/root/device references.
    # Live settings and display rollback retain their existing fence/ownership.
    if(NOT HAZE_RSX_HASH STREQUAL "d81d3ea5b8ab00c1053cc43b1072d06fd175dcbd0a607baf9b53802f9eeaf731")
        message(FATAL_ERROR "Renderer changed: audit the Haze live-settings transaction before building")
    endif()
    # Capture while DXGI still owns this backbuffer, before Present rotates it.
    string(REPLACE "    g.ready = 1;" "    g.ready = 1;\n    haze_overlay_start();"
        HAZE_RSX_TEXT "${HAZE_RSX_TEXT}")
    string(REPLACE "    ld_flush(LD_FLUSH_MOVIE_PRESENT);" "    haze_overlay_draw(bbi);\n    ld_flush(LD_FLUSH_MOVIE_PRESENT);"
        HAZE_RSX_TEXT "${HAZE_RSX_TEXT}")
    # Match only the final presentation flush, not settings/readback flushes.
    string(REPLACE "    ld_flush(LD_FLUSH_PRESENT);\n    {" "    haze_overlay_draw(bbi);\n    ld_flush(LD_FLUSH_PRESENT);\n    {"
        HAZE_RSX_TEXT "${HAZE_RSX_TEXT}")
    string(REPLACE "    rsx_postprocess_destroy(&g_ld_postprocess);"
        "    haze_overlay_destroy();\n    rsx_postprocess_destroy(&g_ld_postprocess);"
        HAZE_RSX_TEXT "${HAZE_RSX_TEXT}")
    string(REPLACE "ld_flush(LD_FLUSH_MOVIE_PRESENT);"
        "ld_flush(LD_FLUSH_MOVIE_PRESENT);\n    haze_movie_capture_backbuffer(bbi);"
        HAZE_RSX_TEXT "${HAZE_RSX_TEXT}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/haze_rsx_live_draw.c" "${HAZE_RSX_TEXT}")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/haze_video_backend.c"
        TARGET_DIRECTORY ps3recomp_runtime APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
    get_target_property(HAZE_SDK_SOURCES ps3recomp_runtime SOURCES)
    list(REMOVE_ITEM HAZE_SDK_SOURCES "${HAZE_RSX_SOURCE}")
    list(REMOVE_ITEM HAZE_SDK_SOURCES "${HAZE_SAVEDATA_SOURCE}")
    list(APPEND HAZE_SDK_SOURCES "${HAZE_SAVEDATA_OUTPUT}")
    list(APPEND HAZE_SDK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/haze_video_backend.c")
    set(HAZE_AUDIO_SOURCE "${PS3RECOMP_DIR}/libs/audio/cellAudio.c")
    file(READ "${HAZE_AUDIO_SOURCE}" HAZE_AUDIO_TEXT)
    string(REPLACE "\r\n" "\n" HAZE_AUDIO_TEXT "${HAZE_AUDIO_TEXT}")
    string(SHA256 HAZE_AUDIO_HASH "${HAZE_AUDIO_TEXT}")
    # Audit both the output shutdown and pre-clamp limiter hooks on SDK changes.
    if(NOT HAZE_AUDIO_HASH STREQUAL "b174688dc8ae62fa67f35888477b5543310cdfee33e6f613e9bb796b7b0c0504")
        message(FATAL_ERROR "Audio backend changed: audit native output shutdown and limiter before building")
    endif()
    string(REPLACE "    /* Clip to [-1.0, 1.0] */"
        "    haze_audio_limiter_process(&s_haze_audio_limiter, s_mix_buffer);\n    /* Clip to [-1.0, 1.0] */"
        HAZE_AUDIO_TEXT "${HAZE_AUDIO_TEXT}")
    string(REPLACE "    if (audio_start_mix_thread() < 0) {"
        "    haze_audio_limiter_reset(&s_haze_audio_limiter);\n    if (audio_start_mix_thread() < 0) {"
        HAZE_AUDIO_TEXT "${HAZE_AUDIO_TEXT}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/haze_cell_audio.c" "${HAZE_AUDIO_TEXT}")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/haze_audio_backend.c"
        TARGET_DIRECTORY ps3recomp_runtime APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
    list(REMOVE_ITEM HAZE_SDK_SOURCES "${HAZE_AUDIO_SOURCE}")
    list(APPEND HAZE_SDK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/haze_audio_backend.c")
    file(READ "${PS3RECOMP_DIR}/libs/codec/cellSail.c" HAZE_SAIL_TEXT)
    string(REPLACE "\r\n" "\n" HAZE_SAIL_TEXT "${HAZE_SAIL_TEXT}")
    string(SHA256 HAZE_SAIL_HASH "${HAZE_SAIL_TEXT}")
    if(NOT HAZE_SAIL_HASH STREQUAL "211a0bbfd9423b57f339817a41b7617a759122f5c8ce1f52df7e4c57c7ef3040")
        message(FATAL_ERROR "SAIL changed: audit the Haze playback lifecycle bridge")
    endif()
    list(REMOVE_ITEM HAZE_SDK_SOURCES "${PS3RECOMP_DIR}/libs/codec/cellSail.c")
    list(APPEND HAZE_SDK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/haze_sail_backend.c")
    set_property(TARGET ps3recomp_runtime PROPERTY SOURCES "${HAZE_SDK_SOURCES}")
    if(HAZE_RSX_PROFILE)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/haze_video_backend.c"
            TARGET_DIRECTORY ps3recomp_runtime APPEND PROPERTY COMPILE_DEFINITIONS YZ_PERF_PROFILE)
    endif()
endif()

# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_native_memory.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../generated/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(SPU_SOURCES "")
if(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        # Patch a build-local copy, never the shared generated/game sources.
        set(HAZE_IDLE_SOURCE "${SPU_DIR}/spu_0028_at_00EC7980/spu_recomp.c")
        if(EXISTS "${HAZE_IDLE_SOURCE}")
            find_package(Python3 COMPONENTS Interpreter REQUIRED)
            set(HAZE_IDLE_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_spu_idle/spu_recomp.c")
            add_custom_command(OUTPUT "${HAZE_IDLE_PATCHED}"
                COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" spu-idle
                    "${HAZE_IDLE_SOURCE}" "${HAZE_IDLE_PATCHED}"
                    --registration "${SPU_DIR}/spu_workloads.c"
                DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_IDLE_SOURCE}" "${SPU_DIR}/spu_workloads.c"
                VERBATIM)
            list(REMOVE_ITEM SPU_LIFTED "${HAZE_IDLE_SOURCE}")
            list(APPEND SPU_LIFTED "${HAZE_IDLE_PATCHED}")
            set_property(SOURCE "${HAZE_IDLE_PATCHED}" APPEND PROPERTY
                INCLUDE_DIRECTORIES "${SPU_DIR}/spu_0028_at_00EC7980")
        endif()
        set(HAZE_TRIPLET_SOURCE "${SPU_DIR}/spu_0041_at_00F2E380/spu_recomp.c")
        if(EXISTS "${HAZE_TRIPLET_SOURCE}")
            find_package(Python3 COMPONENTS Interpreter REQUIRED)
            set(HAZE_TRIPLET_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_spu_triplet/spu_recomp.c")
            add_custom_command(OUTPUT "${HAZE_TRIPLET_PATCHED}"
                COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" spu-triplet-copy
                    "${HAZE_TRIPLET_SOURCE}" "${HAZE_TRIPLET_PATCHED}"
                    --registration "${SPU_DIR}/spu_workloads.c"
                DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_TRIPLET_SOURCE}" "${SPU_DIR}/spu_workloads.c"
                VERBATIM)
            list(REMOVE_ITEM SPU_LIFTED "${HAZE_TRIPLET_SOURCE}")
            list(APPEND SPU_LIFTED "${HAZE_TRIPLET_PATCHED}")
            set_property(SOURCE "${HAZE_TRIPLET_PATCHED}" APPEND PROPERTY INCLUDE_DIRECTORIES
                "${SPU_DIR}/spu_0041_at_00F2E380" "${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
        set(HAZE_CURSOR_SOURCE "${SPU_DIR}/spu_0039_at_00F1C100/spu_recomp.c")
        if(EXISTS "${HAZE_CURSOR_SOURCE}")
            find_package(Python3 COMPONENTS Interpreter REQUIRED)
            set(HAZE_CURSOR_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_native_cursor/spu_recomp.c")
            add_custom_command(OUTPUT "${HAZE_CURSOR_PATCHED}"
                COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" native-cursor
                    "${HAZE_CURSOR_SOURCE}" "${HAZE_CURSOR_PATCHED}"
                    --registration "${SPU_DIR}/spu_workloads.c"
                DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_CURSOR_SOURCE}" "${SPU_DIR}/spu_workloads.c"
                VERBATIM)
            list(REMOVE_ITEM SPU_LIFTED "${HAZE_CURSOR_SOURCE}")
            list(APPEND SPU_LIFTED "${HAZE_CURSOR_PATCHED}")
            set_property(SOURCE "${HAZE_CURSOR_PATCHED}" APPEND PROPERTY INCLUDE_DIRECTORIES
                "${SPU_DIR}/spu_0039_at_00F1C100" "${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
        set(HAZE_JOB0_SOURCE "${SPU_DIR}/spu_0029_at_00EC9A80/spu_recomp.c")
        if(EXISTS "${HAZE_JOB0_SOURCE}")
            find_package(Python3 COMPONENTS Interpreter REQUIRED)
            set(HAZE_JOB0_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/haze_job0_capture/spu_recomp.c")
            add_custom_command(OUTPUT "${HAZE_JOB0_PATCHED}"
                COMMAND "${Python3_EXECUTABLE}" "${HAZE_PATCH_TOOL}" job0-kernel
                    "${HAZE_JOB0_SOURCE}" "${HAZE_JOB0_PATCHED}"
                    --registration "${SPU_DIR}/spu_workloads.c"
                DEPENDS ${HAZE_PATCH_DEPENDS} "${HAZE_JOB0_SOURCE}" "${SPU_DIR}/spu_workloads.c"
                VERBATIM)
            list(REMOVE_ITEM SPU_LIFTED "${HAZE_JOB0_SOURCE}")
            list(APPEND SPU_LIFTED "${HAZE_JOB0_PATCHED}")
            set_property(SOURCE "${HAZE_JOB0_PATCHED}" APPEND PROPERTY INCLUDE_DIRECTORIES
                "${SPU_DIR}/spu_0029_at_00EC9A80" "${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c firmware_register.c audio_plugins_register.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

add_executable(${PROJECT_NAME}
    main.cpp
    stubs.cpp
    haze_input.cpp
    haze_camera.cpp
    haze_menu.cpp
    haze_player_profile.cpp
    haze_player_profile_guest.cpp
    haze_player_profile_menu.cpp
    haze_guest_pacer.cpp
    haze_job_wait.cpp
    haze_bulk_fill.cpp
    haze_slot_copy.cpp
    haze_producer_profile.cpp
    haze_job_profile.cpp
    haze_job_trace.cpp
    haze_vp_upload.cpp
    haze_native_cursor.cpp
    haze_job0_capture.cpp
    haze_job0_kernel.cpp
    haze_spu_triplet_copy.cpp
    haze_graphics.cpp
    haze_graphics_live.cpp
    haze_menu_pointer.cpp
    haze_video_menu.cpp
    haze_audio_menu.cpp
    haze_save_status.cpp
    haze_quit.cpp
    haze_quit_guest.cpp
    haze_quit_menu.cpp
    haze_display.cpp
    haze_display_preview.cpp
    haze_spu_idle.c
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)
if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE haze_profile_transaction.cpp)
    add_subdirectory(net)
    target_sources(${PROJECT_NAME} PRIVATE haze_coop.cpp haze_coop_menu.cpp haze_coop_settings.cpp)
    target_link_libraries(${PROJECT_NAME} PRIVATE haze_network)
endif()
set_property(SOURCE haze_spu_idle.c APPEND PROPERTY
    INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu")

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
option(HAZE_SPU_FMA "Use AVX/FMA3 for lifted SPU arithmetic (requires CPU support)" OFF)
option(HAZE_SPU_BYTE_COMPARE "Compile live scalar/SIMD SPU byte-operation comparisons" OFF)
if(SPU_SOURCES)
    set_property(SOURCE ${SPU_SOURCES} APPEND PROPERTY
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu")
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    if(PS3RECOMP_NATIVE_FLOAT_RUNTIME AND WIN32 AND CMAKE_C_COMPILER_ID MATCHES "Clang")
        set_property(SOURCE ${SPU_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS SPU_FLOAT_NATIVE_RUNTIME=1)
    endif()
    if(HAZE_SPU_BYTE_COMPARE)
        # Explicit experiment: these extra branches can change inlining.
        set_property(SOURCE ${SPU_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS SPU_COMPARE_BYTE_OPS)
    endif()
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64")
            # The SDK's PRIVATE SSE4.1 flag never reached the lifted SPU TUs.
            # Without this, their hot byte-shuffle helper uses a scalar loop.
            set_property(SOURCE ${SPU_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -msse4.1)
            if(HAZE_SPU_FMA)
                # Preserve fused rounding; do not enable fast-math or reassociation.
                set_property(SOURCE ${SPU_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -mavx -mfma)
                set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS HAZE_REQUIRE_FMA)
            endif()
        endif()
    endif()
endif()

# Scope the fast float environment to the Multistream audio worker. Do not
# change the SDK checkout: wrap its halt boundary in this build only.
if(PORT_ENABLE_SPU AND PS3RECOMP_NATIVE_FLOAT_RUNTIME AND WIN32
        AND CMAKE_C_COMPILER_ID MATCHES "Clang"
        AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64")
    file(READ "${PS3RECOMP_DIR}/runtime/spu/spu_channels.c" HAZE_SPU_CHANNELS_TEXT)
    string(REPLACE "\r\n" "\n" HAZE_SPU_CHANNELS_TEXT "${HAZE_SPU_CHANNELS_TEXT}")
    string(SHA256 HAZE_SPU_CHANNELS_HASH "${HAZE_SPU_CHANNELS_TEXT}")
    if(NOT HAZE_SPU_CHANNELS_HASH STREQUAL "d524a6455cc709c751b23e3c83c041677d345b049042f41a69f2e92dbeb87f0f")
        message(FATAL_ERROR "SPU halt boundary changed: audit audio float environment restoration before building")
    endif()
    file(GLOB HAZE_AUDIO_DSP_SOURCES "${SPU_DIR}/audio_pic_*/spu_recomp.c"
        "${SPU_DIR}/firmware_atrac/spu_recomp.c"
        "${SPU_DIR}/spu_0026_at_00DAFD00/spu_recomp.c")
    set_property(SOURCE ${HAZE_AUDIO_DSP_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/haze_audio_spu_float.h" "-ffp-contract=off")
    set(HAZE_AUDIO_FLOAT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/haze_audio_spu_float.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/haze_audio_spu_double.c")
    target_sources(ps3recomp_runtime PRIVATE ${HAZE_AUDIO_FLOAT_SOURCES})
    set_property(SOURCE ${HAZE_AUDIO_FLOAT_SOURCES} TARGET_DIRECTORY ps3recomp_runtime
        APPEND PROPERTY INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu")
    set_property(SOURCE ${HAZE_AUDIO_FLOAT_SOURCES} TARGET_DIRECTORY ps3recomp_runtime
        APPEND PROPERTY COMPILE_OPTIONS "-msse4.1" "-ffp-contract=off")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/haze_audio_spu_double.c"
        TARGET_DIRECTORY ps3recomp_runtime APPEND PROPERTY COMPILE_OPTIONS "-ffp-model=strict")
    set_property(SOURCE "${PS3RECOMP_DIR}/runtime/spu/spu_channels.c"
        TARGET_DIRECTORY ps3recomp_runtime APPEND PROPERTY COMPILE_DEFINITIONS
        spu_run_with_halt=haze_sdk_spu_run_with_halt)
endif()

# LLVM 24's Wasm SIMD selector crashes when vectorizing this image's packed
# unsigned-to-float conversion (WebAssemblyISD::CONVERT_LOW_U). Preserve its
# scalar arithmetic until the compiler issue is resolved; other images retain SIMD.
if(EMSCRIPTEN AND EXISTS "${SPU_DIR}/spu_0022_at_00D15F80/spu_recomp.c")
    set_property(SOURCE "${SPU_DIR}/spu_0022_at_00D15F80/spu_recomp.c" APPEND PROPERTY
        COMPILE_OPTIONS -fno-vectorize -fno-slp-vectorize)
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()
include(windows-icon.cmake)
haze_local_icon(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${PS3RECOMP_DIR}/libs/input"
    "${PS3RECOMP_DIR}/libs/codec"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
if(HAZE_RSX_PROFILE OR HAZE_JOB_DIAGNOSTICS)
    set_property(SOURCE "${HAZE_MAIN_LOOP_PATCHED}" main.cpp haze_job_profile.cpp
        APPEND PROPERTY COMPILE_DEFINITIONS HAZE_JOB_PROFILE)
    set_property(SOURCE "${HAZE_MAIN_LOOP_PATCHED}" main.cpp haze_job_trace.cpp
        haze_job0_capture.cpp "${HAZE_JOB0_PATCHED}"
        APPEND PROPERTY COMPILE_DEFINITIONS HAZE_JOB_TRACE)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

if(WIN32)
    include(controller-dependency.cmake)
    include(movie-dependency.cmake)
    target_sources(${PROJECT_NAME} PRIVATE haze_movie.cpp haze_movie_decode.cpp)
    target_link_libraries(${PROJECT_NAME} PRIVATE HazeMovieCodec)
    haze_movie_runtime(${PROJECT_NAME})
    add_executable(HazeMovieDecodeTest EXCLUDE_FROM_ALL ../tests/test_movie_decode.cpp haze_movie_decode.cpp)
    target_include_directories(HazeMovieDecodeTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(HazeMovieDecodeTest PRIVATE HazeMovieCodec)
    target_compile_options(HazeMovieDecodeTest PRIVATE /UNDEBUG)
    haze_movie_runtime(HazeMovieDecodeTest)
    add_executable(HazeMoviePlaybackTest EXCLUDE_FROM_ALL ../tests/test_movie_playback.cpp haze_movie.cpp haze_movie_decode.cpp)
    target_include_directories(HazeMoviePlaybackTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(HazeMoviePlaybackTest PRIVATE HazeMovieCodec SDL3::SDL3)
    target_compile_options(HazeMoviePlaybackTest PRIVATE /UNDEBUG)
    haze_movie_runtime(HazeMoviePlaybackTest)
    add_executable(HazeMovieSailTest EXCLUDE_FROM_ALL ../tests/test_movie_sail.c)
    target_include_directories(HazeMovieSailTest PRIVATE "${PS3RECOMP_DIR}/include" "${PS3RECOMP_DIR}/libs/codec")
    target_compile_options(HazeMovieSailTest PRIVATE /UNDEBUG)
    add_executable(HazeMoviePamfTest EXCLUDE_FROM_ALL ../tests/test_movie_pamf.cpp)
    target_include_directories(HazeMoviePamfTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(HazeMoviePamfTest PRIVATE /UNDEBUG)
    add_subdirectory(launcher)
    target_sources(${PROJECT_NAME} PRIVATE haze_gamepad.cpp haze_pad_guest.cpp haze_prompts.cpp haze_controls_settings.cpp haze_controls_menu.cpp)
    target_sources(${PROJECT_NAME} PRIVATE haze_steam_vdf.cpp haze_steam_names.cpp haze_steam_win32.cpp haze_steam_art.cpp haze_steam_menu.cpp)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3 windowscodecs ole32 comdlg32 advapi32)
    if(TARGET SDL3::SDL3-shared)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:${PROJECT_NAME}>)
        install(FILES $<TARGET_FILE:SDL3::SDL3-shared> DESTINATION bin)
    endif()
endif()

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
    )
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
if(MSVC)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off")
endif()

# This unproven code-size tradeoff is excluded from ordinary builds entirely.
# PPU_INLINE_READS only changes execution within a deliberately compiled probe.
option(HAZE_EXPERIMENT_INLINE_READS "Compile the developer inline-read comparison" OFF)
# Ordinary guest reads may be inlined into lifted host code. The runtime owns
# the startup eligibility decision; special addresses and diagnostics retain
# their original accessor. Never apply this remapping to runtime/HLE sources.
if(HAZE_EXPERIMENT_INLINE_READS AND MSVC)
    set_property(SOURCE ${RECOMP_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/haze_ppu_inline_reads.h")
elseif(HAZE_EXPERIMENT_INLINE_READS)
    set_property(SOURCE ${RECOMP_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
        "-include" "${CMAKE_CURRENT_SOURCE_DIR}/haze_ppu_inline_reads.h")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
    option(HAZE_WASM_SHADER_COMPILER "Build the in-browser HLSL to GLSL compiler" ON)
    if(HAZE_WASM_SHADER_COMPILER)
        add_subdirectory(wasm)
        target_sources(${PROJECT_NAME} PRIVATE wasm/webgl_backend.cpp)
        set_source_files_properties(wasm/webgl_backend.cpp PROPERTIES COMPILE_OPTIONS -fwasm-exceptions)
        target_link_libraries(${PROJECT_NAME} PRIVATE HazeWasmShaders)
        set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS HAZE_WASM_WEBGL)
        target_link_options(${PROJECT_NAME} PRIVATE -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
            -sOFFSCREENCANVAS_SUPPORT=1 --pre-js=${CMAKE_CURRENT_SOURCE_DIR}/wasm/canvas.js)
        set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/wasm/canvas.js")
    endif()
    add_executable(HazeWasmSmoke ../tests/wasm_smoke.cpp wasm/guest_memory.cpp)
    target_include_directories(HazeWasmSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${RECOMP_DIR}"
        "${PS3RECOMP_DIR}/runtime/platform")
    target_link_libraries(HazeWasmSmoke PRIVATE ps3recomp_runtime)
    target_compile_options(HazeWasmSmoke PRIVATE -UNDEBUG)
    target_link_options(HazeWasmSmoke PRIVATE
        -Wl,--wrap=_Z13func_00C07818P11ppu_context
        -sMEMORY64=1 -sMAXIMUM_MEMORY=16GB -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=128MB -sSTACK_SIZE=4MB -sPROXY_TO_PTHREAD=1
        -sPTHREAD_POOL_SIZE=4 -sEXIT_RUNTIME=1
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createHaze
        -sEXPORTED_RUNTIME_METHODS=FS,callMain,WORKERFS,IDBFS,ENV
        -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1 -lidbfs.js -lworkerfs.js)
    set_target_properties(HazeWasmSmoke PROPERTIES SUFFIX ".js")
    target_compile_options(${PROJECT_NAME} PRIVATE -pthread -msimd128 -sMEMORY64=1)
    target_link_options(${PROJECT_NAME} PRIVATE
        -pthread -sMEMORY64=1 -sMAXIMUM_MEMORY=16GB
        -sINITIAL_MEMORY=128MB -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=4MB -sDEFAULT_PTHREAD_STACK_SIZE=4MB
        -sSTACK_OVERFLOW_CHECK=2
        -sPTHREAD_POOL_SIZE=32 -sPROXY_TO_PTHREAD=1
        # The SDK's generic HLE bridge calls C firmware functions through an
        # eight-u64 signature. Wasm checks indirect-call types strictly.
        -sEMULATE_FUNCTION_POINTER_CASTS=1 --profiling-funcs
        -sEXIT_RUNTIME=1 -sFORCE_FILESYSTEM=1
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createHaze
        -sEXPORTED_RUNTIME_METHODS=FS,callMain,WORKERFS,IDBFS,ENV
        -sINVOKE_RUN=0 -lidbfs.js -lworkerfs.js)
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".js")
    if(HAZE_WASM_SHADER_COMPILER)
        add_executable(HazeWebGLSmoke ../tests/wasm_webgl_smoke.cpp wasm/webgl_backend.cpp)
        target_include_directories(HazeWebGLSmoke PRIVATE "${PS3RECOMP_DIR}/include" "${PS3RECOMP_DIR}/libs/video")
        target_compile_options(HazeWebGLSmoke PRIVATE -pthread -sMEMORY64=1 -msimd128 -fwasm-exceptions -UNDEBUG)
        target_link_libraries(HazeWebGLSmoke PRIVATE HazeWasmShaders ps3recomp_runtime)
        get_target_property(HAZE_WASM_LINK_OPTIONS ${PROJECT_NAME} LINK_OPTIONS)
        target_link_options(HazeWebGLSmoke PRIVATE ${HAZE_WASM_LINK_OPTIONS})
        set_property(TARGET HazeWebGLSmoke APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/wasm/canvas.js")
        set_target_properties(HazeWebGLSmoke PROPERTIES SUFFIX ".js")
    endif()
    target_sources(${PROJECT_NAME} PRIVATE wasm/guest_memory.cpp wasm/audio_bridge.cpp)
    target_link_options(${PROJECT_NAME} PRIVATE -sEXPORTED_RUNTIME_METHODS=FS,callMain,WORKERFS,IDBFS,ENV,HEAPU8)
    add_executable(HazeAudioSmoke ../tests/wasm_audio_smoke.cpp wasm/audio_bridge.cpp)
    target_compile_options(HazeAudioSmoke PRIVATE -pthread -sMEMORY64=1 -msimd128 -UNDEBUG)
    get_target_property(HAZE_AUDIO_LINK_OPTIONS ${PROJECT_NAME} LINK_OPTIONS)
    list(FILTER HAZE_AUDIO_LINK_OPTIONS EXCLUDE REGEX "--pre-js=|OFFSCREENCANVAS_SUPPORT")
    target_link_options(HazeAudioSmoke PRIVATE ${HAZE_AUDIO_LINK_OPTIONS})
    set_target_properties(HazeAudioSmoke PROPERTIES SUFFIX ".js")
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,--wrap=_Z13func_00C07818P11ppu_context)
endif()
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
if(WIN32)
    install(FILES ../config/input.ini DESTINATION bin/config)
    install(DIRECTORY ../assets/input-prompts DESTINATION bin/assets)
endif()
