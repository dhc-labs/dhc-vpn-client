# wintun_loader: small static lib shared by wintun-spike (M2) and the
# kernel-wintun plugin (M3+).

add_library(wintun_loader STATIC
    wintun_loader.c
    wintun_loader.h
)

target_include_directories(wintun_loader PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/third_party/wintun/include"
)

# wintun.h uses MSVC-only pragmas that GCC does not understand — silence them
target_compile_options(wintun_loader PRIVATE
    -Wno-unknown-pragmas
    -Wno-cast-function-type
)

# Path to the amd64 wintun.dll, exported as a CMake variable for consumers
set(WINTUN_DLL_PATH "${CMAKE_SOURCE_DIR}/third_party/wintun/bin/amd64/wintun.dll"
    CACHE INTERNAL "Path to wintun.dll (amd64)")
