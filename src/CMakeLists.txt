# Build the shared libs first — spikes and the plugin depend on them
if(SWAN_BUILD_SPIKE OR SWAN_BUILD_PLUGIN)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/wintun/include/wintun.h")
        add_subdirectory(wintun)
    endif()
endif()

if(SWAN_BUILD_WINDIVERT_SPIKE OR SWAN_BUILD_PLUGIN)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/windivert/include/windivert.h")
        add_subdirectory(windivert)
    endif()
endif()

if(SWAN_BUILD_GUI)
    add_subdirectory(gui)
endif()

if(SWAN_BUILD_SPIKE)
    add_subdirectory(wintun-spike)
endif()

if(SWAN_BUILD_WINDIVERT_SPIKE)
    add_subdirectory(windivert-spike)
endif()

if(SWAN_BUILD_PLUGIN)
    add_subdirectory(plugin/kernel-wintun)
endif()
