find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(dhc-vpn-gui WIN32
    main.cpp
    main_window.cpp
    main_window.h
    swanctl_runner.cpp
    swanctl_runner.h
    profile.cpp
    profile.h
    profile_dialog.cpp
    profile_dialog.h
    dpapi_secret.cpp
    dpapi_secret.h
    resources.qrc
)

target_link_libraries(dhc-vpn-gui PRIVATE
    Qt6::Widgets
    Qt6::Svg
    crypt32     # CryptProtectData / CryptUnprotectData -- DPAPI
)

set_target_properties(dhc-vpn-gui PROPERTIES
    OUTPUT_NAME "dhc-vpn"
)

# Deploy the Qt6 runtime DLLs next to the executable so it can be launched
# from any shell or via Explorer.
find_program(WINDEPLOYQT_EXECUTABLE NAMES windeployqt6 windeployqt
    HINTS "${Qt6_DIR}/../../../bin"
)
if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET dhc-vpn-gui POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations
                --no-system-d3d-compiler
                --no-opengl-sw
                --no-quick-import
                "$<TARGET_FILE:dhc-vpn-gui>"
        COMMENT "Running windeployqt"
        VERBATIM
    )
else()
    message(WARNING "windeployqt not found — Qt DLLs will not be deployed automatically")
endif()

# Recursively copy all MinGW runtime DLLs (libgcc, libstdc++, libicu, libb2,
# ...) next to the executable. windeployqt does not handle these.
if(MINGW)
    get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(BASH_EXECUTABLE bash)
    if(BASH_EXECUTABLE)
        add_custom_command(TARGET dhc-vpn-gui POST_BUILD
            COMMAND "${BASH_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/scripts/deploy-mingw-deps.sh"
                    "$<TARGET_FILE:dhc-vpn-gui>"
                    "${_mingw_bin}"
            COMMENT "Deploying MinGW runtime DLLs"
            VERBATIM
        )
    else()
        message(WARNING "bash not found — MinGW runtime DLLs will not be deployed")
    endif()
endif()
