// tb/cosim/include/cosim_axis.h — header-only AXIS driver/sink helpers
// for the Verilator co-sim harness.
//
// Owned by Agent T2 (T2-cosim-verilator). C++17.
//
// The harness drives a small number of AXIS slaves on the verilated
// `tetra_top` DUT (in NO_PHY/cosim mode). This header provides two
// templates so the driving code in verilator_top.cpp stays readable
// across the three scenarios (m2_attach, group_attach, d_nwrk_broadcast).
//
//   AxisBeatDriver   — pushes a sequence of 32-bit beats with tlast +
//                      tkeep onto a slave port. Honours tready
//                      backpressure beat-by-beat. The DUT is single-
//                      stepped through the helper's `tick()` callback so
//                      ticking remains in caller control (we don't pull
//                      Verilator into the helper).
//
//   AxisByteSink     — captures a per-byte stream gated by valid+ready.
//                      The harness pulls `ready` HIGH each cycle and
//                      records every cycle where `valid` is HIGH after
//                      the DUT eval — this matches how the FPGA framer
//                      emits MM-body bytes (see
//                      rtl/infra/tetra_tmasap_tx_framer.v §S_PAY_E*).
//
// Usage pattern (m2_attach):
//
//     AxisBeatDriver drv(beats);
//     AxisByteSink   sink;
//     while (!drv.done() || !sink.frame_complete()) {
//         drv.update_outputs(dut);
//         sink.set_ready(dut, /*ready=*/1);
//         dut->eval();
//         drv.observe(dut);     // captures tready -> advances drv
//         sink.observe(dut);    // captures byte if valid this cycle
//         tick_clock_edge(dut); // toggle clk_axi
//         dut->eval();
//     }
//
// Both helpers are deliberately minimal — there is no global tick
// budget, no timeout handling, no hierarchical assertions. Those live
// in the harness main() so the helpers stay reusable across scenarios.

#ifndef TETRA_COSIM_AXIS_H
#define TETRA_COSIM_AXIS_H

#include <cstdint>
#include <cstddef>
#include <vector>

namespace tetra_cosim {

// One AXIS beat: 32-bit data, 4-bit tkeep, 1-bit tlast.
struct AxisBeat {
    uint32_t tdata{0u};
    uint8_t  tkeep{0xFu};   // 4-bit; default = full word
    bool     tlast{false};
};

// Pack `bytes` into MSB-first 32-bit beats. The last beat carries
// tlast=1 and tkeep masks any unused trailing lanes (we always pad
// to a 4-byte boundary with zeros so tkeep is 4'b1111 except in the
// truncated-tail case).
inline std::vector<AxisBeat> pack_beats(const uint8_t* bytes, size_t n)
{
    std::vector<AxisBeat> out;
    if (n == 0) return out;
    const size_t full_beats = n / 4;
    const size_t tail       = n % 4;
    out.reserve(full_beats + (tail ? 1u : 0u));

    for (size_t i = 0; i < full_beats; ++i) {
        AxisBeat b;
        b.tdata =
            (static_cast<uint32_t>(bytes[4*i+0]) << 24) |
            (static_cast<uint32_t>(bytes[4*i+1]) << 16) |
            (static_cast<uint32_t>(bytes[4*i+2]) <<  8) |
            (static_cast<uint32_t>(bytes[4*i+3]) <<  0);
        b.tkeep = 0xF;
        b.tlast = (tail == 0u) && (i + 1 == full_beats);
        out.push_back(b);
    }
    if (tail) {
        AxisBeat b;
        uint32_t w = 0u;
        uint8_t  k = 0u;
        for (size_t j = 0; j < tail; ++j) {
            w |= static_cast<uint32_t>(bytes[full_beats*4 + j])
                 << (24 - 8 * static_cast<int>(j));
            k |= static_cast<uint8_t>(1u << (3 - j));
        }
        b.tdata = w;
        b.tkeep = k;
        b.tlast = true;
        out.push_back(b);
    }
    return out;
}

// Drives a slave AXIS port. Caller calls `update_outputs(dut)` BEFORE
// dut->eval(), then `observe(dut)` AFTER eval() to advance on tready.
template <class Dut>
class AxisBeatDriver {
public:
    AxisBeatDriver() = default;
    explicit AxisBeatDriver(std::vector<AxisBeat> beats)
        : beats_(std::move(beats))
    {}

    void load(std::vector<AxisBeat> beats) {
        beats_ = std::move(beats);
        idx_ = 0;
    }

    bool done() const { return idx_ >= beats_.size(); }

    // Drive tdata/tvalid/tlast/tkeep onto the DUT. The port set is
    // hard-coded to the tb_inject_tma_tx_* pinout — we only inject on
    // that port today; if the cosim grows other slaves we'd templatise
    // the field bindings (or just write a sibling driver).
    void update_outputs(Dut* dut) {
        if (done()) {
            dut->tb_inject_tma_tx_tvalid = 0;
            dut->tb_inject_tma_tx_tdata  = 0;
            dut->tb_inject_tma_tx_tlast  = 0;
            dut->tb_inject_tma_tx_tkeep  = 0;
            return;
        }
        const AxisBeat& b = beats_[idx_];
        dut->tb_inject_tma_tx_tvalid = 1;
        dut->tb_inject_tma_tx_tdata  = b.tdata;
        dut->tb_inject_tma_tx_tlast  = b.tlast ? 1 : 0;
        dut->tb_inject_tma_tx_tkeep  = b.tkeep;
    }

    // Sample tready post-eval; advance on a beat-fire (valid && ready).
    // Returns true if a beat fired this cycle.
    bool observe(Dut* dut) {
        if (done()) return false;
        const bool valid = (dut->tb_inject_tma_tx_tvalid != 0);
        const bool ready = (dut->tb_inject_tma_tx_tready != 0);
        if (valid && ready) {
            ++idx_;
            return true;
        }
        return false;
    }

    size_t pending() const { return beats_.size() - idx_; }

private:
    std::vector<AxisBeat> beats_;
    size_t idx_{0};
};

// Captures the framer's per-byte MM-body output. We pull `ready` HIGH
// every cycle and record bytes on (valid && ready). frame_end_pulse and
// frame_error_pulse let main() know when the framer signals completion.
template <class Dut>
class AxisByteSink {
public:
    void set_ready(Dut* dut, bool r) const {
        dut->tb_observe_mb_byte_ready = r ? 1 : 0;
    }

    void observe(Dut* dut) {
        const bool valid = (dut->tb_observe_mb_byte_valid != 0);
        const bool ready = (dut->tb_observe_mb_byte_ready != 0);
        if (valid && ready) {
            captured_.push_back(static_cast<uint8_t>(
                dut->tb_observe_mb_byte_data & 0xFFu));
        }
        if (dut->tb_observe_mb_frame_start_pulse) seen_start_ = true;
        if (dut->tb_observe_mb_frame_end_pulse)   seen_end_   = true;
        if (dut->tb_observe_mb_frame_error_pulse) seen_error_ = true;
    }

    bool frame_complete() const { return seen_end_ || seen_error_; }
    bool frame_error()    const { return seen_error_; }
    bool frame_started()  const { return seen_start_; }

    const std::vector<uint8_t>& captured() const { return captured_; }
    void reset() {
        captured_.clear();
        seen_start_ = false;
        seen_end_   = false;
        seen_error_ = false;
    }

private:
    std::vector<uint8_t> captured_;
    bool seen_start_{false};
    bool seen_end_{false};
    bool seen_error_{false};
};

}  // namespace tetra_cosim

#endif  // TETRA_COSIM_AXIS_H
