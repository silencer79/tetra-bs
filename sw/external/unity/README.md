# Unity (vendored)

Header-only-style C unit-test framework used by all SW host-tests under `tb/sw/`.

| Field | Value |
|---|---|
| **Upstream** | https://github.com/ThrowTheSwitch/Unity |
| **Pinned tag** | `v2.6.0` |
| **Tarball URL** | https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v2.6.0.tar.gz |
| **License** | MIT (see `LICENSE.txt`) |
| **Vendored on** | 2026-05-03 |
| **Vendored by** | T0 build-skeleton agent |

## What is here

Only the three core source files from `Unity-2.6.0/src/` plus the upstream
license:

```
unity.c
unity.h
unity_internals.h
LICENSE.txt
```

The Ruby helpers under `auto/`, the meson/CMake build files, examples, and
docs from the upstream tarball are **not** vendored — `tb/sw/Makefile.inc`
links `unity.c` directly into each test binary. If you need the test-runner
generator (`generate_test_runner.rb`) later, download that file from the
pinned tag at the URL above; do not auto-generate test runners in this
repository — keep test main()s explicit.

## How it is built

Per `tb/sw/Makefile.inc`:

- `gcc -O0 -g -Wall -Wextra -Werror -I sw/external/unity -c unity.c -o build/sw/unity.o`
- Each `tb/sw/<block>/test_*.c` is linked with `unity.o`.

## Decision reference

- `docs/HARDWARE.md` §5: "C unit-test framework: Unity (ThrowTheSwitch),
  vendor under `sw/external/unity/` at a pinned tag (e.g. `v2.6.0`)."
- `docs/MIGRATION_PLAN.md` agent T3 deliverable: "`sw/external/unity/`
  (vendored, pinned)".

## Bumping the pin

1. Read the Unity changelog for the new tag.
2. Replace `unity.c`, `unity.h`, `unity_internals.h`, `LICENSE.txt` from the
   new tarball.
3. Update the table at the top of this file.
4. Run `make sw-test` and confirm the existing tests still pass; bump
   compiler warning flags only if upstream removed deprecated APIs.
