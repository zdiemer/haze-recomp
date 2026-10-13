# Shared LGPL FFmpeg, pinned to a retained monthly build and verified archive.
# An unpacked development package can also be supplied for offline builds.
set(HAZE_FFMPEG_ROOT "" CACHE PATH "FFmpeg shared development package")
if(NOT HAZE_FFMPEG_ROOT)
    include(FetchContent)
    FetchContent_Declare(haze_ffmpeg
        URL https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08/ffmpeg-n8.1.3-9-g29e619e767-win64-lgpl-shared-8.1.zip
        URL_HASH SHA256=3e47bda1607740550141e37c0e49d1e5182b34699f15adfd137ee266d346811a)
    FetchContent_MakeAvailable(haze_ffmpeg)
    set(HAZE_FFMPEG_ROOT "${haze_ffmpeg_SOURCE_DIR}")
endif()
add_library(HazeMovieCodec INTERFACE)
target_include_directories(HazeMovieCodec INTERFACE "${HAZE_FFMPEG_ROOT}/include")
foreach(component avcodec avutil swscale swresample)
    # Respect a changed package root when reconfiguring an existing build.
    unset(HAZE_${component}_LIB CACHE)
    find_library(HAZE_${component}_LIB NAMES ${component} lib${component}
        PATHS "${HAZE_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(HazeMovieCodec INTERFACE "${HAZE_${component}_LIB}")
    file(GLOB component_dll "${HAZE_FFMPEG_ROOT}/bin/${component}-*.dll")
    if(NOT component_dll)
        message(FATAL_ERROR "Missing shared FFmpeg ${component} DLL")
    endif()
    list(APPEND HAZE_MOVIE_DLLS ${component_dll})
endforeach()
function(haze_movie_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E
        copy_if_different ${HAZE_MOVIE_DLLS} $<TARGET_FILE_DIR:${target}>)
endfunction()
install(FILES ${HAZE_MOVIE_DLLS} DESTINATION bin)
install(FILES "${HAZE_FFMPEG_ROOT}/LICENSE.txt" DESTINATION share/ffmpeg)
install(DIRECTORY "${HAZE_FFMPEG_ROOT}/doc/" DESTINATION share/ffmpeg OPTIONAL)
