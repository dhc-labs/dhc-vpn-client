cmake_minimum_required(VERSION 3.21)

project(dhc-vpn-client
    VERSION 0.0.1
    DESCRIPTION "Open-source IKEv2 VPN client for Windows (strongSwan + Qt6)"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT WIN32)
    message(FATAL_ERROR "This project is Windows-only.")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

option(SWAN_BUILD_GUI            "Build Qt6 GUI"                     ON)
option(SWAN_BUILD_PLUGIN         "Build kernel-wintun plugin"        OFF)  # enable when needed
option(SWAN_BUILD_SPIKE          "Build wintun standalone spike"     ON)
option(SWAN_BUILD_WINDIVERT_SPIKE "Build windivert standalone spike" ON)

add_subdirectory(src)
