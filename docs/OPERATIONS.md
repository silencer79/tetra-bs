# Operations

## Build

(TBD after Phase 2 implementation. Carry-over from `tetra-zynq-phy`
`scripts/deploy.sh` is starting point.)

Expected commands:

```bash
make rtl       # Vivado batch synth + place + route → build/system.bit
make sw        # Cross-compile SW daemon + WebUI CGI → build/sw/
make tb        # Run all RTL test benches
make sw-test   # Run SW unit tests on host
make cosim     # Verilator co-sim FPGA + SW
make deploy    # Push bitstream + SW to Board #1
```

## Deploy

(TBD. See old project `scripts/deploy.sh` for pattern.)

## WebUI

**Spec to be filled by TODO-B session.** Top-level sections planned:

### Live Status
- All layer states (PHY/LMAC/UMAC/LLC/MLE/MM/CMCE/SNDCP)
- AST live view (currently registered MS)
- Current calls / Group memberships
- RX statistics (sync rate, CRC pass, frame counts)
- TX queues (depth per priority)
- AACH sequence (last N slots)
- FPGA register dump (read-only)

### Subscriber DB
- Profile editor (6 profiles, all fields, Profile 0 read-only)
- Entity editor (256 entries, ISSI/GSSI add/edit/delete)
- Session view (read-only AST snapshot)

### Debug
- PDU trace with hex bits
- AACH timeline
- Slot schedule view
- DMA channel counters
- IRQ counters
- Message-bus tap (live `SapMsg` flow)

### Configuration
- Cell identity (MCC, MNC, CC, LA)
- Frequencies (RX, TX)
- TX power
- Cipher mode
- Scrambler init
- Training sequences
- Slot table (which slot is what)

### Tools
- Manual PDU sender (assemble + transmit one-off PDU)
- Reset buttons (counters / AST / DB)
- Bitstream switch (between #1 normal and special-mode bitstreams)
- Capture trigger (start WAV recording on board)
- Decoder upload (run decode_dl.py on uploaded WAV)

## Operator Procedures

### After SW or RTL Deploy that affects DL signalling

Per memory `feedback_announce_ms_restart`:
**Power-cycle each MTP3550** before observing behaviour. MS otherwise stays
in backoff/failsafe and ignores correct changes.

### Capture a WAV from the board

(TBD — depends on board-side capture tool implementation.)

### Reset Subscriber DB to defaults

WebUI → Subscriber DB → Reset to factory. (Profile 0 always 0x0000_088F.)

## Monitoring

- WebUI Live Status auto-refreshes counters
- `tetra_d` daemon log: `/var/log/tetra_d.log`
- DMA error counters exposed via WebUI debug page
