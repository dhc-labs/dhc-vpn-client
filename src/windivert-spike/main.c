/*
 * M7 step 1: WinDivert sniffing spike.
 *
 * Goal: prove that WinDivert at the NETWORK layer sees inbound UDP/4500
 * packets that Windows IPsec.sys silently drops as "Unzulässige
 * SPI-Pakete". If this spike counts ESP-in-UDP packets while the Cisco
 * tunnel is up, the WFP-NETWORK layer is positioned before IPsec.sys's
 * inbound callout — which is the load-bearing assumption for the M7
 * plugin integration.
 *
 * Mode: SNIFF + RECV_ONLY. The kernel still sees the packets normally,
 * we just observe a copy. No risk of breaking the IKE control channel
 * or anything else on the host.
 *
 * Requires: admin / SeLoadDriverPrivilege (WinDivert auto-installs the
 * .sys driver on first WinDivertOpen). WinDivert.dll *and* WinDivert64.sys
 * must sit next to this .exe.
 */

#include "windivert_loader.h"

#include <stdio.h>

static volatile LONG g_stop = 0;
static HANDLE g_handle = INVALID_HANDLE_VALUE;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_stop, 1);
    /* Unblock the pending WinDivertRecv() so the loop can exit. */
    if (g_handle != INVALID_HANDLE_VALUE) {
        WinDivertShutdown_p(g_handle, WINDIVERT_SHUTDOWN_RECV);
    }
    return TRUE;
}

static void format_ipv4(UINT32 net_order, char out[16])
{
    /* Wire bytes: byte0.byte1.byte2.byte3. On little-endian, byte0 is the
     * low octet of the UINT32 read from the struct. */
    UINT8 b0 = (UINT8)(net_order        & 0xFF);
    UINT8 b1 = (UINT8)((net_order >> 8) & 0xFF);
    UINT8 b2 = (UINT8)((net_order >> 16) & 0xFF);
    UINT8 b3 = (UINT8)((net_order >> 24) & 0xFF);
    snprintf(out, 16, "%u.%u.%u.%u", b0, b1, b2, b3);
}

int main(void)
{
    DWORD le = windivert_load();
    if (le != 0) {
        fprintf(stderr,
                "windivert_load failed, GLE=%lu "
                "(126=ERROR_MOD_NOT_FOUND -> WinDivert.dll missing)\n", le);
        return 1;
    }
    fprintf(stderr, "WinDivert.dll loaded, symbols resolved.\n");

    /* Catch both IKE (NAT-T non-ESP marker = 0x00000000) and ESP-in-UDP
     * (SPI != 0) at the same filter — demux by content in the loop. */
    const char *filter = "inbound and ip and udp.DstPort == 4500";

    fprintf(stderr, "==> WinDivertOpen filter=\"%s\" "
                    "layer=NETWORK flags=SNIFF|RECV_ONLY\n", filter);
    g_handle = WinDivertOpen_p(filter, WINDIVERT_LAYER_NETWORK, 0,
                               WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
    if (g_handle == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        fprintf(stderr,
                "FAIL: GLE=%lu  (5=ERROR_ACCESS_DENIED -> run as admin; "
                "577=ERROR_INVALID_IMAGE_HASH -> driver signature blocked; "
                "2=ERROR_FILE_NOT_FOUND -> WinDivert64.sys missing next to .dll; "
                "87=ERROR_INVALID_PARAMETER -> filter syntax)\n", e);
        windivert_unload();
        return 2;
    }

    UINT64 ver_major = 0, ver_minor = 0;
    WinDivertGetParam_p(g_handle, WINDIVERT_PARAM_VERSION_MAJOR, &ver_major);
    WinDivertGetParam_p(g_handle, WINDIVERT_PARAM_VERSION_MINOR, &ver_minor);
    fprintf(stderr, "WinDivert driver version: %llu.%llu\n",
            (unsigned long long)ver_major, (unsigned long long)ver_minor);

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    fprintf(stderr, "\nSniffing inbound UDP/4500. Ctrl+C to stop.\n\n");

    BYTE pkt[0xFFFF];
    WINDIVERT_ADDRESS addr;
    UINT pkt_len = 0;
    DWORD esp_count = 0, ike_count = 0, parse_fail = 0;
    DWORD start = GetTickCount();

    while (!g_stop) {
        if (!WinDivertRecv_p(g_handle, pkt, sizeof(pkt), &pkt_len, &addr)) {
            DWORD e = GetLastError();
            if (e == ERROR_NO_DATA || e == ERROR_INVALID_HANDLE) {
                break; /* shutdown was signalled */
            }
            if (e == ERROR_INSUFFICIENT_BUFFER) {
                continue;
            }
            fprintf(stderr, "WinDivertRecv error: %lu\n", e);
            break;
        }

        PWINDIVERT_IPHDR  ip4 = NULL;
        PWINDIVERT_UDPHDR udp = NULL;
        PVOID payload = NULL;
        UINT  payload_len = 0;
        BOOL ok = WinDivertHelperParsePacket_p(
            pkt, pkt_len,
            &ip4, NULL, NULL, NULL, NULL, NULL, &udp,
            &payload, &payload_len, NULL, NULL);

        if (!ok || !ip4 || !udp || !payload || payload_len < 4) {
            ++parse_fail;
            continue;
        }

        BYTE *p = (BYTE *)payload;
        /* Big-endian assemble: SPI on the wire is the first 4 bytes of the
         * UDP payload for ESP-in-UDP; for IKE NAT-T it is 0x00000000. */
        UINT32 first4 = ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16)
                      | ((UINT32)p[2] << 8)  | ((UINT32)p[3]);

        char src[16], dst[16];
        format_ipv4(ip4->SrcAddr, src);
        format_ipv4(ip4->DstAddr, dst);

        DWORD ts = GetTickCount() - start;
        if (first4 == 0) {
            ++ike_count;
            fprintf(stderr, "[%5lu ms] IKE  %s -> %s  udp_payload=%u B\n",
                    ts, src, dst, payload_len);
        } else {
            ++esp_count;
            fprintf(stderr,
                    "[%5lu ms] ESP  %s -> %s  udp_payload=%u B  SPI=0x%08x\n",
                    ts, src, dst, payload_len, first4);
        }
    }

    fprintf(stderr, "\n==> Stopped. Counts:\n");
    fprintf(stderr, "    ESP-in-UDP/4500 inbound : %lu\n", esp_count);
    fprintf(stderr, "    IKE/NAT-T   inbound     : %lu\n", ike_count);
    fprintf(stderr, "    Parse failures          : %lu\n", parse_fail);

    if (g_handle != INVALID_HANDLE_VALUE) {
        WinDivertClose_p(g_handle);
        g_handle = INVALID_HANDLE_VALUE;
    }
    windivert_unload();

    fprintf(stderr,
            "\nNote: the WinDivert kernel service is left installed. Run\n"
            "  sc stop WinDivert && sc delete WinDivert\n"
            "to fully remove it.\n");
    return 0;
}
