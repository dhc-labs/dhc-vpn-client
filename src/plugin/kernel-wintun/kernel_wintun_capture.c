/*
 * kernel-wintun capture: see header for the why. Implementation mirrors
 * kernel_libipsec_esp_handler.c (Linux raw-socket variant): build a
 * packet_t with src/dst host_t (port 4500 from the UDP header) plus the
 * UDP payload as data, push to ipsec->processor->queue_inbound. From
 * there libipsec decrypts via SA lookup and the existing kernel-wintun
 * router::deliver_plain hands the plaintext IP to wintun.
 *
 * Copyright (C) 2026 Tobias Herbert
 *
 * Licensed under the GPL v2 or later, matching the rest of the plugin.
 */

/* windivert_loader.h FIRST -- establishes winsock2/windows include order
 * before any strongSwan header undefs CALLBACK or pulls in compat headers.
 * (windivert.h itself does not use CALLBACK, but we still want winsock2
 * before windows.h so ntohs() and friends behave.) */
#include "windivert_loader.h"

#include "kernel_wintun_capture.h"

#include <library.h>
#include <ipsec.h>
#include <esp_packet.h>
#include <networking/host.h>
#include <networking/packet.h>
#include <processing/jobs/callback_job.h>
#include <utils/debug.h>

typedef struct private_kernel_wintun_capture_t private_kernel_wintun_capture_t;

struct private_kernel_wintun_capture_t {

    /** Public interface. */
    kernel_wintun_capture_t public;

    /** WinDivert handle, INVALID_HANDLE_VALUE when closed. */
    HANDLE handle;

    /** Worker is exiting; set by cancel_worker / destroy. */
    bool stopping;
};

kernel_wintun_capture_t *wintun_capture = NULL;

/**
 * Filter: only inbound UDP/4500 with a non-zero first 4 bytes (= ESP-in-UDP
 * SPI). IKE NAT-T packets (first 4 bytes = 0x00000000) are NOT matched and
 * flow past the WFP callout untouched, so charon's socket-win plugin keeps
 * receiving IKE on its UDP socket as today.
 */
static const char *FILTER_ESP_IN_UDP =
    "inbound and ip and udp.DstPort == 4500 and "
    "udp.PayloadLength >= 4 and "
    "(udp.Payload[0] != 0 or udp.Payload[1] != 0 or "
    " udp.Payload[2] != 0 or udp.Payload[3] != 0)";

/**
 * Worker job: one WinDivertRecv per call, requeue immediately. The
 * processor's job framework handles thread cancellation via cancel_worker.
 */
static job_requeue_t handle_capture(private_kernel_wintun_capture_t *this)
{
    BYTE pkt[0xFFFF];
    WINDIVERT_ADDRESS addr;
    UINT pkt_len = 0;
    PWINDIVERT_IPHDR  ip4 = NULL;
    PWINDIVERT_UDPHDR udp = NULL;
    PVOID payload = NULL;
    UINT  payload_len = 0;
    host_t *src, *dst;
    chunk_t data;
    packet_t *packet;

    if (this->stopping || this->handle == INVALID_HANDLE_VALUE)
    {
        return JOB_REQUEUE_NONE;
    }

    if (!WinDivertRecv_p(this->handle, pkt, sizeof(pkt), &pkt_len, &addr))
    {
        DWORD e = GetLastError();
        if (this->stopping || e == ERROR_NO_DATA || e == ERROR_INVALID_HANDLE)
        {
            return JOB_REQUEUE_NONE;
        }
        if (e == ERROR_INSUFFICIENT_BUFFER)
        {
            DBG2(DBG_NET, "kernel-wintun-capture: oversized packet, skipping");
            return JOB_REQUEUE_DIRECT;
        }
        DBG1(DBG_NET, "kernel-wintun-capture: WinDivertRecv failed: %lu", e);
        return JOB_REQUEUE_FAIR;
    }

    if (!WinDivertHelperParsePacket_p(pkt, pkt_len,
            &ip4, NULL, NULL, NULL, NULL, NULL, &udp,
            &payload, &payload_len, NULL, NULL))
    {
        DBG1(DBG_NET, "kernel-wintun-capture: parse failed (%u bytes)", pkt_len);
        return JOB_REQUEUE_DIRECT;
    }
    if (!ip4 || !udp || !payload || payload_len < 4)
    {
        return JOB_REQUEUE_DIRECT;
    }

    /* Build host_t with port 4500 (from the UDP header). The port is what
     * tells libipsec / esp_packet that this is NAT-T-encapsulated ESP. */
    src = host_create_from_chunk(AF_INET,
        chunk_create((u_char*)&ip4->SrcAddr, sizeof(ip4->SrcAddr)),
        ntohs(udp->SrcPort));
    dst = host_create_from_chunk(AF_INET,
        chunk_create((u_char*)&ip4->DstAddr, sizeof(ip4->DstAddr)),
        ntohs(udp->DstPort));
    if (!src || !dst)
    {
        DESTROY_IF(src);
        DESTROY_IF(dst);
        return JOB_REQUEUE_DIRECT;
    }

    DBG2(DBG_NET, "kernel-wintun-capture: ESP %#H -> %#H (%u bytes), queueing",
         src, dst, payload_len);

    data = chunk_clone(chunk_create((u_char*)payload, payload_len));
    packet = packet_create();
    packet->set_source(packet, src);
    packet->set_destination(packet, dst);
    packet->set_data(packet, data);
    ipsec->processor->queue_inbound(ipsec->processor,
                                    esp_packet_create_from_packet(packet));
    return JOB_REQUEUE_DIRECT;
}

/**
 * callback_job cancel: SetEvent equivalent. Tells WinDivertRecv to wake
 * up and return ERROR_NO_DATA so the worker bails out. Returning TRUE
 * tells the framework to skip pthread_cancel and just join us -- same
 * pattern as kernel_wintun_router::cancel_worker, for the same reason
 * (winpthreads cancellation does not fire inside WinDivertRecv).
 */
static bool cancel_worker(private_kernel_wintun_capture_t *this)
{
    this->stopping = TRUE;
    if (this->handle != INVALID_HANDLE_VALUE && WinDivertShutdown_p)
    {
        WinDivertShutdown_p(this->handle, WINDIVERT_SHUTDOWN_RECV);
    }
    return TRUE;
}

METHOD(kernel_wintun_capture_t, destroy_, void,
    private_kernel_wintun_capture_t *this)
{
    this->stopping = TRUE;
    if (this->handle != INVALID_HANDLE_VALUE)
    {
        if (WinDivertShutdown_p)
        {
            WinDivertShutdown_p(this->handle, WINDIVERT_SHUTDOWN_BOTH);
        }
        if (WinDivertClose_p)
        {
            WinDivertClose_p(this->handle);
        }
        this->handle = INVALID_HANDLE_VALUE;
    }
    windivert_unload();
    wintun_capture = NULL;
    free(this);
}

kernel_wintun_capture_t *kernel_wintun_capture_create(void)
{
    private_kernel_wintun_capture_t *this;
    DWORD le;

    le = windivert_load();
    if (le != 0)
    {
        DBG1(DBG_KNL, "kernel-wintun-capture: windivert_load failed: GLE=%lu "
                      "(126=ERROR_MOD_NOT_FOUND -> WinDivert.dll missing next "
                      "to charon-svc.exe)", le);
        return NULL;
    }

    INIT(this,
        .public = {
            .destroy = _destroy_,
        },
        .handle = INVALID_HANDLE_VALUE,
    );

    this->handle = WinDivertOpen_p(FILTER_ESP_IN_UDP,
                                   WINDIVERT_LAYER_NETWORK, 0,
                                   WINDIVERT_FLAG_RECV_ONLY);
    if (this->handle == INVALID_HANDLE_VALUE)
    {
        DWORD e = GetLastError();
        DBG1(DBG_KNL, "kernel-wintun-capture: WinDivertOpen failed: GLE=%lu "
                      "(5=ACCESS_DENIED, 2=WinDivert64.sys missing, "
                      "87=filter syntax)", e);
        windivert_unload();
        free(this);
        return NULL;
    }

    DBG1(DBG_KNL, "kernel-wintun-capture: ready -- inbound ESP-in-UDP/4500 "
                  "captured before tcpip.sys demux");

    lib->processor->queue_job(lib->processor,
        (job_t*)callback_job_create((callback_job_cb_t)handle_capture, this,
                                    NULL,
                                    (callback_job_cancel_t)cancel_worker));

    wintun_capture = &this->public;
    return &this->public;
}
