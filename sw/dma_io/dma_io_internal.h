/* sw/dma_io/dma_io_internal.h — internals shared with unit tests.
 *
 * Owned by S1. NOT part of IF_DMA_API_v1. Public API lives in
 * include/tetra/dma_io.h.
 */
#ifndef TETRA_DMA_IO_INTERNAL_H
#define TETRA_DMA_IO_INTERNAL_H

#include "tetra/dma_io.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Test-only stimulus hook for the pipe-mock backend.
 *
 * Writes raw bytes into the channel's pipe — bypassing dma_send_frame's
 * header wrapping — so unit tests can exercise the parser directly with
 * crafted (good or malformed) byte streams. Returns 0 on success or
 * -errno; -ENOSYS when the real-HW backend is in use.
 *
 * Implementation lives only in the !HAVE_XILINX_DMA build of dma_io.c.
 */
int dma_mock_inject(DmaCtx *ctx, DmaChan ch, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_DMA_IO_INTERNAL_H */
