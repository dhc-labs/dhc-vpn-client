#include "windivert_loader.h"

WINDIVERT_OPEN_FN                  WinDivertOpen_p;
WINDIVERT_RECV_FN                  WinDivertRecv_p;
WINDIVERT_SEND_FN                  WinDivertSend_p;
WINDIVERT_SHUTDOWN_FN              WinDivertShutdown_p;
WINDIVERT_CLOSE_FN                 WinDivertClose_p;
WINDIVERT_GET_PARAM_FN             WinDivertGetParam_p;
WINDIVERT_SET_PARAM_FN             WinDivertSetParam_p;
WINDIVERT_HELPER_PARSE_PACKET_FN   WinDivertHelperParsePacket_p;
WINDIVERT_HELPER_FORMAT_IPV4_FN    WinDivertHelperFormatIPv4Address_p;

static HMODULE g_dll = NULL;

#define RESOLVE(typedef_name, var_name, sym)                                 \
    do {                                                                     \
        var_name = (typedef_name)(void(*)(void))GetProcAddress(g_dll, sym);  \
        if (!var_name) {                                                     \
            DWORD e = GetLastError();                                        \
            FreeLibrary(g_dll); g_dll = NULL;                                \
            return e ? e : ERROR_PROC_NOT_FOUND;                             \
        }                                                                    \
    } while (0)

DWORD windivert_load(void)
{
    if (g_dll) {
        return 0;
    }

    /* Search application dir first so the spike's local copy wins over any
     * older system-wide install. */
    g_dll = LoadLibraryExW(
        L"WinDivert.dll", NULL,
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_dll) {
        return GetLastError();
    }

    RESOLVE(WINDIVERT_OPEN_FN,                WinDivertOpen_p,                "WinDivertOpen");
    RESOLVE(WINDIVERT_RECV_FN,                WinDivertRecv_p,                "WinDivertRecv");
    RESOLVE(WINDIVERT_SEND_FN,                WinDivertSend_p,                "WinDivertSend");
    RESOLVE(WINDIVERT_SHUTDOWN_FN,            WinDivertShutdown_p,            "WinDivertShutdown");
    RESOLVE(WINDIVERT_CLOSE_FN,               WinDivertClose_p,               "WinDivertClose");
    RESOLVE(WINDIVERT_GET_PARAM_FN,           WinDivertGetParam_p,            "WinDivertGetParam");
    RESOLVE(WINDIVERT_SET_PARAM_FN,           WinDivertSetParam_p,            "WinDivertSetParam");
    RESOLVE(WINDIVERT_HELPER_PARSE_PACKET_FN, WinDivertHelperParsePacket_p,   "WinDivertHelperParsePacket");
    RESOLVE(WINDIVERT_HELPER_FORMAT_IPV4_FN,  WinDivertHelperFormatIPv4Address_p, "WinDivertHelperFormatIPv4Address");

    return 0;
}

void windivert_unload(void)
{
    if (g_dll) {
        FreeLibrary(g_dll);
        g_dll = NULL;
    }
}
