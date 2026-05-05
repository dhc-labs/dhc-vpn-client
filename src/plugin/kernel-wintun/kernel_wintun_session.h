/*
 * Internal struct shared between the plugin (creator) and the router
 * (consumer). Not part of the public API — only included by the plugin's
 * own .c files. Requires wintun_loader.h to be included first so that
 * WINTUN_*_HANDLE etc. are visible.
 */

#ifndef KERNEL_WINTUN_SESSION_H_
#define KERNEL_WINTUN_SESSION_H_

#include "wintun_loader.h"

/**
 * Per-adapter Wintun session bundle. Owned by the plugin, exposed to
 * other strongSwan code via lib->set("kernel-wintun-session", ...).
 */
struct wintun_session_handle_t {
    WINTUN_ADAPTER_HANDLE adapter;
    WINTUN_SESSION_HANDLE session;
    HANDLE                read_event;     /* WintunGetReadWaitEvent result */
    NET_LUID              luid;
    char                 *name;           /* allocated, owned by plugin */
};

#endif /* KERNEL_WINTUN_SESSION_H_ */
