/* tb/cosim/include/cosim_shm.h — shared-memory DMA bridge for T2 co-sim.
 *
 * Owned by Agent T2 (T2-cosim-verilator). Defines the API used by
 *   - tb/cosim/verilator_top.cpp  (FPGA-side, C++17)
 *   - tb/cosim/shm_dma_bridge.c   (implementation, C11)
 *   - sw/dma_io/dma_io.c          (eventually, gated by HAVE_COSIM_SHM)
 *
 * Layout: four POSIX shm rings, one per IF_DMA_API_v1 channel
 * (TMA_RX, TMA_TX, TMD_RX, TMD_TX). Each ring is a power-of-two
 * SPSC byte buffer with futex-based wake on empty/full transitions.
 * Producers write at the head; consumers read at the tail.
 *
 * Wire format inside the rings is the IF_DMA_API_v1 frame format
 *
 *   +------+------+------+--- ... ---+
 *   |MAGIC | LEN  |   PAYLOAD        |
 *   | 4 B  | 4 B  |   LEN bytes      |
 *   +------+------+------+--- ... ---+
 *
 * MAGIC big-endian ASCII tag ('TMAS', 'TMAR', 'TMDC').
 * LEN big-endian payload byte count.
 *
 * That matches sw/dma_io/dma_io.c's pipe-mock byte stream so the
 * daemon binary sees the same shape regardless of backend.
 *
 * The verilator harness translates between this format and the
 * 36-byte structured TMAS frame the FPGA framers
 * (rtl/infra/tetra_tmasap_*_framer.v) emit/consume, since they are
 * NOT the same wire (see README.md §"Re-enabling the full path"
 * Option A).
 */
#ifndef TETRA_COSIM_SHM_H
#define TETRA_COSIM_SHM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ring capacity: 64 KiB per channel — comfortable for a 4 KiB max
 * payload + 8-byte header times whatever burst we expect from the
 * gold-ref scenarios (deepest is M2-Attach with two UL frags + one
 * DL pair, all under 1 KiB total). Power-of-two so head/tail wraps
 * are just bitmask ops. */
#define COSIM_SHM_RING_CAP 65536u

/* Channel enumeration — value-equal to IF_DMA_API_v1 DmaChan.
 * Kept distinct so this header has no dependency on
 * sw/dma_io/include/tetra/dma_io.h (lets T2 build standalone). */
typedef enum {
    COSIM_CHAN_TMA_RX = 0,
    COSIM_CHAN_TMA_TX = 1,
    COSIM_CHAN_TMD_RX = 2,
    COSIM_CHAN_TMD_TX = 3,
    COSIM_CHAN_COUNT  = 4
} CosimChan;

/* One ring. Mapped at the same virtual address in both processes
 * via shm_open + mmap. head/tail are uint32_t for cheap atomic load/
 * store on x86-64; futex sync uses a 32-bit waiter word. */
typedef struct {
    /* Cache-line padding helps reduce false sharing between the
     * producer and consumer cores; not load-bearing for correctness. */
    volatile uint32_t head;          /* writer cursor (bytes since shm open) */
    uint32_t          _pad_head[15];
    volatile uint32_t tail;          /* reader cursor (bytes since shm open) */
    uint32_t          _pad_tail[15];
    volatile uint32_t waiter;        /* futex word: 0=idle, 1=blocked-on-not-empty */
    uint32_t          _pad_waiter[15];
    uint8_t           buf[COSIM_SHM_RING_CAP];
} CosimShmRing;

/* Top-level shm region — four rings, one per channel. Mapped from
 * /dev/shm/tetra-cosim-<pid> (created by the verilator side, opened
 * read/write by both). */
typedef struct {
    uint32_t      magic;             /* "CSHM" 0x4353_484D */
    uint32_t      version;           /* IF_COSIM_SHM_v1 = 1 */
    uint32_t      pid_verilator;     /* set by harness, read by daemon */
    uint32_t      pid_daemon;        /* set by daemon, read by harness */
    CosimShmRing  rings[COSIM_CHAN_COUNT];
} CosimShmRegion;

#define COSIM_SHM_MAGIC   0x4353484Du   /* 'CSHM' big-endian = 0x4353484D */
#define COSIM_SHM_VERSION 1u

/* ---------------------------------------------------------------------------
 * Lifecycle.
 * ------------------------------------------------------------------------- */

/* Create + map the shm region. shm_path is e.g. "tetra-cosim-12345".
 * On success returns 0 and sets *out_region to the mapped pointer;
 * the underlying fd is closed (mmap survives). On failure returns
 * -errno. The verilator harness calls cosim_shm_create; the daemon
 * calls cosim_shm_attach. */
int cosim_shm_create(const char *shm_path, CosimShmRegion **out_region);
int cosim_shm_attach(const char *shm_path, CosimShmRegion **out_region);

/* Tear down — unmap + (creator only) shm_unlink. Idempotent. */
void cosim_shm_close(CosimShmRegion *region, bool unlink_path,
                     const char *shm_path);

/* ---------------------------------------------------------------------------
 * Frame I/O.
 *
 * cosim_shm_send_frame  — writes magic+len+payload onto ch's ring.
 *                         Blocks if the ring is full and timeout_ms<0,
 *                         polls if 0, otherwise waits up to timeout_ms.
 *                         Returns 0 on success or -errno (-ETIMEDOUT,
 *                         -EINVAL, -ENOSPC).
 *
 * cosim_shm_recv_frame  — reads one frame off ch's ring. Returns the
 *                         payload byte length in *out_len on success,
 *                         0 on timeout-with-no-data. -EBADMSG on bad
 *                         magic.
 *
 * Both functions are intended for SPSC use per channel; concurrent
 * writers (or concurrent readers) on the same ring are not supported.
 * For T2's geometry that is fine: harness writes RX channels + reads
 * TX channels; daemon does the inverse.
 * ------------------------------------------------------------------------- */
int cosim_shm_send_frame(CosimShmRegion *region,
                         CosimChan       ch,
                         uint32_t        magic,
                         const uint8_t  *payload,
                         uint32_t        payload_len,
                         int             timeout_ms);

int cosim_shm_recv_frame(CosimShmRegion *region,
                         CosimChan       ch,
                         uint32_t       *out_magic,
                         uint8_t        *buf,
                         size_t          cap,
                         size_t         *out_len,
                         int             timeout_ms);

/* ---------------------------------------------------------------------------
 * Self-test entry point used in fallback mode (no verilator).
 * Loops a handful of frames through one ring locally and asserts the
 * round-trip integrity. Used by `make cosim` so the bridge is at
 * least exercised in CI even when Verilator is missing.
 * ------------------------------------------------------------------------- */
int cosim_shm_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_COSIM_SHM_H */
