/*
 * kernel-wintun ipsec interface: implements kernel_ipsec_t for userspace
 * ESP on Windows. Mirror of kernel_libipsec_ipsec_t.
 *
 * @defgroup kernel_wintun_ipsec kernel_wintun_ipsec
 * @{ @ingroup kernel_wintun
 */

#ifndef KERNEL_WINTUN_IPSEC_H_
#define KERNEL_WINTUN_IPSEC_H_

#include <library.h>
#include <kernel/kernel_ipsec.h>

typedef struct kernel_wintun_ipsec_t kernel_wintun_ipsec_t;

/**
 * IPsec interface backed by libipsec + Wintun.
 */
struct kernel_wintun_ipsec_t {

    /**
     * Implements kernel_ipsec_t interface.
     */
    kernel_ipsec_t interface;
};

/**
 * Create the kernel_wintun_ipsec_t instance.
 *
 * @return  instance, or NULL on failure
 */
kernel_wintun_ipsec_t *kernel_wintun_ipsec_create(void);

#endif /* KERNEL_WINTUN_IPSEC_H_ */
