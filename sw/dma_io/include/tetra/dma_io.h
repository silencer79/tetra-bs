/* sw/dma_io/include/tetra/dma_io.h — userspace 4-channel DMA glue.
 *
 * Owned by S1 (S1-sw-dma-glue). Locked under interface contract
 * IF_DMA_API_v1 per docs/MIGRATION_PLAN.md §S1.
 *
 * Wraps the in-tree Xilinx `xilinx_dma.ko` (HARDWARE.md §4) into a
 * 4-channel send/recv API that
 * speaks the FPGA-side TmaSap/TmdSap frame format from
 * docs/ARCHITECTURE.md §"FPGA↔SW Boundary":
 *
 *   +------+------+------+--- ... ---+
 *   |MAGIC | LEN  |   PAYLOAD        |
 *   | 4 B  | 4 B  |   LEN bytes      |
 *   +------+------+------+--- ... ---+
 *
 * MAGIC ∈ { 'TMAS' (signalling), 'TMAR' (UMAC reports), 'TMDC' (voice) }.
 * MAGIC and LEN are big-endian 32-bit words on the wire.
 *
 * The 4 hardware channels (DT labels per
 * dts/tetra_axi_dma_overlay.dtsi from feat/a1-fpga-axi-dma):
 *
 *   DMA_CHAN_TMA_RX  →  "tma-sap-rx"  (FPGA → PS, signalling RX)
 *   DMA_CHAN_TMA_TX  →  "tma-sap-tx"  (PS → FPGA, signalling TX)
 *   DMA_CHAN_TMD_RX  →  "tmd-sap-rx"  (FPGA → PS, voice RX)
 *   DMA_CHAN_TMD_TX  →  "tmd-sap-tx"  (PS → FPGA, voice TX)
 *
 * Self-describing: this header pulls in only stdint/stddef/stdbool. No
 * xilinx_dma symbols are exposed; the implementation gates real-HW vs
 * pipe-mock via HAVE_XILINX_DMA.
 */
#ifndef TETRA_DMA_IO_H
#define TETRA_DMA_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Channel enumeration — locked order. Other agents (S7/T2) cast the
 * enumerand to an array index; do NOT renumber.
 * ------------------------------------------------------------------------- */
typedef enum {
    DMA_CHAN_TMA_RX = 0,
    DMA_CHAN_TMA_TX = 1,
    DMA_CHAN_TMD_RX = 2,
    DMA_CHAN_TMD_TX = 3,
    DMA_CHAN_COUNT  = 4
} DmaChan;

/* True iff ch is a receive (FPGA → PS) channel. */
static inline bool dma_chan_is_rx(DmaChan ch)
{
    return ch == DMA_CHAN_TMA_RX || ch == DMA_CHAN_TMD_RX;
}

/* ---------------------------------------------------------------------------
 * Frame magics — big-endian 4-byte tags on the wire. The numeric values
 * below are the host-order uint32_t representation, matching the
 * "Frame format" section of docs/ARCHITECTURE.md. ARCHITECTURE.md and
 * the FPGA framers (A2/A3) use the identical constants.
 *
 *   'T''M''A''S'  → 0x544D4153
 *   'T''M''A''R'  → 0x544D4152
 *   'T''M''D''C'  → 0x544D4443
 * ------------------------------------------------------------------------- */
typedef enum {
    FRAME_MAGIC_TMAS = 0x544D4153u,
    FRAME_MAGIC_TMAR = 0x544D4152u,
    FRAME_MAGIC_TMDC = 0x544D4443u
} FrameMagic;

/* True iff magic is one of the three known frame types. */
bool frame_magic_is_known(uint32_t magic);

/* ---------------------------------------------------------------------------
 * Header parser. Returns 0 on success and writes *out_magic + *out_payload_len
 * (the LEN field, in bytes — does NOT include the 8-byte header).
 *
 * Returns -EBADMSG if buf is shorter than 8 bytes, magic unknown, or
 * declared payload would exceed the supplied buffer. -EINVAL if buf
 * or out_* pointers are NULL.
 *
 * Implementation reads buf[0..7] big-endian. No state, no I/O.
 * ------------------------------------------------------------------------- */
int frame_parse_header(const uint8_t *buf,
                       size_t          len,
                       FrameMagic     *out_magic,
                       uint32_t       *out_payload_len);

/* Header-on-the-wire size = magic(4) + len(4). */
#define DMA_FRAME_HEADER_BYTES 8u

/* Per-channel maximum payload. Worst case is TmaSap signalling at one
 * 132-bit MM body + meta; TmdSap voice is 274-bit ACELP MSB-aligned
 * (44 bytes incl. magic+len). 4 KiB is a safe, comfortable upper
 * bound and matches the xilinx_dma SG buffer slot size. */
#define DMA_FRAME_MAX_PAYLOAD 4096u

/* ---------------------------------------------------------------------------
 * Configuration + opaque context handle. Caller allocates DmaCtx; init
 * fills it. Default DmaCfg (all zero) wires up the production paths
 * with the DT names locked in IF_AXIDMA_v1.
 * ------------------------------------------------------------------------- */
typedef struct {
    /* Optional: override per-channel device path (NULL → DT-default).
     * Order matches DmaChan enum; index by (int) DmaChan. */
    const char *chan_dev_paths[DMA_CHAN_COUNT];

    /* Optional: per-channel DT label override. NULL → use the default
     * "tma-sap-rx" / "tma-sap-tx" / "tmd-sap-rx" / "tmd-sap-tx". */
    const char *chan_dt_labels[DMA_CHAN_COUNT];

    /* If true, dma_init must NOT touch real HW (xilinx_dma) — used by
     * unit tests and the host smoke build. The pipe-mock backend
     * always runs in mock mode. */
    bool force_mock;
} DmaCfg;

/* DmaCtx — exposed but treat-as-opaque. Declared in the public header
 * so callers can stack-allocate without dynamic memory; field access
 * MUST go through the dma_*() API. The internal layout (pipe FDs,
 * xilinx_dma handles, reassembly buffer) is implementation-private and
 * may change without bumping IF_DMA_API_v1.
 *
 * Per-channel reassembly buffer sized to one max-payload frame plus
 * the 8-byte header — that is the maximum we may carry across a recv
 * call when the read straddled a frame boundary. */
#define DMA_REASM_CAP_BYTES (8u + DMA_FRAME_MAX_PAYLOAD)

typedef struct {
    /* opaque — do not access. */
    int      _pipe_rd;
    int      _pipe_wr;
    int      _axidma_chan_id;
    void    *_axidma_buf;
    size_t   _axidma_buf_cap;
    uint8_t  _reasm[DMA_REASM_CAP_BYTES];
    size_t   _reasm_used;
    bool     _opened;
    /* DmaStats fields inlined to avoid pulling stats struct above. */
    uint64_t _frames_recv_ok;
    uint64_t _frames_send_ok;
    uint64_t _bytes_recv;
    uint64_t _bytes_send;
    uint64_t _drop_bad_magic;
    uint64_t _drop_bad_length;
    uint64_t _drop_short_read;
} DmaChanState_;

typedef struct DmaCtx {
    DmaChanState_ _chans[DMA_CHAN_COUNT];
    bool          _using_mock;
} DmaCtx;

/* ---------------------------------------------------------------------------
 * Lifecycle — IF_DMA_API_v1.
 *
 *   dma_init        Acquire the 4 channels. Returns 0 on success,
 *                   -errno on first failure (any partially-opened
 *                   channels are closed before returning).
 *
 *   dma_close       Release all 4 channels. Idempotent; safe to call
 *                   on a zero-initialised ctx (no-op).
 *
 *   dma_recv_frame  Block up to timeout_ms reading one frame from ch.
 *                   Returns the payload length (>0) on success and
 *                   writes it to *out_len. Returns 0 on timeout-with-
 *                   no-data. Returns -EBADMSG on bad magic / length-
 *                   overflow / partial-read EOF; ctx-level drop_count
 *                   is incremented and the rest of the channel buffer
 *                   is preserved so the caller can re-sync on the next
 *                   call. timeout_ms == 0 → poll-once, never block.
 *                   timeout_ms < 0 → block forever (until data or
 *                   error). The function will NOT return half-frames:
 *                   it reassembles across short reads internally.
 *
 *   dma_send_frame  Wrap (buf, len) in a magic+length header derived
 *                   from ch (TMA_TX → 'TMAS', TMD_TX → 'TMDC'; RX
 *                   channels reject with -EINVAL — the FPGA owns the
 *                   RX-direction magics). Returns 0 on success or
 *                   -errno.
 *
 *   dma_get_irq_fd  File descriptor the daemon main loop polls for
 *                   epoll/poll wakeup on this channel. Real-HW
 *                   backend: char-dev FD from xilinx_dma. Mock
 *                   backend: read-end of the per-channel pipe (read
 *                   readiness ≡ frame ready). Returns -EINVAL on
 *                   bad ch; otherwise the FD (>=0) — caller must NOT
 *                   close it; lifetime is tied to ctx.
 * ------------------------------------------------------------------------- */
int  dma_init(DmaCtx *ctx, const DmaCfg *cfg);
int  dma_recv_frame(DmaCtx *ctx,
                    DmaChan ch,
                    uint8_t *buf,
                    size_t   cap,
                    size_t  *out_len,
                    int      timeout_ms);
int  dma_send_frame(DmaCtx       *ctx,
                    DmaChan       ch,
                    const uint8_t *buf,
                    size_t        len);
int  dma_get_irq_fd(DmaCtx *ctx, DmaChan ch);
void dma_close(DmaCtx *ctx);

/* ---------------------------------------------------------------------------
 * Diagnostics — read-only. Owned by S1; surfaced to WebUI/debug CGIs
 * via the daemon (S7).
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t frames_recv_ok;
    uint64_t frames_send_ok;
    uint64_t bytes_recv;
    uint64_t bytes_send;
    uint64_t drop_bad_magic;
    uint64_t drop_bad_length;
    uint64_t drop_short_read;
} DmaStats;

int dma_get_stats(const DmaCtx *ctx, DmaChan ch, DmaStats *out);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_DMA_IO_H */
