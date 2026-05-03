# wavs/

Gold-Reference WAV captures for bit-exact verification.

**NOT in git** (large files, see `.gitignore`).

## Required files

These files are required for test verification. Copy from sister project
`tetra-zynq-phy`:

```bash
cp /home/kevin/claude-ralph/tetra/wavs/gold_standard_380-393mhz/GOLD_DL_ANMELDUNG_GRUPPENWECHSEL_GRUPPENRUF.wav .
cp /home/kevin/claude-ralph/tetra/wavs/gold_standard_380-393mhz/GOLD_UL_ANMELDUNG_GRUPPENWECHSEL_GRUPPENRUF.wav .
cp /home/kevin/claude-ralph/tetra/wavs/gold_standard_380-393mhz/baseband_393084625Hz_00-11-52_25-04-2026.wav .
cp /home/kevin/claude-ralph/tetra/wavs/gold_standard_380-393mhz/baseband_382468718Hz_00-11-50_25-04-2026.wav .
```

## Capture metadata

| File | Purpose | Cell-ID | MS-ISSI |
|---|---|---|---|
| `GOLD_DL_ANMELDUNG_GRUPPENWECHSEL_GRUPPENRUF.wav` | DL: M2-Attach + Group-Switch + Group-Call (~104s, 100 MB) | MCC=262 MNC=1010 CC=1 | 0x282FF4 |
| `GOLD_UL_ANMELDUNG_GRUPPENWECHSEL_GRUPPENRUF.wav` | UL companion of above (~110s, 100 MB) | — | 0x282FF4 |
| `baseband_393084625Hz_00-11-52_25-04-2026.wav` | DL: external BS M2-Attach reference | — | 0x282FF4 |
| `baseband_382468718Hz_00-11-50_25-04-2026.wav` | UL companion | — | 0x282FF4 |

## Time-axis correlation

Per `docs/references/reference_gold_full_attach_timeline.md`:
`UL_t = DL_t + 1.611s` (empirical, mtime unreliable).
