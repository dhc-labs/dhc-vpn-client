# strongSwan Plugin API Reference for `kernel-wintun`

> **Hinweis (2026-05-03):** Dieses Dokument entstand vor M3 als Design-
> Reference während der API-Recherche. Es beschreibt was wir bauen
> *wollten* und welche Optionen damals offen waren. Was tatsächlich
> implementiert wurde — inkl. Korrekturen am ursprünglichen Plan
> (z.B. M4 wurde *nicht* durch eine WFP-vs-Userspace-Entscheidung
> dominiert sondern war de-facto schon in M3 entschieden) —
> beschreibt [`../ARCHITECTURE.md`](../ARCHITECTURE.md).
>
> Diese Datei bleibt als Quellverweis-Karte: jeder Doxygen-Snippet und
> jeder `[path:line]`-Pointer hier ist nach wie vor gültig gegen
> strongSwan 6.0.6 unter `third_party/strongswan/`. Wer das Plugin
> verstehen oder erweitern will, findet hier die kompakteste Karte
> der relevanten strongSwan-Innereien.

---

This document is a working C-API reference for implementing the
`kernel-wintun` plugin in `dhc-vpn-client`. It targets strongSwan **6.0.6**
(the version vendored at `third_party/strongswan/`). All function signatures
quoted here are **verbatim** from the local source tree; line references use
`[path:line]` form so you can jump directly to the source.

The plugin we are writing is a Windows-only equivalent of
`kernel_libipsec` on Linux:

* It exposes a Wintun TUN adapter (Layer-3 virtual NIC) to charon as the
  destination for virtual IPs (the `kernel-net` half).
* It performs ESP encrypt/decrypt in user space via libipsec, sending raw
  ESP through the existing IKE socket or the WinDivert helper, and
  delivers decrypted plaintext to / pulls plaintext from the Wintun
  adapter (the `kernel-ipsec` half).

Existing Windows backends in 6.0.6:

* `kernel-iph` — provides `kernel-net` via IP Helper but stubs out
  `add_ip` / `del_ip` (`return NOT_SUPPORTED;` — see §6).
* `kernel-wfp` — provides `kernel-ipsec` by programming SAs into the
  Windows Filtering Platform; needs an in-kernel IPsec driver and does
  not work for road-warrior virtual IPs.

Reading order for the rest of this file:

1. `kernel_net_t` interface (what we must implement to install a VIP)
2. `kernel_ipsec_t` interface (what we implement for userspace ESP)
3. `tun_device_t` (the abstraction we will wrap around Wintun)
4. Plugin lifecycle / registration boilerplate
5. `kernel_libipsec` end-to-end walkthrough (our blueprint)
6. `kernel-iph` and `kernel-wfp` snapshots and the M4 architecture
   decision

All paths below are relative to `third_party/strongswan/`.

---

## 1. `kernel_net_t` interface

**Header:** `src/libcharon/kernel/kernel_net.h`
**Doxygen blurb [src/libcharon/kernel/kernel_net.h:52-57]:**

> *The kernel network interface handles the communication with the kernel
> for interface and IP address management.*

The interface is a struct of function pointers. Every implementation
populates this struct in its `*_create()` constructor and registers the
constructor through `kernel_net_register()` (see §4).

### 1.1 Address-type bitmask used by enumerators

```c
enum kernel_address_type_t {
    ADDR_TYPE_REGULAR  = (1 << 0),
    ADDR_TYPE_DOWN     = (1 << 1),
    ADDR_TYPE_IGNORED  = (1 << 2),
    ADDR_TYPE_LOOPBACK = (1 << 3),
    ADDR_TYPE_VIRTUAL  = (1 << 4),
    ADDR_TYPE_ALL      = (1 << 5) - 1,
};
```
[src/libcharon/kernel/kernel_net.h:37-50]

### 1.2 Methods (every function pointer in `struct kernel_net_t`)

| # | Method | MUST-HAVE for road-warrior client? |
|---|--------|------------------------------------|
| 1 | `get_features` | optional (return 0) |
| 2 | `get_source_addr` | yes — needed for source selection |
| 3 | `get_nexthop` | yes — needed for route install / IKE |
| 4 | `get_interface` | yes |
| 5 | `create_address_enumerator` | yes (must include the VIP) |
| 6 | `create_local_subnet_enumerator` | nice to have; can return empty enumerator |
| 7 | `add_ip` / `del_ip` | **YES — this is the whole point of the plugin** |
| 8 | `add_route` / `del_route` | yes |
| 9 | `destroy` | yes |

#### `get_features`
```c
kernel_feature_t (*get_features)(kernel_net_t *this);
```
[src/libcharon/kernel/kernel_net.h:65]
ORed `kernel_feature_t` bits the backend supports.
For a Wintun-based net backend we will most likely return `0` — none of
the existing flags (`KERNEL_REQUIRE_EXCLUDE_ROUTE`, `KERNEL_ESP_V3_TFC`,
…) apply on the net side.

#### `get_source_addr`
```c
host_t* (*get_source_addr)(kernel_net_t *this, host_t *dest, host_t *src);
```
[src/libcharon/kernel/kernel_net.h:79]
Routing-table lookup: pick the local source address used to reach
`dest`. Must return an allocated `host_t` (caller destroys), or NULL if
unreachable.

The `kernel_iph` implementation calls Win32 `GetBestInterfaceEx()` and
`GetBestRoute2()`:
```c
res = GetBestInterfaceEx(dest->get_sockaddr(dest), &index);
...
res = GetBestRoute2(0, index, sai_src, sai_dst, 0, &route, &best);
...
return host_create_from_sockaddr((struct sockaddr*)&best);
```
[src/libcharon/plugins/kernel_iph/kernel_iph_net.c:541-567]

For `kernel-wintun` we want the **same** behaviour: the underlying
physical interface is what reaches the IKE peer, so we can either
delegate to IPH (if both plugins are loaded) or duplicate this
~25-line implementation.

#### `get_nexthop`
```c
host_t* (*get_nexthop)(kernel_net_t *this, host_t *dest, int prefix,
                       host_t *src, char **iface);
```
[src/libcharon/kernel/kernel_net.h:98-99]
Like `get_source_addr` but returns the next-hop gateway, optionally also
returning the interface name. `0.0.0.0/::` may be returned when the
route exists but is on-link.

`kernel_iph` again uses `GetBestRoute2()`
[src/libcharon/plugins/kernel_iph/kernel_iph_net.c:569-610].

#### `get_interface`
```c
bool (*get_interface) (kernel_net_t *this, host_t *host, char **name);
```
[src/libcharon/kernel/kernel_net.h:109]
Resolve a local IP back to its interface name (allocated in `*name`).
`kernel_iph` walks its cached `iface_t` list
[src/libcharon/plugins/kernel_iph/kernel_iph_net.c:437-451].

#### `create_address_enumerator`
```c
enumerator_t *(*create_address_enumerator) (kernel_net_t *this,
                                            kernel_address_type_t which);
```
[src/libcharon/kernel/kernel_net.h:121-122]
Enumerator over all local `host_t*`. The cached list is locked while the
enumerator lives — release in the enumerator's `destroy`.
`kernel_iph`'s impl is at [src/libcharon/plugins/kernel_iph/kernel_iph_net.c:515-539].

For `kernel-wintun` we likely return only the addresses *we* installed
on the Wintun adapter (the VIPs), and let `kernel-iph` enumerate
physical addresses.

#### `create_local_subnet_enumerator`
```c
enumerator_t *(*create_local_subnet_enumerator)(kernel_net_t *this);
```
[src/libcharon/kernel/kernel_net.h:133]
Yields `host_t*, uint8_t, char*` tuples (network, prefix, iface) for
every connected subnet. Returning `enumerator_create_empty()` is a
valid no-op. `kernel_iph` does not implement this at all — its method
table simply does not set the slot
([src/libcharon/plugins/kernel_iph/kernel_iph_net.c:752-764]).

#### `add_ip` *(MUST-HAVE)*
```c
status_t (*add_ip) (kernel_net_t *this, host_t *virtual_ip, int prefix,
                    char *iface);
```
[src/libcharon/kernel/kernel_net.h:147-148]
Install a virtual IP on `iface`. Refcounted: multiple `add_ip()` for the
same VIP only install once; equally many `del_ip()` calls remove it.

This is the call that fails on Windows today. `kernel_iph` simply
returns `NOT_SUPPORTED`:
```c
METHOD(kernel_net_t, add_ip, status_t,
    private_kernel_iph_net_t *this, host_t *virtual_ip, int prefix,
    char *iface_name)
{
    return NOT_SUPPORTED;
}
```
[src/libcharon/plugins/kernel_iph/kernel_iph_net.c:612-617]

The Linux `kernel_netlink` implementation is much fatter — it has a
`vips` hashtable, refcounting, condvar synchronisation around the
async kernel ack, and respects an `install_virtual_ip_on` config option
to force the VIP onto a specific interface
[src/libcharon/plugins/kernel_netlink/kernel_netlink_net.c:2357 onward].

For `kernel-wintun` we will:
1. Create the Wintun adapter (or look it up if already created).
2. Tell Wintun to set the address `virtual_ip/prefix`.
3. Track refcount in our own `linked_list_t` of `vip_entry_t`.
4. Set `install_virtual_ip_on = "<our wintun ifname>"` from the plugin
   constructor (libipsec does the same — see §5).

#### `del_ip` *(MUST-HAVE)*
```c
status_t (*del_ip) (kernel_net_t *this, host_t *virtual_ip, int prefix,
                    bool wait);
```
[src/libcharon/kernel/kernel_net.h:160-161]
Stub in `kernel_iph` [src/libcharon/plugins/kernel_iph/kernel_iph_net.c:619-624].
`wait=TRUE` means the call must block until the kernel confirms removal
(needed on Linux for the duplicate-address race; on Wintun we can drop
the address synchronously, so this is mostly trivial).

#### `add_route`
```c
status_t (*add_route) (kernel_net_t *this, chunk_t dst_net,
                       uint8_t prefixlen, host_t *gateway, host_t *src_ip,
                       char *if_name, bool pass);
```
[src/libcharon/kernel/kernel_net.h:175-177]
Install a route in the OS routing table. `pass=TRUE` for a passthrough
(exclude) route around the IKE peer.

`kernel_iph` already implements this with `CreateIpForwardEntry2()`
[src/libcharon/plugins/kernel_iph/kernel_iph_net.c:629-715, 717-722].
For `kernel-wintun` we may either reuse that implementation or call
`MIB_IPFORWARD_ROW2` ourselves so the route is bound to the Wintun
adapter's `InterfaceIndex`.

`kernel_libipsec` cares about the return value `ALREADY_DONE`:
```c
case ALREADY_DONE:
    /* route exists, do not uninstall */
    remove_exclude_route(this, route);
    route_entry_destroy(route);
    ...
case SUCCESS:
    /* cache the installed route */
    policy->route = route;
    ...
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_ipsec.c:516-526]

#### `del_route`
```c
status_t (*del_route) (kernel_net_t *this, chunk_t dst_net,
                       uint8_t prefixlen, host_t *gateway, host_t *src_ip,
                       char *if_name, bool pass);
```
[src/libcharon/kernel/kernel_net.h:190-192]
Symmetric. `kernel_iph` shares `manage_route()` with `add_route`
[src/libcharon/plugins/kernel_iph/kernel_iph_net.c:724-729].

#### `destroy`
```c
void (*destroy) (kernel_net_t *this);
```
[src/libcharon/kernel/kernel_net.h:197]
Free everything. `kernel_iph` cleans up its `NotifyIpInterfaceChange`
handle, the `EnableRouter` event handle, mutex, and the iface list
[src/libcharon/plugins/kernel_iph/kernel_iph_net.c:731-742].

### 1.3 Constructor and registration helper

Each plugin defines a typedef'd subclass that embeds `kernel_net_t`:
```c
struct kernel_iph_net_t {
    kernel_net_t interface;
};
kernel_iph_net_t *kernel_iph_net_create();
```
[src/libcharon/plugins/kernel_iph/kernel_iph_net.h:32-45]

The constructor wires up the function pointers via the
`METHOD()`/`INIT()` macros:
```c
INIT(this,
    .public = {
        .interface = {
            .get_interface = _get_interface_name,
            .create_address_enumerator = _create_address_enumerator,
            .get_source_addr = _get_source_addr,
            .get_nexthop = _get_nexthop,
            .add_ip = _add_ip,
            .del_ip = _del_ip,
            .add_route = _add_route,
            .del_route = _del_route,
            .destroy = _destroy,
        },
    },
    ...
);
```
[src/libcharon/plugins/kernel_iph/kernel_iph_net.c:752-771]

`kernel_net_register()` is a `plugin_feature_callback_t` that hands the
constructor to charon's `kernel_interface_t` so it becomes the active
backend:
```c
bool kernel_net_register(plugin_t *plugin, plugin_feature_t *feature,
                         bool reg, void *data);
```
[src/libcharon/kernel/kernel_net.h:211-212]

Constructor type:
```c
typedef kernel_net_t* (*kernel_net_constructor_t)(void);
```
[src/libcharon/kernel/kernel_interface.h:96]

---

## 2. `kernel_ipsec_t` interface

**Header:** `src/libcharon/kernel/kernel_ipsec.h`
**Doxygen blurb [src/libcharon/kernel/kernel_ipsec.h:212-221]:**

> *The kernel ipsec interface handles the communication with the kernel
> for SA and policy management. … Policy information are cached in the
> interface. This is necessary to do reference counting. The Linux
> kernel does not allow the same policy installed twice, but we need
> this as CHILD_SA exist multiple times when rekeying.*

### 2.1 Helper structs (verbatim from header)

`kernel_ipsec_sa_id_t` identifies an SA (src, dst, spi, proto, mark,
if_id) [src/libcharon/kernel/kernel_ipsec.h:48-62].

`kernel_ipsec_add_sa_t` carries everything needed to install one SA:
reqid, mode, traffic selectors, lifetime, enc/int alg+key, replay
window, TFC pad, IPComp params, ESN, encap flag, plus
`initiator/inbound/update` flags
[src/libcharon/kernel/kernel_ipsec.h:67-126].

Other structs (`update_sa_t`, `query_sa_t`, `del_sa_t`, `policy_id_t`,
`manage_policy_t`, `query_policy_t`) are at lines 128–209 of the same
file.

### 2.2 Methods (every function pointer in `struct kernel_ipsec_t`)

| # | Method | MUST-HAVE for userspace ESP? |
|---|--------|------------------------------|
| 1 | `get_features` | yes (must declare `KERNEL_ESP_V3_TFC` etc. correctly) |
| 2 | `get_spi` | yes |
| 3 | `get_cpi` | optional (return `NOT_SUPPORTED` unless we do IPComp) |
| 4 | `add_sa` | **yes — installs SA into libipsec SAD** |
| 5 | `update_sa` | optional (libipsec returns `NOT_SUPPORTED`) |
| 6 | `query_sa` | nice to have (byte/packet counters) |
| 7 | `del_sa` | yes |
| 8 | `flush_sas` | yes |
| 9 | `add_policy` | yes — *and* installs the route via TUN |
| 10 | `query_policy` | optional |
| 11 | `del_policy` | yes |
| 12 | `flush_policies` | yes |
| 13 | `bypass_socket` | optional (libipsec returns `NOT_SUPPORTED`, uses exclude routes instead) |
| 14 | `enable_udp_decap` | optional (`NOT_SUPPORTED` for libipsec) |
| 15 | `destroy` | yes |

#### `get_features`
```c
kernel_feature_t (*get_features)(kernel_ipsec_t *this);
```
[src/libcharon/kernel/kernel_ipsec.h:229]
libipsec returns:
```c
return KERNEL_ESP_V3_TFC | KERNEL_SA_USE_TIME |
        (this->require_encap ? KERNEL_REQUIRE_UDP_ENCAPSULATION : 0);
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_ipsec.c:248-253]

For Wintun we will return the same set; `require_encap` is `TRUE` unless
we ship a raw-ESP handler (a WinDivert-based one, for instance).

#### `get_spi`
```c
status_t (*get_spi)(kernel_ipsec_t *this, host_t *src, host_t *dst,
                    uint8_t protocol, uint32_t *spi);
```
[src/libcharon/kernel/kernel_ipsec.h:240-241]
libipsec delegates to its global SAD:
```c
return ipsec->sas->get_spi(ipsec->sas, src, dst, protocol, spi);
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_ipsec.c:255-260]

`kernel_wfp` keeps an internal counter and returns a random-ish SPI in
the `KERNEL_SPI_MIN..MAX` range
([src/libcharon/kernel/kernel_interface.h:62-63]).

#### `get_cpi`
```c
status_t (*get_cpi)(kernel_ipsec_t *this, host_t *src, host_t *dst,
                    uint16_t *cpi);
```
[src/libcharon/kernel/kernel_ipsec.h:251-252]
libipsec → `NOT_SUPPORTED`
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_ipsec.c:262-267].
We will do the same.

#### `add_sa`
```c
status_t (*add_sa)(kernel_ipsec_t *this, kernel_ipsec_sa_id_t *id,
                   kernel_ipsec_add_sa_t *data);
```
[src/libcharon/kernel/kernel_ipsec.h:264-265]
libipsec passes everything straight through to `ipsec->sas->add_sa()`
after rejecting non-encapsulated SAs when `require_encap` is set:
```c
if (this->require_encap && !data->encap)
{
    DBG1(DBG_ESP, "failed to add SAD entry: only UDP encapsulation is "
         "supported");
    return FAILED;
}
return ipsec->sas->add_sa(ipsec->sas, id->src, id->dst, id->spi, id->proto,
                data->reqid, id->mark, data->tfc, data->lifetime,
                data->enc_alg, data->enc_key, data->int_alg, data->int_key,
                data->mode, data->ipcomp, data->cpi, data->initiator,
                data->encap, data->esn, data->inbound, data->update);
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_ipsec.c:269-284]

`kernel_wfp` does it differently: it stashes an `entry_t` in a temporary
hashtable on the inbound `add_sa`, then completes the pair on the
outbound `add_sa` and submits both directions to WFP at once
[src/libcharon/plugins/kernel_wfp/kernel_wfp_ipsec.c:2110-2199].
That two-phase pattern is specific to WFP and we don't need it.

#### `update_sa` *(optional)*
```c
status_t (*update_sa)(kernel_ipsec_t *this, kernel_ipsec_sa_id_t *id,
                      kernel_ipsec_update_sa_t *data);
```
[src/libcharon/kernel/kernel_ipsec.h:280-281]
libipsec returns `NOT_SUPPORTED`
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_ipsec.c:286-291] —
on rekey, charon will simply install the new SA and delete the old one.
We can do the same.

#### `query_sa`
```c
status_t (*query_sa)(kernel_ipsec_t *this, kernel_ipsec_sa_id_t *id,
                     kernel_ipsec_query_sa_t *data, uint64_t *bytes,
                     uint64_t *packets, time_t *time);
```
[src/libcharon/kernel/kernel_ipsec.h:297-299]
libipsec delegates to the SAD
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_ipsec.c:293-300].

#### `del_sa`, `flush_sas`
```c
status_t (*del_sa)(kernel_ipsec_t *this, kernel_ipsec_sa_id_t *id,
                   kernel_ipsec_del_sa_t *data);
status_t (*flush_sas)(kernel_ipsec_t *this);
```
[src/libcharon/kernel/kernel_ipsec.h:308-309, 316]
Both delegate to the SAD in libipsec
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_ipsec.c:302-314].

#### `add_policy`
```c
status_t (*add_policy)(kernel_ipsec_t *this,
                       kernel_ipsec_policy_id_t *id,
                       kernel_ipsec_manage_policy_t *data);
```
[src/libcharon/kernel/kernel_ipsec.h:325-327]
This is the **non-trivial one**. libipsec does three things here:

1. Install the policy into the userspace SPD
   (`ipsec->policies->add_policy(...)`).
2. Track the policy locally in a `linked_list_t` (with refcounting) so
   it can install / uninstall the *route* in step 3.
3. For outbound policies, call `install_route()` which:
   * Looks up the source IP via `charon->kernel->get_address_by_ts()`.
   * Asks the router for the right TUN device name
     (`router->get_tun_name(router, vip)`).
   * Installs the route with `charon->kernel->add_route(...)`.
   * Optionally adds an exclude route around the IKE peer if the
     remote TS would otherwise eat it.

The full flow is at
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_ipsec.c:537-574]
with `install_route()` at lines 405–535.

Important: `add_policy` ends up *recursively* calling
`charon->kernel->add_route(...)`, which goes back into the **net**
backend (the one we register). On Windows that means we either need to
make sure `kernel-iph`'s `add_route` accepts the Wintun ifname, or we
provide our own.

#### `query_policy`, `del_policy`, `flush_policies`
Quoted at [src/libcharon/kernel/kernel_ipsec.h:340-361] and implemented
in libipsec at lines 576–658 of `kernel_libipsec_ipsec.c`. `del_policy`
mirrors `add_policy` and removes the route on last-ref.

#### `bypass_socket`
```c
bool (*bypass_socket)(kernel_ipsec_t *this, int fd, int family);
```
[src/libcharon/kernel/kernel_ipsec.h:370]
libipsec relies on exclude routes instead and returns `NOT_SUPPORTED`
([src/libcharon/plugins/kernel_libipsec/kernel_libipsec_ipsec.c:660-665]).

#### `enable_udp_decap`
```c
bool (*enable_udp_decap)(kernel_ipsec_t *this, int fd, int family,
                         uint16_t port);
```
[src/libcharon/kernel/kernel_ipsec.h:380-381]
libipsec → `NOT_SUPPORTED` (decap is done in userspace)
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_ipsec.c:667-671].

#### `destroy`
```c
void (*destroy)(kernel_ipsec_t *this);
```
[src/libcharon/kernel/kernel_ipsec.h:386]

### 2.3 Registration helper

```c
bool kernel_ipsec_register(plugin_t *plugin, plugin_feature_t *feature,
                           bool reg, void *data);
```
[src/libcharon/kernel/kernel_ipsec.h:400-401]

Constructor type:
```c
typedef kernel_ipsec_t* (*kernel_ipsec_constructor_t)(void);
```
[src/libcharon/kernel/kernel_interface.h:91]

---

## 3. `tun_device_t` abstraction

**Header:** `src/libstrongswan/networking/tun_device.h`

The header opens with a one-liner [src/libstrongswan/networking/tun_device.h:31-35]:

> *Class to create TUN devices. Creating such a device requires the
> CAP_NET_ADMIN capability.*

### 3.1 Methods (verbatim)

```c
bool   (*read_packet)(tun_device_t *this, chunk_t *packet);
bool   (*write_packet)(tun_device_t *this, chunk_t packet);
bool   (*set_address)(tun_device_t *this, host_t *addr, uint8_t netmask);
host_t* (*get_address)(tun_device_t *this, uint8_t *netmask);
bool   (*up)(tun_device_t *this);
bool   (*set_mtu)(tun_device_t *this, int mtu);
int    (*get_mtu)(tun_device_t *this);
char  *(*get_name)(tun_device_t *this);
int    (*get_fd)(tun_device_t *this);
void   (*destroy)(tun_device_t *this);
```
[src/libstrongswan/networking/tun_device.h:47-113]

Constructor:
```c
tun_device_t *tun_device_create(const char *name_tmpl);
```
[src/libstrongswan/networking/tun_device.h:123]

`name_tmpl` defaults to `"tun%d"`. The Linux/macOS implementation lives
in `src/libstrongswan/networking/tun_device.c` (685 lines). On Windows
the file currently compiles to a `TUN_DEVICE_NOT_SUPPORTED` stub:
```c
#if defined(__APPLE__)
... TARGET_OS_OSX ...
#elif !defined(__linux__) && !defined(HAVE_NET_IF_TUN_H)
#define TUN_DEVICE_NOT_SUPPORTED
#endif
```
[src/libstrongswan/networking/tun_device.c:25-32]

```c
#ifdef TUN_DEVICE_NOT_SUPPORTED
tun_device_t *tun_device_create(const char *name_tmpl)
{
    DBG1(DBG_LIB, "TUN devices are not supported");
    return NULL;
}
```
[src/libstrongswan/networking/tun_device.c:34-40]

### 3.2 How libipsec uses `tun_device_t`

The plugin constructor creates the TUN, sets MTU, brings it up, and
publishes it through the `lib->set()` global registry so the router can
fetch it later:
```c
this->tun = tun_device_create("ipsec%d");
...
if (!this->tun->set_mtu(this->tun, TUN_DEFAULT_MTU) ||
    !this->tun->up(this->tun))
{
    DBG1(DBG_KNL, "failed to configure TUN device");
    destroy(this);
    return NULL;
}
lib->set(lib, "kernel-libipsec-tun", this->tun);

/* set TUN device as default to install VIPs */
lib->settings->set_str(lib->settings, "%s.install_virtual_ip_on",
                       this->tun->get_name(this->tun), lib->ns);
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_plugin.c:141-159]

Note the last call: setting `<ns>.install_virtual_ip_on` is how
libipsec convinces `kernel_netlink_net` to put the VIP onto the TUN.
We will use the **exact same trick** to make sure `kernel-iph`
(or whoever provides `kernel-net`) installs the VIP on Wintun.

The router consumes it on the read side:
```c
static void process_plain(tun_device_t *tun)
{
    chunk_t raw;

    if (tun->read_packet(tun, &raw))
    {
        ip_packet_t *packet;

        packet = ip_packet_create(raw);
        if (packet)
        {
            ipsec->processor->queue_outbound(ipsec->processor, packet);
        }
        ...
    }
}
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_router.c:146-164]

… and on the write side via the inbound delivery callback:
```c
CALLBACK(deliver_plain, void,
    private_kernel_libipsec_router_t *this, ip_packet_t *packet)
{
    tun_device_t *tun;
    tun_entry_t *entry, lookup = {
        .addr = packet->get_destination(packet),
    };
    this->lock->read_lock(this->lock);
    entry = this->tuns->get(this->tuns, &lookup);
    tun = entry ? entry->tun : this->tun.tun;
    tun->write_packet(tun, packet->get_encoding(packet));
    ...
}
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_router.c:127-141]

### 3.3 What we need for Wintun

The router code uses `get_fd()` together with `poll()`:
```c
pfd[count].fd = this->tun.fd;
pfd[count].events = POLLIN;
...
poll(pfd, count, -1)
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_router.c:201-216]

Wintun does **not** expose a file descriptor — it uses an `OVERLAPPED`
event handle. So we have two options:

* **A.** Keep the existing router code and back the FD with a Win32
  socketpair / pipe whose readable end signals when the Wintun read
  thread has queued a packet.
* **B.** Rewrite the router to use `WaitForMultipleObjects`. This
  diverges from libipsec but is the "natural" Windows shape.

This is one of the bigger M3 decisions. See §6 below.

The other methods are easy to map:

| `tun_device_t` method | Wintun equivalent |
|---|---|
| `read_packet` | `WintunReceivePacket` (blocking, with event) |
| `write_packet` | `WintunAllocateSendPacket` + `WintunSendPacket` |
| `set_address` | `CreateUnicastIpAddressEntry` (we keep IPH for this) |
| `get_address` | cached locally |
| `up` | `WintunStartSession` |
| `set_mtu` / `get_mtu` | `WintunGetAdapter*` + `MIB_IPINTERFACE_ROW` |
| `get_name` / `get_fd` | cached / event-handle-as-int |
| `destroy` | `WintunEndSession` + `WintunCloseAdapter` |

---

## 4. Plugin lifecycle / boilerplate

### 4.1 The `plugin_t` struct
```c
struct plugin_t {

    char* (*get_name)(plugin_t *this);

    int   (*get_features)(plugin_t *this, plugin_feature_t *features[]);

    bool  (*reload)(plugin_t *this);

    void  (*destroy)(plugin_t *this);
};
```
[src/libstrongswan/plugins/plugin.h:34-65]

### 4.2 Constructor typedef + `PLUGIN_DEFINE` macro
```c
typedef plugin_t *(*plugin_constructor_t)(void);

#define PLUGIN_DEFINE(name) \
    const char *name##_plugin_version = VERSION; \
    plugin_t *name##_plugin_create()
```
[src/libstrongswan/plugins/plugin.h:76-83]

So `PLUGIN_DEFINE(kernel_wintun)` expands to a string symbol
`kernel_wintun_plugin_version` plus a function
`plugin_t *kernel_wintun_plugin_create()` that the loader will call.

### 4.3 The feature registration macros

The full macro family is documented inline at
[src/libstrongswan/plugins/plugin_feature.h:64-83]:

> ```
> // two features, one with two dependencies, both use a callback to register
> PLUGIN_CALLBACK(...),
>     PLUGIN_PROVIDE(...),
>         PLUGIN_DEPENDS(...),
>         PLUGIN_SDEPEND(...),
>     PLUGIN_PROVIDE(...),
> // common constructor to register for a feature with one dependency
> PLUGIN_REGISTER(...),
>     PLUGIN_PROVIDE(...),
>         PLUGIN_DEPENDS(...),
> // feature that does not use a registration function
> PLUGIN_NOOP,
>     PLUGIN_PROVIDE(...),
> ```

The macros themselves:
```c
#define PLUGIN_REGISTER(type, f, ...)  ...
#define PLUGIN_CALLBACK(cb, data)      ...
#define PLUGIN_NOOP                    _PLUGIN_FEATURE_CALLBACK(NULL, NULL)
#define PLUGIN_PROVIDE(type, ...)      _PLUGIN_FEATURE_##type(PROVIDE, __VA_ARGS__)
#define PLUGIN_DEPENDS(type, ...)      _PLUGIN_FEATURE_##type(DEPENDS, __VA_ARGS__)
#define PLUGIN_SDEPEND(type, ...)      _PLUGIN_FEATURE_##type(SDEPEND, __VA_ARGS__)
```
[src/libstrongswan/plugins/plugin_feature.h:248-285]

`plugin_feature_callback_t` is the signature every callback must match:
```c
typedef bool (*plugin_feature_callback_t)(plugin_t *plugin,
                                          plugin_feature_t *feature,
                                          bool reg, void *cb_data);
```
[src/libstrongswan/plugins/plugin_feature.h:43-45]

For our purposes the relevant FEATURE_* enum values are
`FEATURE_CUSTOM` (custom string-named feature, used for
`"kernel-net"` / `"kernel-ipsec"` / our private
`"kernel-wintun-router"`) [src/libstrongswan/plugins/plugin_feature.h:160-162].

### 4.4 Worked example: `kernel_libipsec_plugin_create`

Quoted in full so we can pattern-match against it.

```c
METHOD(plugin_t, get_name, char*,
    private_kernel_libipsec_plugin_t *this)
{
    return "kernel-libipsec";
}

/**
 * Create the kernel_libipsec_router_t instance
 */
static bool create_router(private_kernel_libipsec_plugin_t *this,
                          plugin_feature_t *feature, bool reg, void *arg)
{
    if (reg)
    {   /* registers as packet handler etc. */
        this->router = kernel_libipsec_router_create();
    }
    else
    {
        DESTROY_IF(this->router);
    }
    return TRUE;
}

METHOD(plugin_t, get_features, int,
    private_kernel_libipsec_plugin_t *this, plugin_feature_t *features[])
{
    static plugin_feature_t f[] = {
        PLUGIN_CALLBACK(kernel_ipsec_register, kernel_libipsec_ipsec_create),
            PLUGIN_PROVIDE(CUSTOM, "kernel-ipsec"),
        PLUGIN_CALLBACK((plugin_feature_callback_t)create_router, NULL),
            PLUGIN_PROVIDE(CUSTOM, "kernel-libipsec-router"),
                PLUGIN_DEPENDS(CUSTOM, "libcharon-receiver"),
    };
    *features = f;
    return countof(f);
}
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_plugin.c:56-91]

Notice three things we will copy verbatim:

* `PLUGIN_CALLBACK(kernel_ipsec_register, kernel_libipsec_ipsec_create)`
  — the registrar from §2.3 plus our constructor.
* `PLUGIN_PROVIDE(CUSTOM, "kernel-ipsec")` — we *advertise* that we
  satisfy the abstract `kernel-ipsec` feature so charon's
  `kernel_interface.c` will pick us up.
* A second `PLUGIN_CALLBACK(create_router, NULL)` for our private
  router, with a hard `PLUGIN_DEPENDS(CUSTOM, "libcharon-receiver")` so
  charon's IKE socket receiver is up before we hook into it.

The full constructor:
```c
PLUGIN_DEFINE(kernel_libipsec)
{
    private_kernel_libipsec_plugin_t *this;

    if (!lib->caps->check(lib->caps, CAP_NET_ADMIN))
    {   /* required to create TUN devices */
        DBG1(DBG_KNL, "kernel-libipsec plugin requires CAP_NET_ADMIN "
             "capability");
        return NULL;
    }

    INIT(this,
        .public = {
            .plugin = {
                .get_name = _get_name,
                .get_features = _get_features,
                .destroy = _destroy,
            },
        },
    );

    if (!libipsec_init())
    {
        DBG1(DBG_LIB, "initialization of libipsec failed");
        destroy(this);
        return NULL;
    }

    this->tun = tun_device_create("ipsec%d");
    ...
    lib->set(lib, "kernel-libipsec-tun", this->tun);

    /* set TUN device as default to install VIPs */
    lib->settings->set_str(lib->settings, "%s.install_virtual_ip_on",
                           this->tun->get_name(this->tun), lib->ns);
    ...
    return &this->public.plugin;
}
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_plugin.c:113-173]

The CAP_NET_ADMIN check is meaningless on Windows; we'll either drop it
or replace it with a Wintun-driver-presence check.

### 4.5 Skeleton for `kernel_wintun_plugin.c`

```c
PLUGIN_DEFINE(kernel_wintun)
{
    private_kernel_wintun_plugin_t *this;

    INIT(this,
        .public.plugin = {
            .get_name     = _get_name,
            .get_features = _get_features,
            .destroy      = _destroy,
        },
    );

    if (!libipsec_init())               return destroy(this), NULL;
    this->tun = wintun_device_create("dhc-vpn%d");
    if (!this->tun)                      return destroy(this), NULL;
    this->tun->set_mtu(this->tun, 1400);
    this->tun->up(this->tun);
    lib->set(lib, "kernel-libipsec-tun", this->tun);    /* router looks for this name */
    lib->settings->set_str(lib->settings, "%s.install_virtual_ip_on",
                           this->tun->get_name(this->tun), lib->ns);
    return &this->public.plugin;
}

METHOD(plugin_t, get_features, int,
    private_kernel_wintun_plugin_t *this, plugin_feature_t *features[])
{
    static plugin_feature_t f[] = {
        PLUGIN_CALLBACK(kernel_ipsec_register, kernel_wintun_ipsec_create),
            PLUGIN_PROVIDE(CUSTOM, "kernel-ipsec"),
        PLUGIN_CALLBACK((plugin_feature_callback_t)create_router, NULL),
            PLUGIN_PROVIDE(CUSTOM, "kernel-wintun-router"),
                PLUGIN_DEPENDS(CUSTOM, "libcharon-receiver"),
    };
    *features = f;
    return countof(f);
}
```

Key choice we still need to make: **do we provide `"kernel-net"` too?**
See §6.

---

## 5. `kernel_libipsec` end-to-end walkthrough

The Linux `kernel_libipsec` plugin is the closest existing analog to
`kernel-wintun`. Its layout:

```
src/libcharon/plugins/kernel_libipsec/
    kernel_libipsec_plugin.c       <-- bootstraps everything
    kernel_libipsec_ipsec.c        <-- implements kernel_ipsec_t (§2)
    kernel_libipsec_router.c       <-- shuffles packets between TUN, libipsec, IKE socket
    kernel_libipsec_esp_handler.c  <-- (optional) raw-ESP path for non-NAT mode
```

### 5.1 The router

The router is the heart of the data plane. It is a `kernel_listener_t`
so it learns about TUN devices that other parts of charon create
(through the `tun()` listener hook), but its main job is to shuttle
packets in three directions:

```
                     ┌─────────────┐
              raw    │   charon    │  raw
   IKE socket ─────►│   receiver  ├─────►   libipsec   inbound queue
                     └─────────────┘            │
                                                ▼
                     ┌─────────────┐  decrypted plaintext
   Wintun ◄──────────│   router    │◄────────  libipsec processor
   adapter   plain   │  (deliver_  │
                     │   plain)    │
                     └──────┬──────┘
                            │ poll() fires on tun.fd
                            ▼
                     ┌─────────────┐  plaintext
   Wintun ──────────►│ process_    ├─────►   libipsec   outbound queue
   adapter   plain   │ plain       │
                     └─────────────┘
                            │
                     ┌──────▼──────┐
                     │  send_esp   │  encrypted ESP packet
                     │  (callback) ├─────►   IKE socket (or raw ESP handler)
                     └─────────────┘
```

The wiring lives in `kernel_libipsec_router_create()`:
```c
charon->kernel->add_listener(charon->kernel, &this->public.listener);
ipsec->processor->register_outbound(ipsec->processor, send_esp, this);
ipsec->processor->register_inbound(ipsec->processor, deliver_plain, this);
charon->receiver->add_esp_cb(charon->receiver, receiver_esp_cb, NULL);
lib->processor->queue_job(lib->processor,
        (job_t*)callback_job_create((callback_job_cb_t)handle_plain, this,
                                    NULL, callback_job_cancel_thread));
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_router.c:361-367]

### 5.2 The four key callbacks

#### `receiver_esp_cb` — IKE socket → libipsec inbound
```c
CALLBACK(receiver_esp_cb, void,
    void *data, packet_t *packet)
{
    ipsec->processor->queue_inbound(ipsec->processor,
                                    esp_packet_create_from_packet(packet));
}
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_router.c:120-125]

charon's UDP receiver hands us every ESP-in-UDP packet it sees; we wrap
it in an `esp_packet_t` and queue it for libipsec to decrypt.

#### `deliver_plain` — libipsec inbound → TUN
```c
CALLBACK(deliver_plain, void,
    private_kernel_libipsec_router_t *this, ip_packet_t *packet)
{
    tun_device_t *tun;
    tun_entry_t *entry, lookup = { .addr = packet->get_destination(packet) };
    this->lock->read_lock(this->lock);
    entry = this->tuns->get(this->tuns, &lookup);
    tun = entry ? entry->tun : this->tun.tun;
    tun->write_packet(tun, packet->get_encoding(packet));
    this->lock->unlock(this->lock);
    packet->destroy(packet);
}
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_router.c:127-141]

After libipsec decrypts, the plaintext IP packet lands here. We pick
the right TUN by destination VIP and write it. The `tuns` hashtable
indexes by VIP — see §5.3.

#### `process_plain` / `handle_plain` — TUN → libipsec outbound

`handle_plain()` is a long-lived `callback_job` that polls every TUN's
FD plus a notify pipe:
```c
pfd[count].fd = this->notify[0]; pfd[count].events = POLLIN; count++;
pfd[count].fd = this->tun.fd;    pfd[count].events = POLLIN; count++;
enumerator = this->tuns->create_enumerator(this->tuns);
while (enumerator->enumerate(enumerator, NULL, &entry))
{
    pfd[count].fd = entry->fd;
    pfd[count].events = POLLIN;
    count++;
}
...
oldstate = thread_cancelability(TRUE);
if (poll(pfd, count, -1) <= 0) {...}
thread_cancelability(oldstate);
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_router.c:186-221]

When data is ready it calls `process_plain(tun)`:
```c
static void process_plain(tun_device_t *tun)
{
    chunk_t raw;
    if (tun->read_packet(tun, &raw))
    {
        ip_packet_t *packet;
        packet = ip_packet_create(raw);
        if (packet)
            ipsec->processor->queue_outbound(ipsec->processor, packet);
        else
            DBG1(DBG_KNL, "invalid IP packet read from TUN device");
    }
}
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_router.c:146-164]

#### `send_esp` — libipsec outbound → IKE socket / raw-ESP
```c
CALLBACK(send_esp, void,
    private_kernel_libipsec_router_t *this, esp_packet_t *packet, bool encap)
{
    if (encap)
        charon->sender->send_no_marker(charon->sender, (packet_t*)packet);
    else if (this->esp_handler)
        this->esp_handler->send(this->esp_handler, packet);
    else
        packet->destroy(packet);  /* shouldn't happen */
}
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_router.c:103-118]

NAT-T case: send through charon's regular sender (the same UDP socket
IKE uses, port 4500). Non-NAT case: hand the raw IP datagram to an
optional ESP handler (a SOCK_RAW on Linux; on Windows we'll use
WinDivert).

### 5.3 The TUN-per-VIP map

The router keeps a hashtable of `tun_entry_t { addr, fd, tun }` keyed
by VIP. Other plugins (e.g. an Android-style per-connection TUN
provider) can announce new TUN devices via the `tun()` listener hook:

```c
METHOD(kernel_listener_t, tun, bool,
    private_kernel_libipsec_router_t *this, tun_device_t *tun, bool created)
{
    tun_entry_t *entry, lookup;
    char buf[] = {0x01};

    this->lock->write_lock(this->lock);
    if (created)
    {
        INIT(entry, .addr = tun->get_address(tun, NULL),
                    .fd   = tun->get_fd(tun),
                    .tun  = tun);
        this->tuns->put(this->tuns, entry, entry);
    }
    else
    {
        lookup.addr = tun->get_address(tun, NULL);
        entry = this->tuns->remove(this->tuns, &lookup);
        free(entry);
    }
    /* notify handler thread to recreate FD set */
    ignore_result(write(this->notify[1], buf, sizeof(buf)));
    this->lock->unlock(this->lock);
    return TRUE;
}
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_router.c:253-279]

For a road-warrior client we won't usually have more than one VIP, so
the default `this->tun.tun` (the one the plugin created) is enough; the
hashtable can stay empty.

The kernel-side mechanism that fires `tun()` is in
`kernel_interface.c` [src/libcharon/kernel/kernel_interface.c:970]:
```
!listener->tun(listener, tun, created)
```

The listener hook itself is declared on `kernel_listener_t`:
```c
bool (*tun)(kernel_listener_t *this, tun_device_t *tun, bool created);
```
[src/libcharon/kernel/kernel_listener.h:114-121]

### 5.4 `get_tun_name` — bridge from `add_policy` to the router

`kernel_libipsec_ipsec.c::install_route()` calls back to the router to
ask which TUN's name to use as the route's `oif`:
```c
INIT(route,
    .if_name = router->get_tun_name(router, is_virtual ? src_ip : NULL),
    .src_ip  = src_ip,
    ...);
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_ipsec.c:458-463]

```c
METHOD(kernel_libipsec_router_t, get_tun_name, char*,
    private_kernel_libipsec_router_t *this, host_t *vip)
{
    tun_entry_t *entry, lookup = { .addr = vip };
    tun_device_t *tun;
    char *name;
    if (!vip)
        return strdup(this->tun.tun->get_name(this->tun.tun));
    this->lock->read_lock(this->lock);
    entry = this->tuns->get(this->tuns, &lookup);
    tun = entry ? entry->tun : this->tun.tun;
    name = strdup(tun->get_name(tun));
    this->lock->unlock(this->lock);
    return name;
}
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_router.c:281-300]

### 5.5 ESP handler (optional)

Header is small enough to quote whole:
```c
struct kernel_libipsec_esp_handler_t {
    void (*send)(kernel_libipsec_esp_handler_t *this, esp_packet_t *packet);
    void (*destroy)(kernel_libipsec_esp_handler_t *this);
};
kernel_libipsec_esp_handler_t *kernel_libipsec_esp_handler_create();
```
[src/libcharon/plugins/kernel_libipsec/kernel_libipsec_esp_handler.h:32-52]

This is the "raw ESP" escape hatch. If we want to support non-NATted
peers we'll implement an analogous WinDivert-backed handler.

### 5.6 Mirror plan for `kernel-wintun`

| libipsec file                       | Wintun analog               | Notes |
|-------------------------------------|-----------------------------|-------|
| `kernel_libipsec_plugin.c`          | `kernel_wintun_plugin.c`    | drop CAP_NET_ADMIN, create Wintun adapter, set `install_virtual_ip_on` |
| `kernel_libipsec_ipsec.c`           | `kernel_wintun_ipsec.c`     | identical except for the route helper, since `add_route` lives in the net backend |
| `kernel_libipsec_router.c`          | `kernel_wintun_router.c`    | replace `poll()` with `WaitForMultipleObjects`; rest of logic stays |
| `kernel_libipsec_esp_handler.c`     | `kernel_wintun_esp_handler.c` (M5) | WinDivert-based; out of scope for M3 |

---

## 6. `kernel-iph` and `kernel-wfp` snapshots

### 6.1 `kernel-iph`

Files: `src/libcharon/plugins/kernel_iph/`
* `kernel_iph_plugin.c` (76 lines)
* `kernel_iph_net.c` (785 lines)
* `kernel_iph_net.h` (47 lines)

Plugin features:
```c
static plugin_feature_t f[] = {
    PLUGIN_CALLBACK(kernel_net_register, kernel_iph_net_create),
        PLUGIN_PROVIDE(CUSTOM, "kernel-net"),
};
```
[src/libcharon/plugins/kernel_iph/kernel_iph_plugin.c:40-49]

So `kernel-iph` provides the **`kernel-net`** feature only. It does not
do `kernel-ipsec` at all.

What's stubbed/missing — both `add_ip` and `del_ip`:
```c
METHOD(kernel_net_t, add_ip, status_t,
    private_kernel_iph_net_t *this, host_t *virtual_ip, int prefix,
    char *iface_name)
{
    return NOT_SUPPORTED;
}

METHOD(kernel_net_t, del_ip, status_t,
    private_kernel_iph_net_t *this, host_t *virtual_ip, int prefix,
    bool wait)
{
    return NOT_SUPPORTED;
}
```
[src/libcharon/plugins/kernel_iph/kernel_iph_net.c:612-624]

`create_local_subnet_enumerator` is **absent from the method table
entirely** [src/libcharon/plugins/kernel_iph/kernel_iph_net.c:752-764] —
it leaves the slot NULL.

Working bits: address enumeration (with full
`NotifyIpInterfaceChange` integration), `get_source_addr` /
`get_nexthop` / `get_interface`, and route management via
`CreateIpForwardEntry2` / `DeleteIpForwardEntry2`.

### 6.2 `kernel-wfp`

Files: `src/libcharon/plugins/kernel_wfp/`
* `kernel_wfp_plugin.c` (79 lines)
* `kernel_wfp_ipsec.c` (2729 lines!)
* `kernel_wfp_compat.c/.h` — MinGW shims for `fwpmu.h`
* `ipsecdump.c` — debug tool

Plugin features:
```c
static plugin_feature_t f[] = {
    PLUGIN_CALLBACK(kernel_ipsec_register, kernel_wfp_ipsec_create),
        PLUGIN_PROVIDE(CUSTOM, "kernel-ipsec"),
            PLUGIN_DEPENDS(RNG, RNG_WEAK),
            PLUGIN_DEPENDS(RNG, RNG_STRONG),
};
```
[src/libcharon/plugins/kernel_wfp/kernel_wfp_plugin.c:41-52]

So `kernel-wfp` provides only **`kernel-ipsec`**, and depends on a weak
+ a strong RNG (used to generate SPIs locally — see the comment block
about `nextspi` / `mixspi` at
[src/libcharon/plugins/kernel_wfp/kernel_wfp_ipsec.c:38-54]).

How it installs SAs (high level):

* `add_sa(inbound)` is buffered into a `tsas` hashtable keyed by reqid
  [src/libcharon/plugins/kernel_wfp/kernel_wfp_ipsec.c:2117-2161].
* `add_sa(outbound)` looks the inbound entry up by reqid, fills in the
  outbound side, and submits **both** directions to WFP at once
  [src/libcharon/plugins/kernel_wfp/kernel_wfp_ipsec.c:2163-2199].
* The actual WFP calls (FWPM / IPSEC_SA_CONTEXT*) live in
  helper functions further up the file.
* Policies are turned into WFP filter rules so the Windows kernel does
  the actual ESP processing in-kernel.

Crucial limitation: WFP requires the IPsec **transport** (IPv4/v6
header + ESP header) to actually arrive at the host. For a road-warrior
client behind NAT, ESP-in-UDP works in WFP, but the kernel does **not**
expose virtual IP installation — that's why `kernel-iph::add_ip`
returns `NOT_SUPPORTED`. Without a VIP, charon has nowhere to write
the inner plaintext, so nothing reaches user applications.

### 6.3 The M4 architecture decision: can we keep `kernel-wfp`?

**No.** Here's why, with the evidence:

1. WFP processes ESP packets that reach the *physical* interface and
   delivers the plaintext to a *kernel-level* TCP/IP stack tied to a
   host route. There is no kernel concept of "deliver this plaintext to
   a TUN device" inside Windows that we can drive from WFP.
2. For a road-warrior VPN client we need the inner traffic to appear
   on the Wintun adapter so that the Windows route table can direct
   user-space sockets to it. That requires us, in user space, to:
   * receive ESP from the IKE socket,
   * decrypt it,
   * `WintunSendPacket()` the plaintext into the adapter.
3. That data path is exactly what `kernel_libipsec_ipsec_t` +
   `kernel_libipsec_router_t` implement. Once we own the SA decryption
   in user space, WFP's SA install becomes redundant — and harmful,
   because two `kernel-ipsec` providers would race for the same SA.

The strongSwan plugin loader will only **ever** use one `kernel-ipsec`
provider — `kernel_interface.c::add_ipsec_interface()` registers one
constructor at a time and any second `PROVIDE(CUSTOM, "kernel-ipsec")`
just bumps a refcount on the same registration ladder. So practically,
loading `kernel-wintun` means **not** loading `kernel-wfp`.

**Therefore: `kernel-wintun` must provide its own `kernel_ipsec_t`.**
That's good news because it's mostly a copy of
`kernel_libipsec_ipsec.c` (§2 / §5).

For `kernel-net` we have a real choice:

* **Option A (recommended):** keep `kernel-iph` as the `kernel-net`
  provider, but extend its `add_ip` / `del_ip` to call into our Wintun
  helper. This is the smallest delta to upstream.
* **Option B:** make `kernel-wintun` provide *both* `kernel-net` *and*
  `kernel-ipsec`, displacing `kernel-iph` entirely.

A single plugin **can** provide both: `kernel-libipsec` doesn't, but
nothing in the API prevents it — `get_features()` simply returns
multiple `PLUGIN_PROVIDE(CUSTOM, ...)` entries. The Linux
`kernel_netlink` plugin is the prior art:
```c
PLUGIN_CALLBACK(kernel_ipsec_register, kernel_netlink_ipsec_create),
    PLUGIN_PROVIDE(CUSTOM, "kernel-ipsec"),
        ...
PLUGIN_CALLBACK(kernel_net_register, kernel_netlink_net_create),
    PLUGIN_PROVIDE(CUSTOM, "kernel-net"),
        ...
```

**Recommendation for M3:** Option A. Provide only `kernel-ipsec` from
`kernel-wintun` (replacing `kernel-wfp`), and keep `kernel-iph` for
`kernel-net` after teaching its `add_ip` / `del_ip` to install on the
Wintun adapter that `kernel-wintun` exposes. This keeps the patch to
`kernel-iph` tiny (~30 lines) and avoids duplicating the
~700 lines of address/route enumeration that already work.

If for some reason we want a single self-contained plugin (Option B),
the `get_features()` return list grows to:
```c
PLUGIN_CALLBACK(kernel_ipsec_register, kernel_wintun_ipsec_create),
    PLUGIN_PROVIDE(CUSTOM, "kernel-ipsec"),
PLUGIN_CALLBACK(kernel_net_register, kernel_wintun_net_create),
    PLUGIN_PROVIDE(CUSTOM, "kernel-net"),
PLUGIN_CALLBACK((plugin_feature_callback_t)create_router, NULL),
    PLUGIN_PROVIDE(CUSTOM, "kernel-wintun-router"),
        PLUGIN_DEPENDS(CUSTOM, "libcharon-receiver"),
```

---

## Appendix: cheatsheet

| Need | Function | File |
|------|----------|------|
| Install VIP on TUN | `kernel_net_t::add_ip` | `kernel_net.h:147` |
| Add a route | `kernel_net_t::add_route` | `kernel_net.h:175` |
| Decrypt ESP in userspace | `kernel_ipsec_t::add_sa` → `ipsec->sas->add_sa` | `kernel_libipsec_ipsec.c:269` |
| Read plaintext from TUN | `tun_device_t::read_packet` | `tun_device.h:47` |
| Write plaintext to TUN | `tun_device_t::write_packet` | `tun_device.h:55` |
| Send ESP through IKE socket | `charon->sender->send_no_marker(...)` | `kernel_libipsec_router.c:108` |
| Receive ESP from IKE socket | `charon->receiver->add_esp_cb(...)` | `kernel_libipsec_router.c:364` |
| Queue inbound to libipsec | `ipsec->processor->queue_inbound(...)` | `kernel_libipsec_router.c:123` |
| Queue outbound to libipsec | `ipsec->processor->queue_outbound(...)` | `kernel_libipsec_router.c:157` |
| Inbound plaintext callback | `ipsec->processor->register_inbound(deliver_plain, ...)` | `kernel_libipsec_router.c:363` |
| Outbound ESP callback | `ipsec->processor->register_outbound(send_esp, ...)` | `kernel_libipsec_router.c:362` |
| Tell net backend "use my TUN for VIPs" | `lib->settings->set_str(... "install_virtual_ip_on" ...)` | `kernel_libipsec_plugin.c:158` |
| Publish TUN to other plugins | `lib->set(lib, "kernel-libipsec-tun", tun)` | `kernel_libipsec_plugin.c:155` |
| Register `kernel_ipsec_t` ctor | `PLUGIN_CALLBACK(kernel_ipsec_register, ...)` | `kernel_ipsec.h:400` |
| Register `kernel_net_t` ctor | `PLUGIN_CALLBACK(kernel_net_register, ...)` | `kernel_net.h:211` |
| Plugin entry point | `PLUGIN_DEFINE(kernel_wintun)` | `plugin.h:81` |
