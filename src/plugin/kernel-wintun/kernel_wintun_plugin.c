/*
 * kernel-wintun plugin — M4 (full Wintun adapter wired up).
 *
 * PLUGIN_DEFINE now:
 *   1. loads wintun.dll (wintun_load) and registers a logger
 *   2. creates the Wintun adapter ("dhc-vpn", tunnel type "dhc-vpn")
 *   3. opens a Wintun session and grabs its read-wait event handle
 *   4. populates a wintun_session_handle_t with adapter/session/luid/name
 *   5. exposes it via lib->set("kernel-wintun-session", ...) for the
 *      router and lib->set("kernel-wintun-luid", ...) for kernel-iph
 *   6. sets install_virtual_ip_on so charon picks our adapter for VIPs
 *   7. initialises libipsec
 *
 * Requires Admin/elevation: WintunCreateAdapter rolls out the driver on
 * first call. As a Windows Service, charon-svc runs as LocalSystem and
 * has the required privileges.
 */

/* wintun_loader.h FIRST — establishes winsock2/windows include order
 * before any strongSwan header undefs CALLBACK. */
#include "wintun_loader.h"
#include "kernel_wintun_session.h"
#include <iphlpapi.h>
#include <netioapi.h>

#include "kernel_wintun_plugin.h"
#include "kernel_wintun_ipsec.h"
#include "kernel_wintun_router.h"
#include "kernel_wintun_capture.h"

#include <daemon.h>
#include <ipsec.h>
#include <utils/debug.h>

#define WINTUN_RING_SIZE   (2 * 1024 * 1024)   /* 2 MiB */
#define WINTUN_ADAPTER_NAME L"dhc-vpn"
#define WINTUN_TUNNEL_TYPE  L"dhc-vpn"

typedef struct private_kernel_wintun_plugin_t private_kernel_wintun_plugin_t;

struct private_kernel_wintun_plugin_t {

    /** public interface */
    kernel_wintun_plugin_t public;

    /**
     * Wintun session bundle. Allocated in PLUGIN_DEFINE, exposed via
     * lib->set("kernel-wintun-session"), torn down in destroy().
     */
    wintun_session_handle_t *sess;

    /**
     * Packet router. Created via the create_router callback registered
     * in get_features(); has a worker thread that pumps packets between
     * sess->session and libipsec.
     */
    kernel_wintun_router_t *router;

    /**
     * Inbound ESP capture via WinDivert. Created via create_capture in
     * get_features(); supplies ipsec->processor->queue_inbound with the
     * ESP packets that Windows tcpip.sys never delivers to userland
     * sockets.
     */
    kernel_wintun_capture_t *capture;
};

/* --- Wintun logger callback (uses Win32 CALLBACK convention) --- */

#pragma push_macro("CALLBACK")
#undef CALLBACK
#define CALLBACK __stdcall

static const char *level_str(WINTUN_LOGGER_LEVEL lvl) {
    switch (lvl) {
        case WINTUN_LOG_INFO: return "info";
        case WINTUN_LOG_WARN: return "warn";
        case WINTUN_LOG_ERR:  return "err ";
        default: return "?   ";
    }
}

static VOID CALLBACK wintun_log_cb(WINTUN_LOGGER_LEVEL lvl, DWORD64 ts,
                                   LPCWSTR msg)
{
    /* We can't easily forward wide chars to charon's logger from a
     * non-strongswan thread; print to stderr for now. */
    (void)ts;
    fwprintf(stderr, L"[wintun %hs] %ls\n", level_str(lvl), msg);
}

#pragma pop_macro("CALLBACK")

/* --- plugin_t methods --- */

METHOD(plugin_t, get_name, char*,
    private_kernel_wintun_plugin_t *this)
{
    return "kernel-wintun";
}

static bool create_router(private_kernel_wintun_plugin_t *this,
                          plugin_feature_t *feature, bool reg, void *arg)
{
    if (reg)
    {
        this->router = kernel_wintun_router_create();
        return this->router != NULL;
    }
    DESTROY_IF(this->router);
    this->router = NULL;
    return TRUE;
}

static bool create_capture(private_kernel_wintun_plugin_t *this,
                           plugin_feature_t *feature, bool reg, void *arg)
{
    if (reg)
    {
        /* Capture is best-effort: if WinDivert is unavailable (driver not
         * present, signature blocked, etc.) we still want the plugin to
         * load -- charon will work, just inbound ESP won't flow. The
         * router and IKE path are unaffected. */
        this->capture = kernel_wintun_capture_create();
        if (!this->capture)
        {
            DBG1(DBG_KNL, "kernel-wintun: capture component unavailable, "
                          "continuing without inbound ESP delivery");
        }
        return TRUE;
    }
    DESTROY_IF(this->capture);
    this->capture = NULL;
    return TRUE;
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
        PLUGIN_CALLBACK((plugin_feature_callback_t)create_capture, NULL),
            PLUGIN_PROVIDE(CUSTOM, "kernel-wintun-capture"),
                PLUGIN_DEPENDS(CUSTOM, "kernel-wintun-router"),
    };
    *features = f;
    return countof(f);
}

/**
 * Tear down everything PLUGIN_DEFINE set up.
 */
METHOD(plugin_t, destroy, void,
    private_kernel_wintun_plugin_t *this)
{
    if (this->sess)
    {
        lib->set(lib, "kernel-wintun-session", NULL);
        lib->set(lib, "kernel-wintun-luid",    NULL);
        if (this->sess->session && WintunEndSession)
        {
            WintunEndSession(this->sess->session);
        }
        if (this->sess->adapter && WintunCloseAdapter)
        {
            WintunCloseAdapter(this->sess->adapter);
        }
        free(this->sess->name);
        free(this->sess);
    }
    if (WintunDeleteDriver)
    {
        /* removes the driver if no other adapter is using it; non-fatal */
        WintunDeleteDriver();
    }
    wintun_unload();
    libipsec_deinit();
    free(this);
}

/* --- Helpers used by PLUGIN_DEFINE --- */

/**
 * Convert the adapter LUID to its friendly name (UTF-8) so we can
 * pass it to charon's "install_virtual_ip_on" setting and use it in
 * lib->set("kernel-wintun-name").
 */
static char *resolve_adapter_alias(NET_LUID luid)
{
    WCHAR aliasW[IF_MAX_STRING_SIZE + 1] = { 0 };
    if (ConvertInterfaceLuidToAlias(&luid, aliasW, IF_MAX_STRING_SIZE) != NO_ERROR)
    {
        return strdup("dhc-vpn");  /* fallback to our requested name */
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, aliasW, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
    {
        return strdup("dhc-vpn");
    }
    char *out = malloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, aliasW, -1, out, n, NULL, NULL);
    return out;
}

PLUGIN_DEFINE(kernel_wintun)
{
    private_kernel_wintun_plugin_t *this;
    DWORD ver, le;

    INIT(this,
        .public = {
            .plugin = {
                .get_name     = _get_name,
                .get_features = _get_features,
                .destroy      = _destroy,
            },
        },
    );

    /* libipsec first — without it the ipsec methods can't run, and
     * libipsec_deinit in destroy is safe even if init failed. */
    if (!libipsec_init())
    {
        DBG1(DBG_LIB, "kernel-wintun: libipsec_init() failed");
        free(this);
        return NULL;
    }

    /* Wintun DLL — wintun.dll must sit next to charon-svc.exe (or in
     * System32). For development we copy it via build-charon.sh post
     * step (see scripts/). */
    le = wintun_load();
    if (le != 0)
    {
        DBG1(DBG_KNL, "kernel-wintun: wintun_load failed: GLE=%lu", le);
        libipsec_deinit();
        free(this);
        return NULL;
    }
    WintunSetLogger(wintun_log_cb);

    INIT(this->sess,
        .name = NULL,
    );

    this->sess->adapter = WintunCreateAdapter(WINTUN_ADAPTER_NAME,
                                              WINTUN_TUNNEL_TYPE, NULL);
    if (!this->sess->adapter)
    {
        DBG1(DBG_KNL, "kernel-wintun: WintunCreateAdapter failed: GLE=%lu "
             "(5=ACCESS_DENIED -> need admin/SYSTEM)", GetLastError());
        free(this->sess);
        this->sess = NULL;
        wintun_unload();
        libipsec_deinit();
        free(this);
        return NULL;
    }

    ver = WintunGetRunningDriverVersion();
    DBG1(DBG_KNL, "kernel-wintun: adapter created, driver version %lu.%lu",
         (ver >> 16) & 0xFFFFul, ver & 0xFFFFul);

    WintunGetAdapterLUID(this->sess->adapter, &this->sess->luid);
    this->sess->name = resolve_adapter_alias(this->sess->luid);

    this->sess->session = WintunStartSession(this->sess->adapter,
                                             WINTUN_RING_SIZE);
    if (!this->sess->session)
    {
        DBG1(DBG_KNL, "kernel-wintun: WintunStartSession failed: GLE=%lu",
             GetLastError());
        WintunCloseAdapter(this->sess->adapter);
        free(this->sess->name);
        free(this->sess);
        this->sess = NULL;
        wintun_unload();
        libipsec_deinit();
        free(this);
        return NULL;
    }
    this->sess->read_event = WintunGetReadWaitEvent(this->sess->session);

    /* Expose the session and the LUID so the router and the kernel-iph
     * patch can find them. The router will pick the session up the next
     * time the worker rebuilds its wait set; we trigger that by setting
     * notify_event indirectly on next destroy/restart, but during normal
     * startup the router was created AFTER us via the plugin loader, so
     * it will see the session already on first iteration. */
    lib->set(lib, "kernel-wintun-session", this->sess);
    lib->set(lib, "kernel-wintun-luid",    &this->sess->luid);

    /* Tell charon to install virtual IPs on our adapter. */
    lib->settings->set_str(lib->settings, "%s.install_virtual_ip_on",
                           this->sess->name, lib->ns);

    DBG1(DBG_KNL, "kernel-wintun: ready -- adapter '%s' (LUID 0x%llx), "
                  "ring %u bytes",
         this->sess->name, (unsigned long long)this->sess->luid.Value,
         WINTUN_RING_SIZE);

    return &this->public.plugin;
}
