/*
 * kernel-wintun router: routes packets between the Wintun TUN adapter,
 * libipsec, and charon's IKE socket. Mirror of kernel_libipsec_router,
 * with the crucial difference that the read loop uses Win32 events
 * (WaitForMultipleObjects) instead of POSIX poll() — Wintun signals via
 * a HANDLE, not via an FD.
 *
 * @defgroup kernel_wintun_router kernel_wintun_router
 * @{ @ingroup kernel_wintun
 */

#ifndef KERNEL_WINTUN_ROUTER_H_
#define KERNEL_WINTUN_ROUTER_H_

#include <kernel/kernel_listener.h>

/* Forward-declared opaque Wintun handles so we don't drag wintun.h into
 * every consumer of this header. The plugin populates a
 * wintun_session_handle_t struct (defined in router.c) and exposes it
 * via lib->set("kernel-wintun-session"). */
typedef struct wintun_session_handle_t wintun_session_handle_t;

typedef struct kernel_wintun_router_t kernel_wintun_router_t;

/**
 * Class that routes packets between the Wintun adapter, libipsec, and
 * the IKE socket.
 */
struct kernel_wintun_router_t {

    /** Implements kernel_listener_t interface. */
    kernel_listener_t listener;

    /**
     * Returns the name of the TUN device used for the given virtual IP.
     * Used indirectly by kernel-iph::add_ip to install the VIP on the
     * right adapter.
     *
     * @param vip   virtual IP, NULL for the default device
     * @return      allocated name (caller frees) or NULL
     */
    char *(*get_tun_name)(kernel_wintun_router_t *this, host_t *vip);

    /** Destructor. */
    void (*destroy)(kernel_wintun_router_t *this);
};

/**
 * Single instance, if created — set by create(), looked up by the
 * kernel-iph patch and by the ipsec implementation to find the TUN.
 */
extern kernel_wintun_router_t *wintun_router;

/**
 * Create the kernel_wintun_router_t instance.
 */
kernel_wintun_router_t *kernel_wintun_router_create(void);

#endif /* KERNEL_WINTUN_ROUTER_H_ */
