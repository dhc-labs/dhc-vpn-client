/*
 * kernel-wintun capture: WinDivert-based packet source for inbound
 * ESP-in-UDP/4500. On Windows tcpip.sys silently demuxes UDP/4500 packets
 * with a non-zero first 4 bytes (= ESP SPI) away from userland sockets and
 * into the kernel IPsec engine -- which has no SAs because charon keeps
 * them in libipsec. This component captures those packets at the WFP
 * NETWORK layer (before the demux) and feeds them into the same
 * ipsec->processor->queue_inbound pipeline that charon's receiver_t feeds
 * for IKE.
 *
 * @defgroup kernel_wintun_capture kernel_wintun_capture
 * @{ @ingroup kernel_wintun
 */

#ifndef KERNEL_WINTUN_CAPTURE_H_
#define KERNEL_WINTUN_CAPTURE_H_

typedef struct kernel_wintun_capture_t kernel_wintun_capture_t;

/**
 * Singleton that owns the WinDivert handle and the capture worker thread.
 */
struct kernel_wintun_capture_t {

    /** Destructor. Stops the worker and closes the WinDivert handle. */
    void (*destroy)(kernel_wintun_capture_t *this);
};

/** Single instance, set by create() / NULLed by destroy(). */
extern kernel_wintun_capture_t *wintun_capture;

/**
 * Create the capture singleton. Loads WinDivert.dll, opens a NETWORK-layer
 * handle with an ESP-in-UDP filter, spawns the worker job. Returns NULL on
 * failure (DLL missing, driver install denied, filter rejected).
 */
kernel_wintun_capture_t *kernel_wintun_capture_create(void);

#endif /* KERNEL_WINTUN_CAPTURE_H_ */
