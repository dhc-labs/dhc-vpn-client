/*
 * kernel-wintun: strongSwan plugin that provides CUSTOM:kernel-ipsec on
 * Windows — userspace ESP via libipsec, plain packets through a Wintun TUN
 * adapter.
 *
 * This plugin replaces kernel-wfp in the road-warrior setup. kernel-net is
 * still provided by kernel-iph (with a small patch to add_ip/del_ip that
 * installs the VIP on our Wintun adapter).
 *
 * Mirror template: src/libcharon/plugins/kernel_libipsec/
 *
 * @defgroup kernel_wintun kernel_wintun
 * @ingroup cplugins
 */

#ifndef KERNEL_WINTUN_PLUGIN_H_
#define KERNEL_WINTUN_PLUGIN_H_

#include <library.h>
#include <plugins/plugin.h>

typedef struct kernel_wintun_plugin_t kernel_wintun_plugin_t;

/**
 * Wintun-backed "kernel" interface plugin.
 */
struct kernel_wintun_plugin_t {

    /**
     * Implements plugin_t interface.
     */
    plugin_t plugin;

};

#endif /* KERNEL_WINTUN_PLUGIN_H_ */
