/*
 * Wintun DLL loader: loads wintun.dll, resolves all functions, exposes
 * them as global function pointers. Used by both wintun-spike (M2) and
 * the kernel-wintun plugin (M3+).
 *
 * Lifecycle:
 *   wintun_load()    once at startup (returns 0 on success, otherwise GLE)
 *   ... Wintun*()    use as needed
 *   wintun_unload()  at shutdown
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT  0x0A00
#endif
#ifndef NTDDI_VERSION
#  define NTDDI_VERSION 0x0A000000
#endif
/* IMPORTANT: winsock2.h must come before windows.h. Including this header
 * before iphlpapi.h / windows.h ensures the order is correct. */
#include <winsock2.h>
#include <windows.h>

/* strongSwan's utils/compat/windows.h does `#undef CALLBACK` and
 * utils/utils/object.h redefines CALLBACK as a function-style macro.
 * Wintun needs the Win32 calling-convention CALLBACK (= __stdcall).
 * Save whatever is in scope, install the Win32 meaning while we parse
 * wintun.h, then restore — so callers can still use strongSwan's
 * CALLBACK macro afterwards. */
#pragma push_macro("CALLBACK")
#undef CALLBACK
#define CALLBACK __stdcall
#include "wintun.h"
#pragma pop_macro("CALLBACK")

#ifdef __cplusplus
extern "C" {
#endif

extern WINTUN_CREATE_ADAPTER_FUNC             *WintunCreateAdapter;
extern WINTUN_OPEN_ADAPTER_FUNC               *WintunOpenAdapter;
extern WINTUN_CLOSE_ADAPTER_FUNC              *WintunCloseAdapter;
extern WINTUN_DELETE_DRIVER_FUNC              *WintunDeleteDriver;
extern WINTUN_GET_ADAPTER_LUID_FUNC           *WintunGetAdapterLUID;
extern WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC *WintunGetRunningDriverVersion;
extern WINTUN_SET_LOGGER_FUNC                 *WintunSetLogger;
extern WINTUN_START_SESSION_FUNC              *WintunStartSession;
extern WINTUN_END_SESSION_FUNC                *WintunEndSession;
extern WINTUN_GET_READ_WAIT_EVENT_FUNC        *WintunGetReadWaitEvent;
extern WINTUN_RECEIVE_PACKET_FUNC             *WintunReceivePacket;
extern WINTUN_RELEASE_RECEIVE_PACKET_FUNC     *WintunReleaseReceivePacket;
extern WINTUN_ALLOCATE_SEND_PACKET_FUNC       *WintunAllocateSendPacket;
extern WINTUN_SEND_PACKET_FUNC                *WintunSendPacket;

/**
 * Loads wintun.dll and resolves all symbols. Idempotent — repeated calls
 * are OK; only the first one does work.
 *
 * @return 0 on success, otherwise GetLastError() from the failing step
 */
DWORD wintun_load(void);

/**
 * Frees the DLL. After this call, the Wintun* function pointers must
 * no longer be used.
 */
void wintun_unload(void);

#ifdef __cplusplus
}
#endif
