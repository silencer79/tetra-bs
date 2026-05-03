// tb/cosim/verilator_top.cpp — Verilator harness for tetra_top.v.
//
// Owned by Agent T2 (T2-cosim-verilator). C++17 because Verilator's
// generated headers require C++ — this file is the *only* C++ file in
// the project (CLAUDE.md §Languages still locks the rest of the SW
// stack to C11).
//
// Responsibilities:
//   1. Elaborate Verilated rtl/tetra_top.v (`Vtetra_top`).
//   2. Drive `clk_axi` + `clk_sys` (both 100 MHz baseline; A4 CDC
//      handles the future split).
//   3. Bridge the four AXIS streams that the AXI-DMA wrapper would
//      normally read/write to PS DDR onto the shared-memory rings
//      defined by tb/cosim/include/cosim_shm.h.
//   4. Translate between the FPGA-side 36-byte structured TMAS frame
//      (per ARCHITECTURE.md §"TmaSap (Signalling) - Frame format") and
//      the IF_DMA_API_v1 8-byte-header format that the daemon's
//      sw/dma_io/dma_io.c speaks. The daemon stays unchanged; the
//      verilator harness is the only place that knows about the
//      36-byte layout. README.md §"Re-enabling the full path" Option A.
//
// Status: this file compiles *only* when Verilator is installed
// (`apt install verilator`, candidate 5.020-1 per HARDWARE.md §6).
// In fallback mode (no verilator) the file is excluded from the
// build by tb/cosim/Makefile.
//
// The structure here is deliberately conservative — read scenario
// fixtures, push UL frames into the harness-side ring, run the
// simulator clock-by-clock, capture DL bytes from the TX-side AXIS
// streams, write them out to a `build/<scenario>.dl.bin` file for
// the make target's bit-diff step. No timing-detail fidelity beyond
// what `tetra_top.v` itself models.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if !defined(VERILATOR_NOT_AVAILABLE)
// These headers exist only when Verilator generated `obj_dir/Vtetra_top.h`.
// The Makefile wraps `verilator` invocation so by the time this TU is
// compiled they must be on the include path.
#  include "verilated.h"
#  include "Vtetra_top.h"
#endif

extern "C" {
#  include "cosim_shm.h"
}

namespace {

constexpr uint32_t MAGIC_TMAS = 0x544D4153u;  // 'TMAS'
constexpr uint32_t MAGIC_TMAR = 0x544D4152u;  // 'TMAR'
constexpr uint32_t MAGIC_TMDC = 0x544D4443u;  // 'TMDC'

// ---------------------------------------------------------------------------
// Scenario file format (see scenarios/README within the binaries):
//   raw concatenation of IF_DMA_API_v1 frames — each frame is
//   MAGIC(4) + LEN_BE(4) + LEN bytes. The harness pushes those onto
//   the appropriate channel (TMA_RX for signalling UL stimulus).
// ---------------------------------------------------------------------------

struct Frame {
    uint32_t              magic{0u};
    std::vector<uint8_t>  payload;
};

bool load_scenario(const std::string& path, std::vector<Frame>& frames)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "[verilator_top] cannot open scenario: " << path << "\n";
        return false;
    }
    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());

    size_t off = 0;
    while (off + 8 <= bytes.size()) {
        const uint8_t* h = &bytes[off];
        uint32_t magic = ((uint32_t) h[0] << 24) | ((uint32_t) h[1] << 16) |
                         ((uint32_t) h[2] <<  8) | ((uint32_t) h[3] <<  0);
        uint32_t plen  = ((uint32_t) h[4] << 24) | ((uint32_t) h[5] << 16) |
                         ((uint32_t) h[6] <<  8) | ((uint32_t) h[7] <<  0);
        if (off + 8 + plen > bytes.size()) {
            std::cerr << "[verilator_top] truncated frame at off=" << off
                      << " plen=" << plen << "\n";
            return false;
        }
        Frame fr;
        fr.magic = magic;
        fr.payload.assign(&bytes[off + 8], &bytes[off + 8 + plen]);
        frames.push_back(std::move(fr));
        off += 8 + plen;
    }
    return off == bytes.size();
}

[[maybe_unused]] bool dump_capture(const std::string& path,
                                   const std::vector<uint8_t>& bytes)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!bytes.empty()) {
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    return f.good();
}

// ---------------------------------------------------------------------------
// 36-byte TMAS structured-header layout per ARCHITECTURE.md §"TmaSap
// (Signalling) - Frame format". This is what the FPGA framers
// (rtl/infra/tetra_tmasap_*_framer.v) emit/consume on the AXIS bus.
// We translate between this and the 8-byte IF_DMA_API_v1 header
// (`MAGIC | LEN_BE | PAYLOAD`).
//
// Translation policy: when the harness pushes UL stimulus, it reads
// IF_DMA_API_v1 frames out of the scenario fixture and re-emits a
// 36-byte TMAS-headered AXIS burst into the verilated FPGA. When DL
// bytes flow out of the FPGA's TMAS-TX framer, the harness strips the
// 36-byte header and re-emits IF_DMA_API_v1 frames to the daemon.
//
// For the structural-only fallback scenarios (no daemon in loop),
// we just propagate the IF_DMA_API_v1 frame unchanged for visual
// inspection — the bit-diff step is done at the IF_DMA_API_v1 layer.
// ---------------------------------------------------------------------------

constexpr size_t TMAS_HEADER_BYTES = 36;

[[maybe_unused]] void wrap_tmas36(const Frame& src,
                                  std::vector<uint8_t>& out_axis_bytes,
                                  uint32_t ssi, uint32_t endpoint_id)
{
    // Build a 36-byte TMAS header + payload. Field positions per
    // ARCHITECTURE.md.  This is "best-shape" — we don't try to drive
    // ssi_type / scrambling_code / chan_alloc precisely because the
    // structural fallback doesn't exercise the FPGA framer's parser.
    const uint16_t pdu_bits  = static_cast<uint16_t>(src.payload.size() * 8u);
    const uint16_t frame_len = static_cast<uint16_t>(TMAS_HEADER_BYTES +
                                                     src.payload.size());

    out_axis_bytes.clear();
    out_axis_bytes.resize(frame_len, 0);
    auto* p = out_axis_bytes.data();

    // magic
    p[0] = 'T'; p[1] = 'M'; p[2] = 'A'; p[3] = 'S';
    // frame_len, pdu_len_bits — big-endian uint16
    p[4] = static_cast<uint8_t>(frame_len >> 8);
    p[5] = static_cast<uint8_t>(frame_len);
    p[6] = static_cast<uint8_t>(pdu_bits >> 8);
    p[7] = static_cast<uint8_t>(pdu_bits);
    // ssi at offset 8 (24-bit MSB-aligned: byte 8 = 0, 9..11 = ssi)
    p[8]  = 0;
    p[9]  = static_cast<uint8_t>((ssi >> 16) & 0xFFu);
    p[10] = static_cast<uint8_t>((ssi >>  8) & 0xFFu);
    p[11] = static_cast<uint8_t>((ssi >>  0) & 0xFFu);
    // ssi_type/flags/reserved already zero
    // endpoint_id at offset 16
    p[16] = static_cast<uint8_t>((endpoint_id >> 24) & 0xFFu);
    p[17] = static_cast<uint8_t>((endpoint_id >> 16) & 0xFFu);
    p[18] = static_cast<uint8_t>((endpoint_id >>  8) & 0xFFu);
    p[19] = static_cast<uint8_t>((endpoint_id >>  0) & 0xFFu);
    // pdu payload at offset 36 (TMAS_HEADER_BYTES)
    if (!src.payload.empty()) {
        std::memcpy(&p[TMAS_HEADER_BYTES], src.payload.data(),
                    src.payload.size());
    }
}

#if !defined(VERILATOR_NOT_AVAILABLE)
// ---------------------------------------------------------------------------
// Verilator clock driver. tetra_top has clk_axi + clk_sys on
// independent ports — our baseline runs both at 100 MHz with the same
// edge phase (A4 CDC FIFOs handle the future async split).
//
// One simulated tick = one half-period; we toggle both clocks every
// other tick. Reset is held low for the first 8 ticks then released.
// ---------------------------------------------------------------------------

struct VerilatorDrv {
    Vtetra_top* dut{nullptr};
    uint64_t    sim_time{0};   // in nanoseconds; 5 ns per half-period

    void init(int argc, char** argv)
    {
        Verilated::commandArgs(argc, argv);
        dut = new Vtetra_top();
        dut->clk_axi  = 0;
        dut->clk_sys  = 0;
        dut->rstn_axi = 0;
        dut->rstn_sys = 0;
        dut->eval();
    }

    void release_reset()
    {
        dut->rstn_axi = 1;
        dut->rstn_sys = 1;
    }

    void tick(int n = 1)
    {
        for (int i = 0; i < n; ++i) {
            dut->clk_axi = !dut->clk_axi;
            dut->clk_sys = !dut->clk_sys;
            dut->eval();
            sim_time += 5;  // 5 ns per half-period (100 MHz clock)
        }
    }

    void final_eval()
    {
        if (dut) {
            dut->final();
            delete dut;
            dut = nullptr;
        }
    }
};
#endif  // !VERILATOR_NOT_AVAILABLE

}  // namespace

// ---------------------------------------------------------------------------
// Entry point. Argument shape: `verilator_top <scenario-name> <stim-path>
// <expected-dl-path> <capture-out-path>`.  The make target supplies the
// paths so this binary is location-independent.
//
// Fallback (no Verilator): we still parse the scenario file and write
// it back out as the capture so the make target can run a structural
// `cmp` and report the diff size. The capture is intentionally NOT
// equal to the expected DL — the make target prints a "deferred"
// banner instead of PASS/FAIL when the capture is the trivial pass-
// through.
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    if (argc < 5) {
        std::cerr << "usage: " << (argc > 0 ? argv[0] : "verilator_top")
                  << " <scenario> <stim-path> <expected-dl-path>"
                  << " <capture-out-path>\n";
        return 2;
    }
    const std::string scenario        = argv[1];
    const std::string stim_path       = argv[2];
    const std::string expected_dl     = argv[3];  // unused here; diff is in Make
    const std::string capture_out     = argv[4];
    (void) expected_dl;

    std::vector<Frame> stim_frames;
    if (!load_scenario(stim_path, stim_frames)) {
        std::cerr << "[verilator_top] failed to load " << stim_path << "\n";
        return 3;
    }

    std::cout << "[verilator_top] scenario=" << scenario
              << " stim_frames=" << stim_frames.size() << "\n";

#if defined(VERILATOR_NOT_AVAILABLE)
    // Build excluded this from the verilator path. Should never run.
    std::cerr << "[verilator_top] built without verilator - logic error\n";
    (void) capture_out;
    return 4;
#else
    // Real path: drive the Verilated DUT and capture DL bytes.
    // The implementation here is the *skeleton* — see README.md
    // §"Re-enabling the full path" for the work-back checklist that
    // turns it into a true bit-exact harness. What is in place:
    //   - DUT elaboration, reset release, clock toggling.
    //   - Scenario parsing into Frame{} structs.
    //   - 36-byte TMAS-header wrapper for the UL stimulus side.
    //   - Capture buffer + write-out to capture_out.
    //
    // What still needs wiring (Phase-4 fold-back):
    //   - AXIS slave driver onto m_axis_tma_rx_*.
    //   - AXIS master sink off m_axis_tma_tx_* into IF_DMA_API_v1
    //     frames (strip the 36-byte TMAS header).
    //   - shm bridge integration so a real tetra_d binary picks up
    //     the captured frames as RX events and replies with TX frames.
    //   - Cycle budget per scenario (currently a fixed 1e6-tick run).

    VerilatorDrv drv;
    drv.init(argc, argv);

    // Hold reset low for 8 ticks (4 cycles).
    drv.tick(8);
    drv.release_reset();
    drv.tick(8);

    // Push UL stimulus into the AXIS-RX path. Skeleton: just record
    // the wrapped payload to the capture buffer so the make target
    // can confirm the harness saw the stimulus end-to-end. Replace
    // with a real AXIS driver once verilator + the DUT-port-level
    // bindings are wired (Phase 4 fold-back).
    std::vector<uint8_t> capture;
    for (const auto& fr : stim_frames) {
        std::vector<uint8_t> wrapped;
        wrap_tmas36(fr, wrapped, /*ssi=*/0x282FF4u, /*endpoint_id=*/0u);
        capture.insert(capture.end(), wrapped.begin(), wrapped.end());
    }

    // Run the DUT for a fixed budget. d_nwrk_broadcast wants ~10 s of
    // simulated time (= 2e8 100MHz cycles); the others finish much
    // sooner. Keep the budget bounded so a wedge in the DUT doesn't
    // hang CI forever.
    constexpr int kTickBudget = 1'000'000;
    drv.tick(kTickBudget);

    drv.final_eval();

    if (!dump_capture(capture_out, capture)) {
        std::cerr << "[verilator_top] failed to write " << capture_out << "\n";
        return 5;
    }
    std::cout << "[verilator_top] captured " << capture.size()
              << " bytes -> " << capture_out << "\n";
    return 0;
#endif
}
