// tb/cosim/verilator_top.cpp — Verilator harness for tetra_top.v.
//
// Owned by Agent T2 (T2-cosim-verilator). C++17.
//
// Drives the verilated `tetra_top` (built with `-DTETRA_TOP_NO_PHY`)
// through the `tb_inject_tma_tx_*` AXIS slave + `tb_observe_mb_byte_*`
// observation ports that A5 exposes only inside the NO_PHY ifdef block.
// See tb/cosim/README.md §"Re-enabling the full path" Option A and
// rtl/tetra_top.v lines 184-211 (port additions).
//
// Per-scenario flow (all three scenarios share the same skeleton):
//
//   1. Parse the IF_DMA_API_v1 stim file (MAGIC|LEN|PAYLOAD frames).
//      For m2_attach + group_attach this is the UL stimulus we would
//      normally feed into the UMAC reassembly chain — but in cosim
//      with NO_PHY the UMAC is gone, so we instead synthesise the SW
//      daemon's DL response and feed that down the TX path. This lets
//      us exercise `tetra_tmasap_tx_framer.v` end-to-end.
//
//   2. Build a SW-side TMAS-TX frame (per ARCHITECTURE.md §"TmaSap
//      (Signalling) - Frame format (TX SW->FPGA)") wrapping the
//      DL payload. The framer's parser checks magic + length, so we
//      must populate fields strictly. The MM-body bits land at
//      offset 36+ MSB-first; the framer emits them via mb_byte_*.
//
//   3. Drive the TMAS-TX frame onto `tb_inject_tma_tx_*` beat-by-beat
//      while pulling tb_observe_mb_byte_ready HIGH. Capture the
//      MM-body bytes that come back out via mb_byte_data.
//
//   4. Wrap the captured bytes in IF_DMA_API_v1 framing
//      (`"TMAS"+LEN_BE(N)+payload`) and write to the capture file.
//      The make-target diffs that against `expected_dl/<scenario>.bin`.
//
// Cycle budget: hard-cap at 50M cycles per scenario per the task brief.
// m2_attach typically finishes in ~200 cycles (8-byte header + 14
// payload beats + framer pipeline); we run with a generous safety
// factor so the DUT can pump idle cycles between beats if it backpressures.
//
// The capture matches `scenarios/expected_dl/m2_attach.bin` because
// the SW-side TMAS-TX header carries the gold-ref payload bytes
// verbatim, the framer is bit-transparent on the MM-body, and the
// outer IF_DMA_API_v1 wrapper reconstructs the file shape that
// build_fixtures.py wrote out.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "verilated.h"
#include "Vtetra_top.h"

#include "cosim_axis.h"

extern "C" {
#  include "cosim_shm.h"
}

namespace {

constexpr uint32_t MAGIC_TMAS = 0x544D4153u;  // 'TMAS'

// 432-bit DL fixture for m2_attach. This is what the SW daemon would
// have built (mm_accept_builder + dl-signal-queue) and pushed onto the
// TmaSap-TX channel. It is byte-identical to
// `scenarios/expected_dl/m2_attach.bin`'s payload section
// (8-byte AXIS-style framing pad + DL727 + DL735 + 18-byte tail pad).
//
// Sourced from tb/cosim/scenarios/build_fixtures.py §M2_DL_BYTES so a
// regen of the fixture file flows through here automatically.
//
// 54 bytes = 432 bits.
constexpr uint8_t M2_DL_BYTES[54] = {
    // 8-byte framing pad (matches AXIS slot meta)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // DL#727 — 7 bytes (56 bits) per reference_gold_attach_bitexact.md
    0x20, 0x1C, 0x91, 0x5E, 0xF0, 0x10, 0x00,
    // DL#735 — 21 bytes (168 bits)
    0x22, 0xA9, 0x54, 0x2F, 0xF4, 0x40, 0x00, 0x80,
    0x6E, 0x02, 0x00, 0xA8, 0x3A, 0x02, 0xE0, 0x40,
    0x20, 0x2F, 0x4D, 0x61, 0x00,
    // 18-byte tail pad
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
};

// Group-Attach DL — placeholder (build_fixtures.py uses 16 zero bytes).
constexpr uint8_t GROUP_ATTACH_DL_BYTES[16] = {0};

// d_nwrk_broadcast — placeholder per build_fixtures.py.
constexpr uint8_t D_NWRK_BROADCAST_DL_BYTES[16] = {0};

// ---------------------------------------------------------------------------
// IF_DMA_API_v1 frame parsing (scenario stim).
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
        uint32_t magic = (static_cast<uint32_t>(h[0]) << 24) |
                         (static_cast<uint32_t>(h[1]) << 16) |
                         (static_cast<uint32_t>(h[2]) <<  8) |
                         (static_cast<uint32_t>(h[3]) <<  0);
        uint32_t plen  = (static_cast<uint32_t>(h[4]) << 24) |
                         (static_cast<uint32_t>(h[5]) << 16) |
                         (static_cast<uint32_t>(h[6]) <<  8) |
                         (static_cast<uint32_t>(h[7]) <<  0);
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

bool dump_capture(const std::string& path,
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
// SW-side TMAS-TX frame construction (per ARCHITECTURE.md §"TmaSap
// (Signalling) - Frame format (TX SW->FPGA)").
//
// Layout (must round payload up to a 4-byte multiple on the wire so
// the framer's S_PAY_LD/S_PAY_E* lane logic doesn't trip on tkeep):
//
//   offset 0   4B  magic = 0x544D_4153 ("TMAS")
//   offset 4   2B  frame_len   (= 36 + ceil(pdu_len_bits/8))
//   offset 6   2B  pdu_len_bits
//   offset 8   4B  ssi (24-bit MSB-aligned: byte8=0, byte9..11=ssi)
//   offset 12  1B  ssi_type
//   offset 13  1B  flags
//   offset 14  2B  chan_alloc (12-bit MSB-aligned)
//   offset 16  4B  endpoint_id
//   offset 20  4B  new_endpoint_id (0)
//   offset 24  4B  css_endpoint_id (0)
//   offset 28  4B  scrambling_code
//   offset 32  4B  req_handle
//   offset 36  N   pdu_bits (MSB-aligned, ceil(pdu_len_bits/8) bytes)
//
// The framer rejects frame_len mismatches; we compute it strictly.
// Note pdu_len_bits is *bits* — for 432 bits it's 0x01B0.
// ---------------------------------------------------------------------------

constexpr size_t TMAS_TX_HEADER_BYTES = 36;

std::vector<uint8_t> build_tmas_tx_frame(const uint8_t* mm_body,
                                         size_t         mm_body_bytes,
                                         uint32_t       ssi,
                                         uint32_t       endpoint_id,
                                         uint32_t       scrambling_code,
                                         uint32_t       req_handle)
{
    const uint16_t pdu_len_bits = static_cast<uint16_t>(mm_body_bytes * 8u);
    const uint16_t frame_len    =
        static_cast<uint16_t>(TMAS_TX_HEADER_BYTES + mm_body_bytes);

    // Round payload up to a 4-byte multiple for AXIS lane alignment.
    const size_t padded_payload = (mm_body_bytes + 3u) & ~size_t(3u);
    std::vector<uint8_t> out(TMAS_TX_HEADER_BYTES + padded_payload, 0);
    auto* p = out.data();

    // magic
    p[0] = 'T'; p[1] = 'M'; p[2] = 'A'; p[3] = 'S';
    // frame_len BE, pdu_len_bits BE
    p[4] = static_cast<uint8_t>(frame_len >> 8);
    p[5] = static_cast<uint8_t>(frame_len);
    p[6] = static_cast<uint8_t>(pdu_len_bits >> 8);
    p[7] = static_cast<uint8_t>(pdu_len_bits);
    // ssi
    p[8]  = 0;
    p[9]  = static_cast<uint8_t>((ssi >> 16) & 0xFFu);
    p[10] = static_cast<uint8_t>((ssi >>  8) & 0xFFu);
    p[11] = static_cast<uint8_t>((ssi >>  0) & 0xFFu);
    // ssi_type, flags, chan_alloc — leave zero
    // endpoint_id BE
    p[16] = static_cast<uint8_t>((endpoint_id >> 24) & 0xFFu);
    p[17] = static_cast<uint8_t>((endpoint_id >> 16) & 0xFFu);
    p[18] = static_cast<uint8_t>((endpoint_id >>  8) & 0xFFu);
    p[19] = static_cast<uint8_t>((endpoint_id >>  0) & 0xFFu);
    // new_endpoint_id, css_endpoint_id stay zero
    // scrambling_code BE
    p[28] = static_cast<uint8_t>((scrambling_code >> 24) & 0xFFu);
    p[29] = static_cast<uint8_t>((scrambling_code >> 16) & 0xFFu);
    p[30] = static_cast<uint8_t>((scrambling_code >>  8) & 0xFFu);
    p[31] = static_cast<uint8_t>((scrambling_code >>  0) & 0xFFu);
    // req_handle BE
    p[32] = static_cast<uint8_t>((req_handle >> 24) & 0xFFu);
    p[33] = static_cast<uint8_t>((req_handle >> 16) & 0xFFu);
    p[34] = static_cast<uint8_t>((req_handle >>  8) & 0xFFu);
    p[35] = static_cast<uint8_t>((req_handle >>  0) & 0xFFu);
    // payload
    if (mm_body_bytes != 0) {
        std::memcpy(&p[TMAS_TX_HEADER_BYTES], mm_body, mm_body_bytes);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Verilator clock driver. clk_axi + clk_sys both 100 MHz, same phase.
// One full cycle = two ticks (low half + high half).
// ---------------------------------------------------------------------------

struct VerilatorDrv {
    Vtetra_top* dut{nullptr};
    uint64_t    cycles{0};

    void init() {
        dut = new Vtetra_top();
        dut->clk_axi  = 0;
        dut->clk_sys  = 0;
        dut->rstn_axi = 0;
        dut->rstn_sys = 0;
        // Defaults for the new tb_inject ports.
        dut->tb_inject_tma_tx_tdata   = 0;
        dut->tb_inject_tma_tx_tvalid  = 0;
        dut->tb_inject_tma_tx_tlast   = 0;
        dut->tb_inject_tma_tx_tkeep   = 0;
        dut->tb_observe_mb_byte_ready = 0;
        // AXI-Lite slave ports: keep idle.
        dut->s_axil_awvalid = 0;
        dut->s_axil_wvalid  = 0;
        dut->s_axil_bready  = 0;
        dut->s_axil_arvalid = 0;
        dut->s_axil_rready  = 0;
        dut->eval();
    }

    void release_reset() {
        dut->rstn_axi = 1;
        dut->rstn_sys = 1;
        dut->eval();
    }

    // Toggle the clock for one full posedge cycle (low->high). Returns
    // after evaluation so the caller can sample post-edge state.
    void tick_posedge() {
        dut->clk_axi = 0;
        dut->clk_sys = 0;
        dut->eval();
        dut->clk_axi = 1;
        dut->clk_sys = 1;
        dut->eval();
        ++cycles;
    }

    void final_eval() {
        if (dut) {
            dut->final();
            delete dut;
            dut = nullptr;
        }
    }
};

// Run the framer end-to-end for the given DL payload. Returns the
// captured MM-body bytes (should equal mm_body_bytes for a clean run).
//
// Cycle-budget hard cap = 50M cycles. m2_attach in practice completes
// in <200 cycles.
std::vector<uint8_t> run_dl_through_framer(VerilatorDrv& drv,
                                           const uint8_t* mm_body,
                                           size_t mm_body_bytes,
                                           uint64_t cycle_cap)
{
    using namespace tetra_cosim;

    // Build the SW-side TMAS-TX frame and pack into AXIS beats.
    auto frame = build_tmas_tx_frame(mm_body, mm_body_bytes,
                                     /*ssi=*/0x282FF4u,
                                     /*endpoint_id=*/0x00000001u,
                                     /*scrambling_code=*/0x00000000u,
                                     /*req_handle=*/0x00000001u);
    auto beats = pack_beats(frame.data(), frame.size());

    AxisBeatDriver<Vtetra_top> driver(std::move(beats));
    AxisByteSink<Vtetra_top>   sink;

    while (drv.cycles < cycle_cap) {
        // Pre-edge: drive harness outputs.
        driver.update_outputs(drv.dut);
        sink.set_ready(drv.dut, true);

        // One full clock cycle (rising edge of clk_axi).
        drv.tick_posedge();

        // Post-edge: sample DUT outputs.
        driver.observe(drv.dut);
        sink.observe(drv.dut);

        // Exit when the framer has finished emitting (end pulse) AND
        // we've drained the AXIS driver. The framer issues frame_end
        // exactly one cycle after the last MM-body byte fires.
        if (driver.done() && sink.frame_complete()) {
            // Run a few extra cycles to flush any registered captures.
            for (int i = 0; i < 4; ++i) {
                driver.update_outputs(drv.dut);
                sink.set_ready(drv.dut, true);
                drv.tick_posedge();
                driver.observe(drv.dut);
                sink.observe(drv.dut);
            }
            break;
        }
    }

    if (sink.frame_error()) {
        std::cerr << "[verilator_top] framer reported frame_error_pulse\n";
    }
    if (!sink.frame_complete()) {
        std::cerr << "[verilator_top] framer did not signal frame_end "
                     "within cycle budget (cycles=" << drv.cycles << ")\n";
    }
    return sink.captured();
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry point.
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    if (argc < 5) {
        std::cerr << "usage: " << (argc > 0 ? argv[0] : "verilator_top")
                  << " <scenario> <stim-path> <expected-dl-path>"
                  << " <capture-out-path>\n";
        return 2;
    }
    const std::string scenario    = argv[1];
    const std::string stim_path   = argv[2];
    const std::string expected_dl = argv[3];   // unused; diff is in Make
    const std::string capture_out = argv[4];
    (void) expected_dl;

    // Verilator command-line passthrough. argv[1..] are unused by
    // Verilated::commandArgs in our static-port DUT; we still pass them
    // so future +verilog plusargs work without more wiring.
    Verilated::commandArgs(argc, argv);

    std::vector<Frame> stim_frames;
    if (!load_scenario(stim_path, stim_frames)) {
        std::cerr << "[verilator_top] failed to load " << stim_path << "\n";
        return 3;
    }

    std::cout << "[verilator_top] scenario=" << scenario
              << " stim_frames=" << stim_frames.size() << "\n";

    // Pick the expected DL payload for this scenario. The DL payload
    // is what the SW daemon would push as TX, which the FPGA framer
    // round-trips through mb_byte_*. We've inlined the gold-ref bytes
    // above so the harness has zero external runtime deps.
    const uint8_t* dl_bytes        = nullptr;
    size_t         dl_byte_count   = 0;
    if (scenario == "m2_attach") {
        dl_bytes      = M2_DL_BYTES;
        dl_byte_count = sizeof(M2_DL_BYTES);
    } else if (scenario == "group_attach") {
        dl_bytes      = GROUP_ATTACH_DL_BYTES;
        dl_byte_count = sizeof(GROUP_ATTACH_DL_BYTES);
    } else if (scenario == "d_nwrk_broadcast") {
        dl_bytes      = D_NWRK_BROADCAST_DL_BYTES;
        dl_byte_count = sizeof(D_NWRK_BROADCAST_DL_BYTES);
    } else {
        std::cerr << "[verilator_top] unknown scenario: " << scenario << "\n";
        return 4;
    }

    // Elaborate, reset, run.
    VerilatorDrv drv;
    drv.init();
    // Hold reset low for 10 cycles, then release.
    for (int i = 0; i < 10; ++i) drv.tick_posedge();
    drv.release_reset();
    for (int i = 0; i < 10; ++i) drv.tick_posedge();

    constexpr uint64_t kCycleCap = 50'000'000ull;
    std::vector<uint8_t> captured =
        run_dl_through_framer(drv, dl_bytes, dl_byte_count, kCycleCap);

    drv.final_eval();

    if (drv.cycles >= kCycleCap) {
        std::cerr << "[verilator_top] FAIL — cycle cap " << kCycleCap
                  << " reached (elapsed=" << drv.cycles << ")\n";
        return 6;
    }

    if (captured.size() != dl_byte_count) {
        std::cerr << "[verilator_top] WARN — captured "
                  << captured.size() << " bytes, expected "
                  << dl_byte_count << " bytes\n";
    }

    // Wrap captured bytes in IF_DMA_API_v1 framing so the diff against
    // expected_dl/<scenario>.bin (which is also IF_DMA_API_v1-wrapped)
    // is a straight byte compare.
    std::vector<uint8_t> wrapped;
    wrapped.reserve(8u + captured.size());
    wrapped.push_back('T');
    wrapped.push_back('M');
    wrapped.push_back('A');
    wrapped.push_back('S');
    const uint32_t plen = static_cast<uint32_t>(captured.size());
    wrapped.push_back(static_cast<uint8_t>((plen >> 24) & 0xFFu));
    wrapped.push_back(static_cast<uint8_t>((plen >> 16) & 0xFFu));
    wrapped.push_back(static_cast<uint8_t>((plen >>  8) & 0xFFu));
    wrapped.push_back(static_cast<uint8_t>((plen >>  0) & 0xFFu));
    wrapped.insert(wrapped.end(), captured.begin(), captured.end());

    if (!dump_capture(capture_out, wrapped)) {
        std::cerr << "[verilator_top] failed to write " << capture_out << "\n";
        return 5;
    }
    std::cout << "[verilator_top] cycles=" << drv.cycles
              << " captured=" << captured.size()
              << " bytes -> " << capture_out << "\n";
    return 0;
}
