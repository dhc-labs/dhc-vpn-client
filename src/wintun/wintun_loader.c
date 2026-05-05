#include "wintun_loader.h"

#include <stdio.h>

WINTUN_CREATE_ADAPTER_FUNC             *WintunCreateAdapter;
WINTUN_OPEN_ADAPTER_FUNC               *WintunOpenAdapter;
WINTUN_CLOSE_ADAPTER_FUNC              *WintunCloseAdapter;
WINTUN_DELETE_DRIVER_FUNC              *WintunDeleteDriver;
WINTUN_GET_ADAPTER_LUID_FUNC           *WintunGetAdapterLUID;
WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC *WintunGetRunningDriverVersion;
WINTUN_SET_LOGGER_FUNC                 *WintunSetLogger;
WINTUN_START_SESSION_FUNC              *WintunStartSession;
WINTUN_END_SESSION_FUNC                *WintunEndSession;
WINTUN_GET_READ_WAIT_EVENT_FUNC        *WintunGetReadWaitEvent;
WINTUN_RECEIVE_PACKET_FUNC             *WintunReceivePacket;
WINTUN_RELEASE_RECEIVE_PACKET_FUNC     *WintunReleaseReceivePacket;
WINTUN_ALLOCATE_SEND_PACKET_FUNC       *WintunAllocateSendPacket;
WINTUN_SEND_PACKET_FUNC                *WintunSendPacket;

static HMODULE g_dll = NULL;

#define RESOLVE(typedef_name, var_name, sym)                                  \
    do {                                                                      \
        var_name = (typedef_name *)(void(*)(void))GetProcAddress(g_dll, sym); \
        if (!var_name) {                                                      \
            DWORD e = GetLastError();                                         \
            FreeLibrary(g_dll); g_dll = NULL;                                 \
            return e ? e : ERROR_PROC_NOT_FOUND;                              \
        }                                                                     \
    } while (0)

DWORD wintun_load(void)
{
    if (g_dll) {
        return 0;
    }

    g_dll = LoadLibraryExW(
        L"wintun.dll", NULL,
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_dll) {
        return GetLastError();
    }

    RESOLVE(WINTUN_CREATE_ADAPTER_FUNC,             WintunCreateAdapter,           "WintunCreateAdapter");
    RESOLVE(WINTUN_OPEN_ADAPTER_FUNC,               WintunOpenAdapter,             "WintunOpenAdapter");
    RESOLVE(WINTUN_CLOSE_ADAPTER_FUNC,              WintunCloseAdapter,            "WintunCloseAdapter");
    RESOLVE(WINTUN_DELETE_DRIVER_FUNC,              WintunDeleteDriver,            "WintunDeleteDriver");
    RESOLVE(WINTUN_GET_ADAPTER_LUID_FUNC,           WintunGetAdapterLUID,          "WintunGetAdapterLUID");
    RESOLVE(WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC, WintunGetRunningDriverVersion, "WintunGetRunningDriverVersion");
    RESOLVE(WINTUN_SET_LOGGER_FUNC,                 WintunSetLogger,               "WintunSetLogger");
    RESOLVE(WINTUN_START_SESSION_FUNC,              WintunStartSession,            "WintunStartSession");
    RESOLVE(WINTUN_END_SESSION_FUNC,                WintunEndSession,              "WintunEndSession");
    RESOLVE(WINTUN_GET_READ_WAIT_EVENT_FUNC,        WintunGetReadWaitEvent,        "WintunGetReadWaitEvent");
    RESOLVE(WINTUN_RECEIVE_PACKET_FUNC,             WintunReceivePacket,           "WintunReceivePacket");
    RESOLVE(WINTUN_RELEASE_RECEIVE_PACKET_FUNC,     WintunReleaseReceivePacket,    "WintunReleaseReceivePacket");
    RESOLVE(WINTUN_ALLOCATE_SEND_PACKET_FUNC,       WintunAllocateSendPacket,      "WintunAllocateSendPacket");
    RESOLVE(WINTUN_SEND_PACKET_FUNC,                WintunSendPacket,              "WintunSendPacket");

    return 0;
}

void wintun_unload(void)
{
    if (g_dll) {
        FreeLibrary(g_dll);
        g_dll = NULL;
    }
}
