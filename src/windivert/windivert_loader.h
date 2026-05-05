/*
 * WinDivert DLL loader: loads WinDivert.dll, resolves the API entries we
 * need, exposes them as global function pointers. Used by both
 * windivert-spike (M7 step 1) and the kernel-wintun plugin (M7 step 2+).
 *
 * Lifecycle:
 *   windivert_load()    once at startup (returns 0 on success, otherwise GLE)
 *   ... WinDivert*()    use as needed
 *   windivert_unload()  at shutdown
 *
 * WinDivert.dll auto-installs WinDivert64.sys (driver) on the first
 * WinDivertOpen() call when the process holds SeLoadDriverPrivilege.
 * Both files must be co-located next to the executable.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT  0x0A00
#endif
#ifndef NTDDI_VERSION
#  define NTDDI_VERSION 0x0A000000
#endif
#include <winsock2.h>
#include <windows.h>

#include "windivert.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Function-pointer typedefs for the API entries we use. WinDivert's header
 * declares the functions only as `extern __declspec(dllimport)` prototypes,
 * not as typedefs — so we mirror the signatures here. */
typedef HANDLE (WINAPI *WINDIVERT_OPEN_FN)(
    const char *filter, WINDIVERT_LAYER layer, INT16 priority, UINT64 flags);

typedef BOOL (WINAPI *WINDIVERT_RECV_FN)(
    HANDLE handle, VOID *pPacket, UINT packetLen, UINT *pRecvLen,
    WINDIVERT_ADDRESS *pAddr);

typedef BOOL (WINAPI *WINDIVERT_SEND_FN)(
    HANDLE handle, const VOID *pPacket, UINT packetLen, UINT *pSendLen,
    const WINDIVERT_ADDRESS *pAddr);

typedef BOOL (WINAPI *WINDIVERT_SHUTDOWN_FN)(
    HANDLE handle, WINDIVERT_SHUTDOWN how);

typedef BOOL (WINAPI *WINDIVERT_CLOSE_FN)(HANDLE handle);

typedef BOOL (WINAPI *WINDIVERT_GET_PARAM_FN)(
    HANDLE handle, WINDIVERT_PARAM param, UINT64 *pValue);

typedef BOOL (WINAPI *WINDIVERT_SET_PARAM_FN)(
    HANDLE handle, WINDIVERT_PARAM param, UINT64 value);

typedef BOOL (WINAPI *WINDIVERT_HELPER_PARSE_PACKET_FN)(
    const VOID *pPacket, UINT packetLen,
    PWINDIVERT_IPHDR *ppIPHdr, PWINDIVERT_IPV6HDR *ppIPv6Hdr,
    UINT8 *pProtocol,
    PWINDIVERT_ICMPHDR *ppICMPHdr, PWINDIVERT_ICMPV6HDR *ppICMPv6Hdr,
    PWINDIVERT_TCPHDR *ppTCPHdr, PWINDIVERT_UDPHDR *ppUDPHdr,
    PVOID *ppData, UINT *pDataLen,
    PVOID *ppNext, UINT *pNextLen);

typedef BOOL (WINAPI *WINDIVERT_HELPER_FORMAT_IPV4_FN)(
    UINT32 addr, char *buffer, UINT bufLen);

extern WINDIVERT_OPEN_FN                  WinDivertOpen_p;
extern WINDIVERT_RECV_FN                  WinDivertRecv_p;
extern WINDIVERT_SEND_FN                  WinDivertSend_p;
extern WINDIVERT_SHUTDOWN_FN              WinDivertShutdown_p;
extern WINDIVERT_CLOSE_FN                 WinDivertClose_p;
extern WINDIVERT_GET_PARAM_FN             WinDivertGetParam_p;
extern WINDIVERT_SET_PARAM_FN             WinDivertSetParam_p;
extern WINDIVERT_HELPER_PARSE_PACKET_FN   WinDivertHelperParsePacket_p;
extern WINDIVERT_HELPER_FORMAT_IPV4_FN    WinDivertHelperFormatIPv4Address_p;

/**
 * Loads WinDivert.dll and resolves all symbols. Idempotent.
 * @return 0 on success, otherwise GetLastError() from the failing step.
 */
DWORD windivert_load(void);

/**
 * Frees the DLL. After this call, the function pointers must no longer be
 * used. Note: this does NOT remove the WinDivert kernel driver — that
 * sticks around as a service named "WinDivert" until the OS reboots or
 * `sc delete WinDivert` is run.
 */
void windivert_unload(void);

#ifdef __cplusplus
}
#endif
