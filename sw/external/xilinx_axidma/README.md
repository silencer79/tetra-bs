# xilinx_axidma — vendored userspace AXI-DMA driver

Owned by S1 (`S1-sw-dma-glue`). Companion to `sw/dma_io/dma_io.c`,
which gates real-HW use of this library behind `HAVE_LIBAXIDMA`.

## Source

- **Upstream:** <https://github.com/jacobfeder/xilinx_axidma>
- **License:** MIT (compatible with our GPL-2.0 daemon — see
  `docs/HARDWARE.md` §4 lock decision dated 2026-05-03).
- **Pinned commit:** `<-- TODO: pin specific commit hash -->`
  *(lookup via `git ls-remote https://github.com/jacobfeder/xilinx_axidma`
  failed at vendor-time on the audit host: `Repository not found`.
  Either the repository was renamed/moved, or the audit host had
  no GitHub credentials. Re-run `scripts/build/vendor-axidma.sh`
  from a host with network access; the script writes the resolved
  commit hash back into this README. If the upstream repo cannot
  be located, fall back to the LinuxFPGA fork referenced in
  `docs/HARDWARE.md` §4 and update this URL.)*

## Why this lives here

The migration plan (Decision #3, `docs/MIGRATION_PLAN.md`) calls for
4× AXI-DMA channels. `docs/HARDWARE.md` §4 picked Option B (Jacob
Feder's `libaxidma`) on 2026-05-03 because:

- the board already runs an out-of-tree kernel-module workflow
  (`/root/kernel_modules32/xilinx_dma.ko`),
- `libaxidma` has clean 4-channel multi-instance support,
- MIT licensing is compatible with our GPL-2.0 base.

## Layout (after `scripts/build/vendor-axidma.sh` runs)

```
sw/external/xilinx_axidma/
├── README.md          (this file — kept under git)
├── Makefile.kmod      (stub describing the module-build invocation)
├── COMMIT_HASH        (written by the vendor script — pinned hash)
├── LICENSE            (MIT, copied from upstream)
├── library/           (libaxidma userspace .so source)
├── driver/            (xilinx_axidma.ko kernel-module source)
├── examples/          (upstream examples — kept for reference, not built)
└── scripts/           (any upstream helper scripts)
```

The vendor script does NOT commit upstream sources to our repo by default;
the directory tree above is created in-place under the worktree but is
covered by `.gitignore` so the pin hash is the canonical reference, not
the bytes. (Bump procedure — see below — re-runs the clone.) If you
prefer to commit the sources for offline reproducibility, edit the
vendor script's `KEEP_HISTORY=` flag.

## Building on the target board (Zynq-7020 / kernel 5.10)

`Makefile.kmod` is a stub right now — see file. Once the upstream is
checked out at the pinned commit, the build is two steps:

```sh
# 1. Userspace library libaxidma.so
cd sw/external/xilinx_axidma/library
make                        # → libaxidma.so

# 2. Kernel module xilinx_axidma.ko (cross-build for ARM, kernel 5.10)
cd ../driver
make module \
    ARCH=arm \
    CROSS_COMPILE=arm-linux-gnueabihf- \
    KDIR=/lib/modules/5.10.0-98248-g1bbe32fa5182-dirty/build
                            # → xilinx_axidma.ko
```

`docs/HARDWARE.md` §3 records the live kernel version on Board #1.

## Bump procedure

To advance the pinned commit (e.g. when a new upstream tag lands or a
kernel-compatibility bug is fixed):

1. Find the candidate commit: `git ls-remote https://github.com/jacobfeder/xilinx_axidma`.
2. Update `scripts/build/vendor-axidma.sh`'s `PIN_REF=` variable to the new
   commit hash (40-char) or tag name.
3. Run `scripts/build/vendor-axidma.sh` — it will re-clone, write the
   new commit hash into `sw/external/xilinx_axidma/COMMIT_HASH`, and
   patch this README's "Pinned commit" line in-place.
4. Cross-build `xilinx_axidma.ko` against the board's kernel headers
   (`/lib/modules/<uname -r>/build`).
5. Re-run `make sw-test` (host) — should still PASS, the unit tests
   use the pipe-mock backend and never link against `libaxidma.so`.
6. Live-validation on Board #1: `insmod xilinx_axidma.ko`, then run
   the daemon under `tetra_d --dma-loopback-test` (S7 will provide).
7. Commit the README + script change in one atomic commit titled
   `chore(s1): bump xilinx_axidma to <hash>`.

## Why no in-tree clone yet?

Network access at vendor-time (this commit) failed to locate the
upstream URL. The S1 deliverable is therefore the *vendoring scaffold*
(this README, `Makefile.kmod`, `scripts/build/vendor-axidma.sh`),
not the upstream sources. A follow-up commit on a network-connected
host runs the script and lands the pinned hash. Unit-tests pass today
with the pipe-mock backend (`HAVE_LIBAXIDMA` undefined), so this
unblocks S2/S3/S4/S7 from making progress against `IF_DMA_API_v1`.
