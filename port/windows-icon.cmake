include_guard(GLOBAL)
function(haze_local_icon target)
    if(NOT WIN32)
        return()
    endif()
    enable_language(RC)
    find_program(HAZE_ICON_POWERSHELL NAMES powershell REQUIRED)
    get_filename_component(icon_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    if(DEFINED HAZE_SHARED_ROOT)
        set(icon_assets "${HAZE_SHARED_ROOT}")
    else()
        set(icon_assets "${icon_root}")
    endif()
    set(icon_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}-icon")
    set(icon_rc "${icon_dir}/app.rc")
    # Run each build so adding/removing local artwork is noticed without a
    # reconfigure. The converter preserves output timestamps when unchanged.
    add_custom_target(${target}_local_icon
        COMMAND "${HAZE_ICON_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${icon_root}/scripts/build-icon.ps1"
            -Source "${icon_assets}/game/disc/PS3_GAME/ICON0.PNG"
            -FallbackSource "${icon_root}/assets/haze-logo.png"
            -Output "${icon_dir}/app.ico" -Resource "${icon_rc}"
        BYPRODUCTS "${icon_dir}/app.ico" "${icon_rc}"
        VERBATIM)
    add_dependencies(${target} ${target}_local_icon)
    target_sources(${target} PRIVATE "${icon_rc}")
    set_source_files_properties("${icon_rc}" PROPERTIES
        GENERATED TRUE OBJECT_DEPENDS "${icon_dir}/app.ico")
endfunction()
