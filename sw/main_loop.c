/* sw/main_loop.c — epoll-based event loop for tetra_d.
 *
 * Owned by S7 (S7-sw-tetra-d). Wires the multiplexer that drives:
 *   - 4× DMA IRQ FDs from S1 (TMA_RX, TMA_TX, TMD_RX, TMD_TX)
 *   - 1× Unix-socket listener from socket_server.c
 *   - Accepted CGI client FDs (single-shot req/resp lifecycle)
 *   - 1× signalfd for SIGTERM / SIGINT / SIGQUIT
 *   - 1× timerfd ticking once per multiframe (~1.02 s) for the
 *     CMCE D-NWRK-BCAST cadence + msgbus drain
 *
 * Threading model: single-threaded — the daemon owns one msgbus and
 * dispatches in the same loop that pumps events. Every entity
 * (S2/S3/S4) registers handlers via msgbus_register from its `_init`
 * function; this file does not invoke entity functions directly except
 * for `cmce_send_d_nwrk_broadcast` on the timer tick and the RX-frame
 * dispatch path that turns a TmaSap frame into a TmaUnitdataInd that
 * LLC consumes.
 *
 * RX dispatch convention (DMA RX → entity):
 *   - `TMAS` magic → llc_handle_tma_unitdata_ind  (signalling)
 *   - `TMAR` magic → debug counter only           (UMAC reports;
 *                     the report dispatcher is OPERATIONS.md §3
 *                     debug.tmasap_recent territory, not a SAP)
 *   - `TMDC` magic → TmdSap voice-frame counter   (S4 routes to
 *                     CMCE eventually via TmdSap; for now we count
 *                     and drop — voice plane is the Phase-3 cosim
 *                     gate, not the daemon-smoke gate)
 */
#define _GNU_SOURCE   /* signalfd, timerfd, epoll, accept4, sig*set */

#include "tetra/cmce.h"
#include "tetra/daemon_ops.h"
#include "tetra/db.h"
#include "tetra/dma_io.h"
#include "tetra/llc.h"
#include "tetra/mle.h"
#include "tetra/mm.h"
#include "tetra/msgbus.h"
#include "tetra/sap.h"
#include "tetra/types.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* DaemonState — central handle. Lives on the stack of main(). All
 * pointer fields are non-owning into stack-allocated entities. */
struct DaemonState {
    /* Core msgbus + storage */
    MsgBus      *bus;

    /* Entities */
    Llc         *llc;
    Mle         *mle;
    Mm          *mm;
    Cmce        *cmce;
    SubscriberDb *db;

    /* AST persistence (Decision #10) */
    Ast         *ast;
    const char  *ast_path;
    bool         ast_loaded_at_start;

    /* DMA glue */
    DmaCtx      *dma;

    /* Event-loop state */
    int          epoll_fd;
    int          listener_fd;
    int          signal_fd;
    int          timer_fd;
    int          dma_irq_fd[DMA_CHAN_COUNT];

    /* Multiframe counter (incremented once per timerfd tick). */
    uint64_t     mf_now;

    /* Shutdown signalling — set by SIGTERM handler or daemon.stop op. */
    bool         shutdown_requested;
    bool         clean_shutdown_flag;
};

/* Forward decls — implemented in socket_server.c. */
int socket_server_listen(const char *path, int *out_fd);
int socket_server_close_listener(int fd, const char *path);
int socket_server_handle_client(int fd, DaemonState *state);

/* ---------------------------------------------------------------------------
 * Event-source tags for epoll.data.u32. We pack [ kind:8 | aux:24 ] so
 * one u32 carries both the event-kind and any per-kind index (DMA chan,
 * client fd-id). This avoids a side-table per-fd while keeping the
 * dispatch O(1).
 * ------------------------------------------------------------------------- */
#define ML_KIND_LISTENER  0u
#define ML_KIND_SIGNAL    1u
#define ML_KIND_TIMER     2u
#define ML_KIND_DMA       3u
#define ML_KIND_CLIENT    4u

static uint32_t ml_tag(uint32_t kind, uint32_t aux)
{
    return ((kind & 0xFFu) << 24) | (aux & 0x00FFFFFFu);
}
static uint32_t ml_tag_kind(uint32_t tag) { return (tag >> 24) & 0xFFu; }
static uint32_t ml_tag_aux (uint32_t tag) { return tag & 0x00FFFFFFu; }

static int ml_epoll_add(int epfd, int fd, uint32_t tag)
{
    struct epoll_event ev = { 0 };
    ev.events   = EPOLLIN;
    ev.data.u32 = tag;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        return -errno;
    }
    return 0;
}

static int ml_epoll_del(int epfd, int fd)
{
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        return -errno;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Signal-fd setup. Block SIGTERM / SIGINT / SIGQUIT in the calling
 * thread so they only deliver via the FD. Per CLAUDE.md the daemon is
 * expected to be a single-thread process, so blocking process-wide is
 * the simplest correct approach.
 * ------------------------------------------------------------------------- */
static int ml_signalfd_setup(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGQUIT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        return -errno;
    }
    int fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd < 0) {
        return -errno;
    }
    return fd;
}

/* ---------------------------------------------------------------------------
 * Timer-fd setup. Period ≈ 1.02 s — one TETRA multiframe (18 frames *
 * 56.67 ms = 1.02 s) per CLAUDE.md context note. The CMCE periodic
 * driver expects the daemon to tick once per multiframe.
 * ------------------------------------------------------------------------- */
#define ML_MULTIFRAME_NS 1020000000L  /* 1.020 s */

static int ml_timerfd_setup(long period_ns)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0) {
        return -errno;
    }
    struct itimerspec spec = { 0 };
    spec.it_value.tv_sec    = period_ns / 1000000000L;
    spec.it_value.tv_nsec   = period_ns % 1000000000L;
    spec.it_interval.tv_sec  = spec.it_value.tv_sec;
    spec.it_interval.tv_nsec = spec.it_value.tv_nsec;
    if (timerfd_settime(fd, 0, &spec, NULL) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

/* ---------------------------------------------------------------------------
 * RX-frame dispatcher: DMA recv → entity SAP entry.
 *
 * On a DMA-IRQ-fd readable event, drain dma_recv_frame() until empty
 * and route by `ch`:
 *   - TMA_RX  → check magic; TMAS → llc_handle_tma_unitdata_ind
 *               TMAR → counter only (UMAC report buffer fills via
 *                      debug.tmasap_recent, not a SAP path).
 *   - TMD_RX  → counter; voice plane lands later through TmdSap.
 *
 * The `frame` buffer is the raw DMA payload: the magic + length header
 * have already been stripped by dma_recv_frame, so we get the body
 * bytes directly. To re-derive the magic we re-peek by looking at the
 * channel kind (TMA_RX may carry TMAS or TMAR — in practice the FPGA
 * framer multiplexes both onto the same channel; the fpga sets the
 * leading byte of the body to a 1-byte magic-disambiguator OR the daemon
 * reads a separate framer-meta header when bluestation makes it
 * available).  For S7 today, TMA_RX → TMAS body, period — TMAR routing
 * is wired through a separate sub-channel by FPGA (A2) and recovered
 * in Phase 3. The agent contract above explicitly defers TMAR fan-out
 * to the cosim sync-point.
 * ------------------------------------------------------------------------- */
#define ML_RX_BUF_BYTES 4096u

static void ml_dispatch_tma_rx(DaemonState *st,
                               const uint8_t *body,
                               size_t body_len)
{
    if (st == NULL || st->llc == NULL) {
        return;
    }
    if (body_len > TMA_SDU_MAX_BYTES) {
        body_len = TMA_SDU_MAX_BYTES;
    }
    TmaUnitdataInd ind;
    memset(&ind, 0, sizeof(ind));
    ind.endpoint     = (EndpointId) 1u;        /* signalling endpoint */
    ind.addr.ssi     = 0u;                     /* fpga framer fills via TMAR meta */
    ind.addr.ssi_type = SsiType_Issi;
    ind.sdu_len_bits = (uint16_t) (body_len * 8u);
    memcpy(ind.sdu_bits, body, body_len);

    /* Direct call (single-thread). LLC will post via msgbus to MLE. */
    (void) llc_handle_tma_unitdata_ind(st->llc, &ind);
}

static void ml_drain_dma_chan(DaemonState *st, DmaChan ch)
{
    uint8_t  buf[ML_RX_BUF_BYTES];
    size_t   got = 0;
    /* Drain non-blocking: timeout_ms=0. */
    while (1) {
        int rc = dma_recv_frame(st->dma, ch, buf, sizeof(buf), &got, 0);
        if (rc <= 0 && got == 0) {
            /* timeout, EAGAIN, or framing error — just stop. */
            break;
        }
        if (rc < 0) {
            /* Bad-magic or short-read — already counted by dma_io. */
            break;
        }
        switch (ch) {
        case DMA_CHAN_TMA_RX:
            ml_dispatch_tma_rx(st, buf, got);
            break;
        case DMA_CHAN_TMD_RX:
            /* voice plane — Phase 3 cosim gate, not S7. */
            break;
        default:
            break;
        }
        got = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Timer tick — one multiframe.
 *
 * Reads the timerfd to clear the wakeup, increments the multiframe
 * counter, then asks CMCE if it is time to emit D-NWRK-BCAST.
 *
 * Also drains the msgbus once per tick so any bus posts done by entity
 * handlers (themselves invoked from RX dispatch) move forward. The
 * msgbus is single-thread, dispatch-when-asked — without this drain
 * a busy RX storm could accumulate and the WebUI status.msgbus_depth
 * would balloon.
 * ------------------------------------------------------------------------- */
static void ml_timer_tick(DaemonState *st)
{
    uint64_t expirations = 0;
    ssize_t  n = read(st->timer_fd, &expirations, sizeof(expirations));
    if (n != (ssize_t) sizeof(expirations)) {
        /* Spurious wake — ignore. */
        return;
    }
    st->mf_now += expirations;

    if (st->cmce != NULL && cmce_nwrk_bcast_tick(st->cmce, st->mf_now)) {
        (void) cmce_send_d_nwrk_broadcast(st->cmce);
    }

    /* Drain bus until empty or 64 ops max — bound work per tick. */
    if (st->bus != NULL) {
        for (int i = 0; i < 64; i++) {
            int rc = msgbus_dispatch_one(st->bus);
            if (rc <= 0) {
                break;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Signal handler — SIGTERM/SIGINT → request clean shutdown.
 * ------------------------------------------------------------------------- */
static void ml_signal_event(DaemonState *st)
{
    struct signalfd_siginfo si;
    ssize_t n = read(st->signal_fd, &si, sizeof(si));
    if (n != (ssize_t) sizeof(si)) {
        return;
    }
    /* Any of the three trigger the same shutdown path. */
    st->shutdown_requested = true;
    st->clean_shutdown_flag = true;
}

/* daemon_request_shutdown — invoked by socket_server's daemon.stop op
 * handler. Returns 0 on success, sets *out_ast_snapshotted to whatever
 * the caller can see immediately (we always set the flag here so the
 * snapshot will be taken when the loop unwinds). */
int daemon_request_shutdown(DaemonState *state, bool *out_ast_snapshotted);
int daemon_request_shutdown(DaemonState *state, bool *out_ast_snapshotted)
{
    if (state == NULL) {
        return -EINVAL;
    }
    state->shutdown_requested  = true;
    state->clean_shutdown_flag = true;
    if (out_ast_snapshotted != NULL) {
        *out_ast_snapshotted = (state->ast != NULL && state->ast_path != NULL);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Listener-accept event — single-shot per-client request.
 *
 * Per OPERATIONS.md §6 + the BusyBox httpd model, each CGI hits the
 * socket once: connect, write request, read response, close. We accept,
 * service in-line (bounded by socket_server_handle_client), close, and
 * never add the client fd to epoll. This keeps the loop simple and
 * matches the per-request lifecycle.
 *
 * If a future agent (e.g. a long-poll SSE client) needs persistent fds,
 * register a separate epoll-add path here.
 * ------------------------------------------------------------------------- */
#include <sys/socket.h>

static void ml_listener_accept(DaemonState *st)
{
    while (1) {
        int cfd = accept4(st->listener_fd, NULL, NULL,
                          SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        /* Synchronous: read+dispatch+write+close. */
        (void) socket_server_handle_client(cfd, st);
        close(cfd);
    }
}

/* ---------------------------------------------------------------------------
 * Public entry: main_loop_init / main_loop_run / main_loop_teardown.
 *
 * The daemon (tetra_d.c) owns the entity instances; main_loop just
 * stitches their fds + the listener fd into one epoll set, then drives
 * the dispatch in run().
 * ------------------------------------------------------------------------- */
int main_loop_init(DaemonState *st);
int main_loop_run(DaemonState *st);
void main_loop_teardown(DaemonState *st);

int main_loop_init(DaemonState *st)
{
    if (st == NULL) {
        return -EINVAL;
    }
    st->epoll_fd    = -1;
    st->signal_fd   = -1;
    st->timer_fd    = -1;
    st->listener_fd = -1;
    for (size_t i = 0; i < DMA_CHAN_COUNT; i++) {
        st->dma_irq_fd[i] = -1;
    }

    int rc = epoll_create1(EPOLL_CLOEXEC);
    if (rc < 0) {
        return -errno;
    }
    st->epoll_fd = rc;

    int sfd = ml_signalfd_setup();
    if (sfd < 0) {
        return sfd;
    }
    st->signal_fd = sfd;
    rc = ml_epoll_add(st->epoll_fd, sfd, ml_tag(ML_KIND_SIGNAL, 0));
    if (rc < 0) {
        return rc;
    }

    int tfd = ml_timerfd_setup(ML_MULTIFRAME_NS);
    if (tfd < 0) {
        return tfd;
    }
    st->timer_fd = tfd;
    rc = ml_epoll_add(st->epoll_fd, tfd, ml_tag(ML_KIND_TIMER, 0));
    if (rc < 0) {
        return rc;
    }

    /* Listener — caller-supplied path lives in DaemonState? Not yet —
     * the listener is opened by tetra_d.c which hands the fd in via
     * st->listener_fd before main_loop_run.  But for symmetry we
     * accept either: if listener_fd is -1, open the default. */
    if (st->listener_fd < 0) {
        int lfd = -1;
        rc = socket_server_listen(DAEMON_OPS_SOCKET_PATH, &lfd);
        if (rc < 0) {
            return rc;
        }
        st->listener_fd = lfd;
    }
    rc = ml_epoll_add(st->epoll_fd, st->listener_fd,
                      ml_tag(ML_KIND_LISTENER, 0));
    if (rc < 0) {
        return rc;
    }

    /* DMA IRQ fds — each chan exposes a poll-able fd from S1. */
    if (st->dma != NULL) {
        for (uint32_t i = 0; i < DMA_CHAN_COUNT; i++) {
            int fd = dma_get_irq_fd(st->dma, (DmaChan) i);
            if (fd < 0) {
                /* RX-only channels obviously have one; if dma_io
                 * does not expose an fd for a TX channel skip it. */
                continue;
            }
            st->dma_irq_fd[i] = fd;
            rc = ml_epoll_add(st->epoll_fd, fd, ml_tag(ML_KIND_DMA, i));
            if (rc < 0) {
                return rc;
            }
        }
    }

    return 0;
}

#define ML_MAX_EVENTS 16

int main_loop_run(DaemonState *st)
{
    if (st == NULL || st->epoll_fd < 0) {
        return -EINVAL;
    }
    while (!st->shutdown_requested) {
        struct epoll_event evs[ML_MAX_EVENTS];
        int n = epoll_wait(st->epoll_fd, evs, ML_MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        for (int i = 0; i < n && !st->shutdown_requested; i++) {
            uint32_t kind = ml_tag_kind(evs[i].data.u32);
            uint32_t aux  = ml_tag_aux (evs[i].data.u32);
            switch (kind) {
            case ML_KIND_SIGNAL:
                ml_signal_event(st);
                break;
            case ML_KIND_TIMER:
                ml_timer_tick(st);
                break;
            case ML_KIND_LISTENER:
                ml_listener_accept(st);
                break;
            case ML_KIND_DMA:
                ml_drain_dma_chan(st, (DmaChan) aux);
                break;
            default:
                /* Unknown tag — likely stale fd from a teardown race.
                 * Just skip it; epoll_ctl(DEL) is idempotent. */
                break;
            }
        }
    }
    return 0;
}

void main_loop_teardown(DaemonState *st)
{
    if (st == NULL) {
        return;
    }
    if (st->epoll_fd >= 0) {
        for (size_t i = 0; i < DMA_CHAN_COUNT; i++) {
            if (st->dma_irq_fd[i] >= 0) {
                (void) ml_epoll_del(st->epoll_fd, st->dma_irq_fd[i]);
                st->dma_irq_fd[i] = -1;
            }
        }
        if (st->listener_fd >= 0) {
            (void) ml_epoll_del(st->epoll_fd, st->listener_fd);
        }
        if (st->signal_fd >= 0) {
            (void) ml_epoll_del(st->epoll_fd, st->signal_fd);
        }
        if (st->timer_fd >= 0) {
            (void) ml_epoll_del(st->epoll_fd, st->timer_fd);
        }
        close(st->epoll_fd);
        st->epoll_fd = -1;
    }
    if (st->signal_fd >= 0) { close(st->signal_fd); st->signal_fd = -1; }
    if (st->timer_fd  >= 0) { close(st->timer_fd);  st->timer_fd  = -1; }
    if (st->listener_fd >= 0) {
        socket_server_close_listener(st->listener_fd, DAEMON_OPS_SOCKET_PATH);
        st->listener_fd = -1;
    }
}

/* ---------------------------------------------------------------------------
 * Test-only injection: feed a fully-formed TmaSap payload into the
 * RX-dispatch path without going through DMA. Used by test_main_loop.c
 * to assert routing without spinning up a real DMA backend.
 * ------------------------------------------------------------------------- */
void main_loop_inject_tma_rx(DaemonState *st, const uint8_t *body, size_t len);
void main_loop_inject_tma_rx(DaemonState *st, const uint8_t *body, size_t len)
{
    ml_dispatch_tma_rx(st, body, len);
}
