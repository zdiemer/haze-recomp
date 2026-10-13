cmake_minimum_required(VERSION 3.24)
# Called from an initialized VS developer environment so dumpbin and the
# app-local redistributables are resolved without relying on the system CRT.
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${GAME}" "${LAUNCHER}"
    DIRECTORIES ${SEARCH_DIRECTORIES}
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    PRE_EXCLUDE_REGEXES "[Aa][Pp][Ii]-[Mm][Ss]-.*" "[Ee][Xx][Tt]-[Mm][Ss]-.*"
    POST_EXCLUDE_REGEXES ".*[/\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\].*")
if(unresolved)
    message(FATAL_ERROR "Unresolved runtime DLLs: ${unresolved}")
endif()
file(COPY "${GAME}" "${LAUNCHER}" ${resolved} DESTINATION "${OUTPUT}")
