# M2 spike: standalone program that exercises the Wintun driver.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/wintun/include/wintun.h")
    message(STATUS "wintun-spike: skipping — third_party/wintun/ not vendored yet")
    return()
endif()

add_executable(wintun-spike
    main.c
)

target_link_libraries(wintun-spike PRIVATE
    wintun_loader
    iphlpapi
    ws2_32
)

target_compile_options(wintun-spike PRIVATE
    -Wno-unknown-pragmas
    -Wno-cast-function-type
)

# Copy wintun.dll (amd64) next to the executable
if(EXISTS "${WINTUN_DLL_PATH}")
    add_custom_command(TARGET wintun-spike POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${WINTUN_DLL_PATH}"
                "$<TARGET_FILE_DIR:wintun-spike>/wintun.dll"
        VERBATIM
    )
endif()
