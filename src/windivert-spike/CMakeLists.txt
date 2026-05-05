# M7 step 1 spike: standalone tool that sniffs inbound UDP/4500 via
# WinDivert to verify capture-before-IPsec.sys.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/windivert/include/windivert.h")
    message(STATUS "windivert-spike: skipping — third_party/windivert/ not vendored yet")
    return()
endif()

add_executable(windivert-spike
    main.c
)

target_link_libraries(windivert-spike PRIVATE
    windivert_loader
    ws2_32
)

target_compile_options(windivert-spike PRIVATE
    -Wno-unknown-pragmas
    -Wno-cast-function-type
)

# Copy WinDivert.dll *and* WinDivert64.sys next to the executable. The
# DLL needs the .sys file in its own directory to install the driver on
# the first WinDivertOpen() call.
if(EXISTS "${WINDIVERT_DLL_PATH}")
    add_custom_command(TARGET windivert-spike POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${WINDIVERT_DLL_PATH}"
                "$<TARGET_FILE_DIR:windivert-spike>/WinDivert.dll"
        VERBATIM
    )
endif()
if(EXISTS "${WINDIVERT_SYS_PATH}")
    add_custom_command(TARGET windivert-spike POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${WINDIVERT_SYS_PATH}"
                "$<TARGET_FILE_DIR:windivert-spike>/WinDivert64.sys"
        VERBATIM
    )
endif()
