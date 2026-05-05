/*
 * kernel-wintun router — Win32 port of kernel_libipsec_router.c.
 *
 * Architecture mirrors libipsec's router 1:1, with these substitutions:
 *
 *   POSIX                    Win32 / Wintun
 *   --------------------     ---------------------------------------
 *   pipe(notify)             CreateEventW(notify_event)
 *   write(notify[1], ...)    SetEvent(notify_event)
 *   read(notify[0], ...)     ResetEvent(notify_event)
 *   poll(pollfds)            WaitForMultipleObjects(handles)
 *   tun->get_fd()            WintunGetReadWaitEvent(session) [HANDLE]
 *   tun->read_packet()       WintunReceivePacket() + ReleaseReceivePacket()
 *   tun->write_packet()      WintunAllocateSendPacket() + WintunSendPacket()
 *
 * The Wintun adapter / session is provided by the plugin via
 *   lib->set(lib, "kernel-wintun-session", &g_session)
 * As long as no session is exposed (current state in M3.5), the worker
 * thread sits on the notify event indefinitely and does nothing — which
 * is the correct no-op behaviour until the adapter is wired up
 * (post-M3 milestone).
 */

/* wintun_loader.h FIRST — must establish winsock2/windows include order
 * before any strongSwan header pulls in compat/windows.h. */
#include "wintun_loader.h"
#include "kernel_wintun_session.h"

#include "kernel_wintun_router.h"

#include <daemon.h>
#include <ipsec.h>
#include <collections/hashtable.h>
#include <threading/rwlock.h>
#include <threading/thread.h>
#include <processing/jobs/callback_job.h>
#include <utils/debug.h>

typedef struct private_kernel_wintun_router_t private_kernel_wintun_router_t;

/**
 * Entry in the per-VIP TUN device map.
 */
typedef struct {
    /** virtual IP (points to internal data of session->name) */
    host_t                  *addr;
    /** Wintun session for this VIP */
    wintun_session_handle_t *sess;
} tun_entry_t;

/** singleton (set by create, NULLed by destroy) */
kernel_wintun_router_t *wintun_router = NULL;

/**
 * Private data.
 */
struct private_kernel_wintun_router_t {

    /** Public interface. */
    kernel_wintun_router_t public;

    /**
     * Default Wintun session (the one created by the plugin at init).
     * NULL until the plugin sets "kernel-wintun-session" via lib->set.
     */
    wintun_session_handle_t *tun;

    /**
     * Hashtable mapping virtual IPs to Wintun sessions (tun_entry_t).
     * Currently only one session is ever installed — the per-VIP map is
     * here to mirror libipsec's structure for future multi-tunnel use.
     */
    hashtable_t *tuns;

    /** Lock protecting tuns. */
    rwlock_t *lock;

    /**
     * Manual-reset event used by tun() listener callback to wake up the
     * worker thread when the TUN map changes (so the WaitForMultipleObjects
     * handle set can be rebuilt).
     */
    HANDLE notify_event;

    /**
     * Manual-reset event used in destroy() to break the worker out of
     * its WaitForMultipleObjects call.
     */
    HANDLE stop_event;

    /** Set to TRUE in destroy() so the worker bails out. */
    bool stopping;
};

/* --- Hashtable helpers --- */

static u_int tun_entry_hash(tun_entry_t *entry)
{
    return chunk_hash(entry->addr->get_address(entry->addr));
}

static bool tun_entry_equals(tun_entry_t *a, tun_entry_t *b)
{
    return a->addr->ip_equals(a->addr, b->addr);
}

/* --- Outgoing ESP path: libipsec -> IKE socket --- */

CALLBACK(send_esp, void,
    private_kernel_wintun_router_t *this, esp_packet_t *packet, bool encap)
{
    if (encap)
    {
        charon->sender->send_no_marker(charon->sender, (packet_t*)packet);
    }
    else
    {
        /* No raw-ESP handler on Windows yet — UDP encapsulation only. */
        packet->destroy(packet);
    }
}

/* --- Incoming ESP path: IKE socket -> libipsec --- */

CALLBACK(receiver_esp_cb, void,
    void *data, packet_t *packet)
{
    DBG2(DBG_NET, "kernel-wintun: ESP packet received, queueing for libipsec");
    ipsec->processor->queue_inbound(ipsec->processor,
                                    esp_packet_create_from_packet(packet));
}

/* --- libipsec inbound -> Wintun (deliver decrypted packet to TUN) --- */

CALLBACK(deliver_plain, void,
    private_kernel_wintun_router_t *this, ip_packet_t *packet)
{
    tun_entry_t *entry, lookup = {
        .addr = packet->get_destination(packet),
    };
    wintun_session_handle_t *sess;
    chunk_t enc;
    BYTE *buf;

    this->lock->read_lock(this->lock);
    entry = this->tuns->get(this->tuns, &lookup);
    sess = entry ? entry->sess : this->tun;
    if (sess && sess->session && WintunAllocateSendPacket)
    {
        enc = packet->get_encoding(packet);
        buf = WintunAllocateSendPacket(sess->session, enc.len);
        if (buf)
        {
            memcpy(buf, enc.ptr, enc.len);
            WintunSendPacket(sess->session, buf);
        }
    }
    this->lock->unlock(this->lock);
    packet->destroy(packet);
}

/* --- Wintun -> libipsec outbound (drain a session's RX ring) --- */

static void process_plain(wintun_session_handle_t *sess)
{
    DWORD sz = 0;
    BYTE *raw;
    chunk_t chunk;
    ip_packet_t *packet;

    if (!sess || !sess->session)
    {
        return;
    }
    /* Drain everything currently queued — the read_event is auto-reset
     * by the system once we've consumed the ring. */
    while ((raw = WintunReceivePacket(sess->session, &sz)) != NULL)
    {
        chunk = chunk_create(raw, sz);
        packet = ip_packet_create(chunk_clone(chunk));
        if (packet)
        {
            ipsec->processor->queue_outbound(ipsec->processor, packet);
        }
        else
        {
            DBG1(DBG_KNL, "kernel-wintun: invalid IP packet from TUN");
        }
        WintunReleaseReceivePacket(sess->session, raw);
    }
}

/* --- Worker thread: WaitForMultipleObjects loop --- */

static job_requeue_t handle_plain(private_kernel_wintun_router_t *this)
{
    enumerator_t *enumerator;
    tun_entry_t *entry;
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    wintun_session_handle_t *sess_by_idx[MAXIMUM_WAIT_OBJECTS] = { NULL };
    DWORD count = 0;
    DWORD r;
    bool oldstate;

    /* Build the wait set:
     *   [0] stop_event   — unblocks us at shutdown
     *   [1] notify_event — TUN map changed, rebuild
     *   [2] default tun read event (if any)
     *   [3..] per-VIP tun read events
     */
    handles[count++] = this->stop_event;
    handles[count++] = this->notify_event;

    this->lock->read_lock(this->lock);
    if (this->tun && this->tun->read_event)
    {
        sess_by_idx[count] = this->tun;
        handles[count++] = this->tun->read_event;
    }
    enumerator = this->tuns->create_enumerator(this->tuns);
    while (count < MAXIMUM_WAIT_OBJECTS &&
           enumerator->enumerate(enumerator, NULL, &entry))
    {
        if (entry->sess && entry->sess->read_event)
        {
            sess_by_idx[count] = entry->sess;
            handles[count++] = entry->sess->read_event;
        }
    }
    enumerator->destroy(enumerator);
    this->lock->unlock(this->lock);

    if (count <= 2)
    {
        /* No TUN session yet — wait on stop+notify only, with a longer
         * timeout so we don't busy-spin while the plugin is wiring up
         * the adapter. */
        oldstate = thread_cancelability(TRUE);
        r = WaitForMultipleObjects(count, handles, FALSE, 1000);
        thread_cancelability(oldstate);
    }
    else
    {
        oldstate = thread_cancelability(TRUE);
        r = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
        thread_cancelability(oldstate);
    }

    if (this->stopping || r == WAIT_OBJECT_0)
    {
        return JOB_REQUEUE_NONE;
    }
    if (r == WAIT_TIMEOUT)
    {
        return JOB_REQUEUE_DIRECT;
    }
    if (r == WAIT_FAILED)
    {
        DBG1(DBG_KNL, "kernel-wintun: WaitForMultipleObjects failed: %lu",
             GetLastError());
        return JOB_REQUEUE_FAIR;
    }

    if (r == WAIT_OBJECT_0 + 1)
    {
        /* TUN map changed — reset notify event and rebuild on next pass. */
        ResetEvent(this->notify_event);
        return JOB_REQUEUE_DIRECT;
    }

    /* A TUN session has packets ready. */
    {
        DWORD idx = r - WAIT_OBJECT_0;
        if (idx < count && sess_by_idx[idx])
        {
            process_plain(sess_by_idx[idx]);
        }
    }
    return JOB_REQUEUE_DIRECT;
}

/* --- callback_job cancel callback ---
 *
 * Called by the processor during shutdown. We *cannot* rely on
 * pthread_cancel here: winpthreads-based cancellation only fires at
 * cancellation points, and WaitForMultipleObjects is not one. Returning
 * TRUE signals the framework to skip thread cancellation and just join
 * the worker -- which will exit on its own once it sees stop_event.
 */
static bool cancel_worker(private_kernel_wintun_router_t *this)
{
    this->stopping = TRUE;
    if (this->stop_event)
    {
        SetEvent(this->stop_event);
    }
    return TRUE;
}

/* --- kernel_listener_t::tun callback --- */

METHOD(kernel_listener_t, tun, bool,
    private_kernel_wintun_router_t *this, tun_device_t *tun, bool created)
{
    /* On Windows tun_device_t is a NULL stub. We don't track per-tun
     * entries through this callback; sessions are registered by the
     * plugin via lib->set/get. Just signal the worker so it re-evaluates
     * the wait set. */
    (void)tun;
    (void)created;
    SetEvent(this->notify_event);
    return TRUE;
}

/* --- Public methods --- */

METHOD(kernel_wintun_router_t, get_tun_name, char*,
    private_kernel_wintun_router_t *this, host_t *vip)
{
    tun_entry_t *entry, lookup = { .addr = vip };
    wintun_session_handle_t *sess;
    char *name = NULL;

    if (!vip)
    {
        if (this->tun && this->tun->name)
        {
            return strdup(this->tun->name);
        }
        return NULL;
    }
    this->lock->read_lock(this->lock);
    entry = this->tuns->get(this->tuns, &lookup);
    sess = entry ? entry->sess : this->tun;
    if (sess && sess->name)
    {
        name = strdup(sess->name);
    }
    this->lock->unlock(this->lock);
    return name;
}

METHOD(kernel_wintun_router_t, destroy_, void,
    private_kernel_wintun_router_t *this)
{
    this->stopping = TRUE;
    if (this->stop_event)
    {
        SetEvent(this->stop_event);
    }
    /* Give the worker a moment to notice; the callback_job framework
     * will cancel/join on plugin tear-down. */

    charon->receiver->del_esp_cb(charon->receiver, receiver_esp_cb);
    ipsec->processor->unregister_outbound(ipsec->processor, send_esp);
    ipsec->processor->unregister_inbound(ipsec->processor, deliver_plain);
    charon->kernel->remove_listener(charon->kernel, &this->public.listener);

    if (this->lock)
    {
        this->lock->destroy(this->lock);
    }
    if (this->tuns)
    {
        this->tuns->destroy(this->tuns);
    }
    if (this->notify_event)
    {
        CloseHandle(this->notify_event);
    }
    if (this->stop_event)
    {
        CloseHandle(this->stop_event);
    }

    wintun_router = NULL;
    free(this);
}

kernel_wintun_router_t *kernel_wintun_router_create(void)
{
    private_kernel_wintun_router_t *this;

    INIT(this,
        .public = {
            .listener = {
                .tun = _tun,
            },
            .get_tun_name = _get_tun_name,
            .destroy = _destroy_,
        },
    );

    this->notify_event = CreateEventW(NULL, TRUE,  FALSE, NULL);  /* manual */
    this->stop_event   = CreateEventW(NULL, TRUE,  FALSE, NULL);  /* manual */
    if (!this->notify_event || !this->stop_event)
    {
        DBG1(DBG_KNL, "kernel-wintun: CreateEvent failed: %lu", GetLastError());
        if (this->notify_event) CloseHandle(this->notify_event);
        if (this->stop_event)   CloseHandle(this->stop_event);
        free(this);
        return NULL;
    }

    /* The plugin will hand us its session via lib->set("kernel-wintun-session").
     * If it's already there at create time (post M5), pick it up. */
    this->tun = lib->get(lib, "kernel-wintun-session");

    this->tuns = hashtable_create((hashtable_hash_t)tun_entry_hash,
                                  (hashtable_equals_t)tun_entry_equals, 4);
    this->lock = rwlock_create(RWLOCK_TYPE_DEFAULT);

    charon->kernel->add_listener(charon->kernel, &this->public.listener);
    ipsec->processor->register_outbound(ipsec->processor, send_esp, this);
    ipsec->processor->register_inbound(ipsec->processor, deliver_plain, this);
    charon->receiver->add_esp_cb(charon->receiver, receiver_esp_cb, NULL);

    lib->processor->queue_job(lib->processor,
        (job_t*)callback_job_create((callback_job_cb_t)handle_plain, this,
                                    NULL, (callback_job_cancel_t)cancel_worker));

    wintun_router = &this->public;
    DBG1(DBG_KNL, "kernel-wintun router: Win32 worker thread started");
    return &this->public;
}
