/* tb/cosim/shm_dma_bridge.c — POSIX shm + futex DMA bridge for T2 cosim.
 *
 * Owned by Agent T2 (T2-cosim-verilator). C11 implementation of the
 * API in tb/cosim/include/cosim_shm.h.
 *
 * Two roles share a single shm region with four SPSC byte rings, one
 * per IF_DMA_API_v1 channel:
 *
 *   verilator harness        daemon (tetra_d)
 *   ─────────────────        ─────────────────
 *     write TMA_RX  ──────►   read TMA_RX
 *     read  TMA_TX  ◄──────   write TMA_TX
 *     write TMD_RX  ──────►   read TMD_RX
 *     read  TMD_TX  ◄──────   write TMD_TX
 *
 * Wire format inside each ring is the IF_DMA_API_v1 frame format
 * (MAGIC + LEN_BE + payload), identical to what sw/dma_io/dma_io.c's
 * pipe-mock writes/reads. That keeps the daemon binary unchanged
 * regardless of whether it runs against the pipe-mock (host tests)
 * or the shm bridge (cosim).
 *
 * Sync model:
 *   - Each ring has a head, tail, and a 32-bit `waiter` futex word.
 *   - Writer publishes: store payload into buf, advance head with
 *     a release fence, wake any sleeper via futex(FUTEX_WAKE).
 *   - Reader pops: load head with acquire fence; if head==tail and
 *     timeout_ms != 0, set waiter=1 and futex(FUTEX_WAIT); on wake,
 *     clear waiter and re-check.
 *
 * The implementation is deliberately conservative: SPSC only, one
 * reader and one writer per ring. Concurrent producers/consumers on
 * the same ring are not supported and not needed for T2.
 *
 * In fallback mode (no Verilator), only the self-test path is
 * exercised: it creates a region, runs a few send/recv round trips
 * inside the same process, and tears down. That ensures the bridge
 * code is at least kept compiling + green in CI even before the
 * full Verilator path is wired.
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include "cosim_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Big-endian load/store helpers — local; not worth a dep on <endian.h>.
 * ------------------------------------------------------------------------- */
static uint32_t be32_load(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24)
         | ((uint32_t) p[1] << 16)
         | ((uint32_t) p[2] <<  8)
         | ((uint32_t) p[3] <<  0);
}

static void be32_store(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >>  8);
    p[3] = (uint8_t) (v >>  0);
}

/* ---------------------------------------------------------------------------
 * Futex syscall wrappers. glibc has no first-class futex(2) wrapper;
 * we go through SYS_futex directly. The 32-bit waiter word is in
 * shared memory (mmap).
 * ------------------------------------------------------------------------- */
static int futex_wait_word(volatile uint32_t *waddr,
                           uint32_t           expected,
                           int                timeout_ms)
{
    struct timespec ts;
    struct timespec *p_ts = NULL;

    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (long) (timeout_ms % 1000) * 1000000L;
        p_ts = &ts;
    }

    long rc = syscall(SYS_futex, waddr, FUTEX_WAIT, expected,
                      p_ts, NULL, 0);
    if (rc < 0) {
        return -errno;
    }
    return 0;
}

static int futex_wake_one(volatile uint32_t *waddr)
{
    long rc = syscall(SYS_futex, waddr, FUTEX_WAKE, 1, NULL, NULL, 0);
    if (rc < 0) {
        return -errno;
    }
    return (int) rc;  /* number of waiters woken (0 or 1) */
}

/* ---------------------------------------------------------------------------
 * Ring helpers. head/tail are monotonic byte counters; the modulo
 * is taken at the index. Since COSIM_SHM_RING_CAP is a power of two
 * we use a bitmask.
 * ------------------------------------------------------------------------- */
_Static_assert((COSIM_SHM_RING_CAP & (COSIM_SHM_RING_CAP - 1)) == 0,
               "COSIM_SHM_RING_CAP must be a power of two");

static uint32_t ring_used(const CosimShmRing *r)
{
    /* head is producer-only writer, tail is consumer-only writer.
     * Even with relaxed atomics on x86-64 the difference is well-
     * defined for our SPSC pattern: producers see their own head
     * and a possibly-stale tail (≤ actual). Conservative under-
     * estimation of free space is the safe direction. */
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    return h - t;
}

static uint32_t ring_free(const CosimShmRing *r)
{
    return COSIM_SHM_RING_CAP - ring_used(r);
}

/* Copy `nbytes` from `src` into the ring at the producer's head,
 * advancing head atomically. Caller has already ensured nbytes <=
 * ring_free(). */
static void ring_push(CosimShmRing *r, const uint8_t *src, uint32_t nbytes)
{
    uint32_t h    = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t off  = h & (COSIM_SHM_RING_CAP - 1u);
    uint32_t tail = COSIM_SHM_RING_CAP - off;

    if (nbytes <= tail) {
        memcpy(&r->buf[off], src, nbytes);
    } else {
        memcpy(&r->buf[off], src, tail);
        memcpy(&r->buf[0],   src + tail, nbytes - tail);
    }

    __atomic_store_n(&r->head, h + nbytes, __ATOMIC_RELEASE);
}

/* Copy `nbytes` from the consumer's tail into `dst`, advancing tail
 * atomically. Caller has already ensured nbytes <= ring_used(). */
static void ring_pop(CosimShmRing *r, uint8_t *dst, uint32_t nbytes)
{
    uint32_t t   = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    uint32_t off = t & (COSIM_SHM_RING_CAP - 1u);
    uint32_t tail = COSIM_SHM_RING_CAP - off;

    if (nbytes <= tail) {
        memcpy(dst, &r->buf[off], nbytes);
    } else {
        memcpy(dst,        &r->buf[off], tail);
        memcpy(dst + tail, &r->buf[0],   nbytes - tail);
    }

    __atomic_store_n(&r->tail, t + nbytes, __ATOMIC_RELEASE);
}

/* Peek without advancing; used by recv_frame to inspect the
 * 8-byte magic+length header before deciding whether the full
 * frame is present. */
static void ring_peek(const CosimShmRing *r, uint32_t skip,
                      uint8_t *dst, uint32_t nbytes)
{
    uint32_t t   = __atomic_load_n(&r->tail, __ATOMIC_RELAXED) + skip;
    uint32_t off = t & (COSIM_SHM_RING_CAP - 1u);
    uint32_t tail = COSIM_SHM_RING_CAP - off;

    if (nbytes <= tail) {
        memcpy(dst, &r->buf[off], nbytes);
    } else {
        memcpy(dst,        &r->buf[off], tail);
        memcpy(dst + tail, &r->buf[0],   nbytes - tail);
    }
}

/* ---------------------------------------------------------------------------
 * Path helpers — POSIX shm names live under /dev/shm and must start
 * with a single '/'.
 * ------------------------------------------------------------------------- */
static int build_shm_name(char *out, size_t cap, const char *path)
{
    if (path == NULL || cap < 2u) {
        return -EINVAL;
    }
    if (path[0] == '/') {
        if (strlen(path) + 1u > cap) return -ENAMETOOLONG;
        strncpy(out, path, cap - 1u);
        out[cap - 1u] = '\0';
        return 0;
    }
    int n = snprintf(out, cap, "/%s", path);
    if (n < 0 || (size_t) n >= cap) {
        return -ENAMETOOLONG;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Lifecycle.
 * ------------------------------------------------------------------------- */
int cosim_shm_create(const char *shm_path, CosimShmRegion **out_region)
{
    if (shm_path == NULL || out_region == NULL) return -EINVAL;

    char name[256];
    int rc = build_shm_name(name, sizeof name, shm_path);
    if (rc != 0) return rc;

    /* Best-effort unlink in case a previous run left a stale region. */
    (void) shm_unlink(name);

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return -errno;

    if (ftruncate(fd, (off_t) sizeof(CosimShmRegion)) != 0) {
        int e = errno;
        close(fd);
        shm_unlink(name);
        return -e;
    }

    void *p = mmap(NULL, sizeof(CosimShmRegion),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        int e = errno;
        close(fd);
        shm_unlink(name);
        return -e;
    }
    close(fd);

    CosimShmRegion *region = (CosimShmRegion *) p;
    memset(region, 0, sizeof *region);
    region->magic         = COSIM_SHM_MAGIC;
    region->version       = COSIM_SHM_VERSION;
    region->pid_verilator = (uint32_t) getpid();

    *out_region = region;
    return 0;
}

int cosim_shm_attach(const char *shm_path, CosimShmRegion **out_region)
{
    if (shm_path == NULL || out_region == NULL) return -EINVAL;

    char name[256];
    int rc = build_shm_name(name, sizeof name, shm_path);
    if (rc != 0) return rc;

    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) return -errno;

    void *p = mmap(NULL, sizeof(CosimShmRegion),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        int e = errno;
        close(fd);
        return -e;
    }
    close(fd);

    CosimShmRegion *region = (CosimShmRegion *) p;
    if (region->magic != COSIM_SHM_MAGIC ||
        region->version != COSIM_SHM_VERSION) {
        munmap(p, sizeof(CosimShmRegion));
        return -EBADMSG;
    }

    region->pid_daemon = (uint32_t) getpid();
    *out_region = region;
    return 0;
}

void cosim_shm_close(CosimShmRegion *region, bool unlink_path,
                     const char *shm_path)
{
    if (region == NULL) return;
    munmap(region, sizeof(CosimShmRegion));
    if (unlink_path && shm_path != NULL) {
        char name[256];
        if (build_shm_name(name, sizeof name, shm_path) == 0) {
            (void) shm_unlink(name);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Frame I/O.
 * ------------------------------------------------------------------------- */
static int64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t) ts.tv_sec * 1000 + (int64_t) ts.tv_nsec / 1000000;
}

int cosim_shm_send_frame(CosimShmRegion *region,
                         CosimChan       ch,
                         uint32_t        magic,
                         const uint8_t  *payload,
                         uint32_t        payload_len,
                         int             timeout_ms)
{
    if (region == NULL || (ch >= COSIM_CHAN_COUNT) ||
        (payload == NULL && payload_len > 0u)) {
        return -EINVAL;
    }
    if ((uint64_t) payload_len + 8u > COSIM_SHM_RING_CAP) {
        return -E2BIG;
    }

    CosimShmRing *r = &region->rings[ch];

    uint8_t hdr[8];
    be32_store(&hdr[0], magic);
    be32_store(&hdr[4], payload_len);

    uint32_t need = 8u + payload_len;

    int64_t deadline = (timeout_ms >= 0) ? now_ms() + timeout_ms : 0;
    for (;;) {
        if (ring_free(r) >= need) {
            ring_push(r, hdr, 8u);
            if (payload_len > 0u) ring_push(r, payload, payload_len);
            /* Wake the consumer if it is sleeping on this ring. */
            if (__atomic_load_n(&r->waiter, __ATOMIC_ACQUIRE) != 0u) {
                __atomic_store_n(&r->waiter, 0u, __ATOMIC_RELEASE);
                (void) futex_wake_one(&r->waiter);
            }
            return 0;
        }
        if (timeout_ms == 0) return -ENOSPC;
        if (timeout_ms > 0 && now_ms() >= deadline) return -ETIMEDOUT;
        /* Backpressure: we don't have a full-side futex (T2 doesn't
         * need it — the rings are 64 KiB and per-scenario load is
         * <= 1 KiB). Spin-yield is fine here. */
        struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = 1000000L };
        nanosleep(&sleep_ts, NULL);
    }
}

int cosim_shm_recv_frame(CosimShmRegion *region,
                         CosimChan       ch,
                         uint32_t       *out_magic,
                         uint8_t        *buf,
                         size_t          cap,
                         size_t         *out_len,
                         int             timeout_ms)
{
    if (region == NULL || (ch >= COSIM_CHAN_COUNT) ||
        out_magic == NULL || out_len == NULL) {
        return -EINVAL;
    }
    *out_magic = 0u;
    *out_len   = 0u;

    CosimShmRing *r = &region->rings[ch];

    int64_t deadline = (timeout_ms >= 0) ? now_ms() + timeout_ms : 0;

    for (;;) {
        uint32_t avail = ring_used(r);
        if (avail >= 8u) {
            uint8_t hdr[8];
            ring_peek(r, 0u, hdr, 8u);
            uint32_t magic = be32_load(&hdr[0]);
            uint32_t plen  = be32_load(&hdr[4]);

            if (plen > COSIM_SHM_RING_CAP - 8u) {
                /* Catastrophic — bridge desync. Drain channel and
                 * report. The harness should treat this as fatal. */
                __atomic_store_n(&r->tail,
                                 __atomic_load_n(&r->head, __ATOMIC_ACQUIRE),
                                 __ATOMIC_RELEASE);
                return -EBADMSG;
            }

            if (avail >= 8u + plen) {
                if (plen > cap) return -EMSGSIZE;
                /* Pop the header (we already have it) and the payload. */
                uint8_t scratch[8];
                ring_pop(r, scratch, 8u);
                if (plen > 0u) ring_pop(r, buf, plen);
                *out_magic = magic;
                *out_len   = plen;
                return 0;
            }
        }

        if (timeout_ms == 0) return 0;
        int wait_ms;
        if (timeout_ms < 0) {
            wait_ms = -1;
        } else {
            int64_t left = deadline - now_ms();
            if (left <= 0) return 0;
            wait_ms = (left > 1000) ? 1000 : (int) left;
        }

        /* Park on waiter==1; the producer clears+wakes it. */
        __atomic_store_n(&r->waiter, 1u, __ATOMIC_RELEASE);
        /* Re-check after publishing the wait intent — closes the
         * lost-wakeup race against a producer that pushed between
         * our last ring_used() and the waiter store. */
        if (ring_used(r) >= 8u) {
            __atomic_store_n(&r->waiter, 0u, __ATOMIC_RELEASE);
            continue;
        }
        int rc = futex_wait_word(&r->waiter, 1u, wait_ms);
        __atomic_store_n(&r->waiter, 0u, __ATOMIC_RELEASE);
        if (rc == -ETIMEDOUT) continue;
        if (rc != 0 && rc != -EAGAIN && rc != -EINTR) return rc;
    }
}

/* ---------------------------------------------------------------------------
 * Self-test — exercises the bridge without Verilator. Used in
 * fallback mode so the code is at least kept green.
 *
 * Strategy: create an in-process region under a unique shm name,
 * push a frame on TMA_RX, pop it back, validate magic+payload, tear
 * down. Returns 0 on success, -errno otherwise.
 * ------------------------------------------------------------------------- */
int cosim_shm_selftest(void)
{
    char name[64];
    snprintf(name, sizeof name, "tetra-cosim-selftest-%d", (int) getpid());

    CosimShmRegion *region = NULL;
    int rc = cosim_shm_create(name, &region);
    if (rc != 0) {
        fprintf(stderr, "[shm-bridge selftest] create failed: %d\n", rc);
        return rc;
    }

    static const uint8_t SAMPLE[12] = {
        0x01, 0x41, 0x7F, 0xA7, 0x01, 0x12,
        0x66, 0x34, 0x20, 0xC1, 0x22, 0x60
    };

    rc = cosim_shm_send_frame(region, COSIM_CHAN_TMA_RX,
                              0x544D4153u /* 'TMAS' */,
                              SAMPLE, sizeof SAMPLE, 100);
    if (rc != 0) {
        fprintf(stderr, "[shm-bridge selftest] send failed: %d\n", rc);
        cosim_shm_close(region, true, name);
        return rc;
    }

    uint32_t got_magic = 0u;
    uint8_t  got_buf[64];
    size_t   got_len = 0u;

    rc = cosim_shm_recv_frame(region, COSIM_CHAN_TMA_RX,
                              &got_magic, got_buf, sizeof got_buf,
                              &got_len, 100);
    if (rc != 0) {
        fprintf(stderr, "[shm-bridge selftest] recv failed: %d\n", rc);
        cosim_shm_close(region, true, name);
        return rc;
    }

    if (got_magic != 0x544D4153u || got_len != sizeof SAMPLE ||
        memcmp(got_buf, SAMPLE, sizeof SAMPLE) != 0) {
        fprintf(stderr, "[shm-bridge selftest] mismatch:"
                " magic=%08x len=%zu\n",
                (unsigned) got_magic, got_len);
        cosim_shm_close(region, true, name);
        return -EBADMSG;
    }

    cosim_shm_close(region, true, name);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Standalone main() for the self-test, invoked by `make cosim` in
 * fallback mode. Compiled when -DSHM_BRIDGE_SELFTEST_MAIN is set.
 * ------------------------------------------------------------------------- */
#ifdef SHM_BRIDGE_SELFTEST_MAIN
int main(void)
{
    int rc = cosim_shm_selftest();
    if (rc == 0) {
        fprintf(stdout, "[shm-bridge selftest] PASS\n");
        return 0;
    }
    fprintf(stdout, "[shm-bridge selftest] FAIL rc=%d\n", rc);
    return 1;
}
#endif
