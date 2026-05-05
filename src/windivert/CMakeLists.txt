# windivert_loader: small static lib shared by windivert-spike (M7 step 1)
# and the kernel-wintun plugin (M7 step 2+).

add_library(windivert_loader STATIC
    windivert_loader.c
    windivert_loader.h
)

target_include_directories(windivert_loader PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/third_party/windivert/include"
)

target_compile_options(windivert_loader PRIVATE
    -Wno-unknown-pragmas
    -Wno-cast-function-type
)

# Paths to the amd64 WinDivert binaries, exported as CMake variables for
# consumers (spike + plugin).
set(WINDIVERT_DLL_PATH "${CMAKE_SOURCE_DIR}/third_party/windivert/x64/WinDivert.dll"
    CACHE INTERNAL "Path to WinDivert.dll (amd64)")
set(WINDIVERT_SYS_PATH "${CMAKE_SOURCE_DIR}/third_party/windivert/x64/WinDivert64.sys"
    CACHE INTERNAL "Path to WinDivert64.sys (amd64)")
