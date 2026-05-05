/*
 * M2 Wintun spike — the honest end-to-end test that Wintun on this Windows
 * version does what we will need from it in the kernel-wintun plugin:
 *
 *   1. load wintun.dll, resolve symbols (via wintun_loader)
 *   2. create adapter ("dhc-vpn-spike" / TunnelType "dhc-vpn-spike")
 *   3. get adapter LUID, resolve to IfIndex
 *   4. set a virtual IP via the IPHelper API (AddIPAddress)
 *   5. start a session, wait briefly for inbound packets
 *   6. clean teardown — delete adapter, end session, free driver/DLL
 *
 * Requires Admin/elevation (the Wintun driver is rolled out by the service
 * on the first WintunCreateAdapter call).
 */

#include "wintun_loader.h"   /* first — pulls winsock2.h before windows.h */

#include <stdio.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wchar.h>

static const char *level_str(WINTUN_LOGGER_LEVEL lvl) {
    switch (lvl) {
        case WINTUN_LOG_INFO: return "INFO";
        case WINTUN_LOG_WARN: return "WARN";
        case WINTUN_LOG_ERR:  return "ERR ";
        default: return "?   ";
    }
}

static VOID CALLBACK wintun_log(WINTUN_LOGGER_LEVEL lvl, DWORD64 ts, LPCWSTR msg) {
    (void)ts;
    fwprintf(stderr, L"[wintun %hs] %ls\n", level_str(lvl), msg);
}

/* Set IP address 10.99.99.99/24 on the adapter. */
static int set_vip(NET_LUID luid) {
    NET_IFINDEX ifindex = 0;
    NETIO_STATUS s = ConvertInterfaceLuidToIndex(&luid, &ifindex);
    if (s != NO_ERROR) {
        fprintf(stderr, "ConvertInterfaceLuidToIndex: %lu\n", (unsigned long)s);
        return -1;
    }
    fprintf(stderr, "Adapter IfIndex: %lu\n", (unsigned long)ifindex);

    ULONG nte_ctx = 0, nte_inst = 0;
    DWORD r = AddIPAddress(htonl(0x0A636363),  /* 10.99.99.99 */
                           htonl(0xFFFFFF00),  /* 255.255.255.0 */
                           ifindex,
                           &nte_ctx, &nte_inst);
    if (r != NO_ERROR) {
        fprintf(stderr, "AddIPAddress: %lu\n", r);
        return -1;
    }
    fprintf(stderr, "VIP 10.99.99.99/24 installed (NTE-ctx=%lu).\n", nte_ctx);
    return 0;
}

int main(void) {
    DWORD le = wintun_load();
    if (le != 0) {
        fprintf(stderr, "wintun_load failed, GLE=%lu\n", le);
        return 1;
    }
    WintunSetLogger(wintun_log);
    fprintf(stderr, "Wintun DLL loaded, symbols resolved.\n");

    fprintf(stderr, "==> WintunCreateAdapter(\"dhc-vpn-spike\", \"dhc-vpn-spike\", NULL)\n");
    WINTUN_ADAPTER_HANDLE adapter =
        WintunCreateAdapter(L"dhc-vpn-spike", L"dhc-vpn-spike", NULL);
    if (!adapter) {
        fprintf(stderr, "FAIL: GLE=%lu (5=ERROR_ACCESS_DENIED -> need admin?)\n",
                GetLastError());
        wintun_unload();
        return 2;
    }

    DWORD ver = WintunGetRunningDriverVersion();
    fprintf(stderr, "Adapter created. Wintun driver version: %lu.%lu\n",
            (ver >> 16) & 0xFFFFul, ver & 0xFFFFul);

    NET_LUID luid;
    WintunGetAdapterLUID(adapter, &luid);
    fprintf(stderr, "Adapter LUID: 0x%llx\n",
            (unsigned long long)luid.Value);

    if (set_vip(luid) != 0) {
        WintunCloseAdapter(adapter);
        wintun_unload();
        return 3;
    }

    fprintf(stderr, "==> WintunStartSession (2 MiB ring)\n");
    WINTUN_SESSION_HANDLE session = WintunStartSession(adapter, 0x200000);
    if (!session) {
        fprintf(stderr, "FAIL: GLE=%lu\n", GetLastError());
        WintunCloseAdapter(adapter);
        wintun_unload();
        return 4;
    }

    fprintf(stderr, "Session active. Waiting 5 s for inbound packets...\n");
    HANDLE wait = WintunGetReadWaitEvent(session);
    DWORD pkt_count = 0;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 5000) {
        DWORD sz = 0;
        BYTE *pkt = WintunReceivePacket(session, &sz);
        if (pkt) {
            ++pkt_count;
            BYTE first = pkt[0] >> 4;
            fprintf(stderr, "  RX #%lu: %lu bytes, IPv%u\n",
                    pkt_count, sz, first);
            WintunReleaseReceivePacket(session, pkt);
            continue;
        }
        DWORD err = GetLastError();
        if (err == ERROR_NO_MORE_ITEMS) {
            WaitForSingleObject(wait, 500);
            continue;
        }
        fprintf(stderr, "ReceivePacket error: %lu\n", err);
        break;
    }
    fprintf(stderr, "Received %lu packets in 5 s.\n", pkt_count);

    fprintf(stderr, "==> Cleanup\n");
    WintunEndSession(session);
    WintunCloseAdapter(adapter);
    if (!WintunDeleteDriver()) {
        fprintf(stderr, "WintunDeleteDriver: GLE=%lu (likely non-fatal)\n",
                GetLastError());
    }
    wintun_unload();
    fprintf(stderr, "OK.\n");
    return 0;
}
