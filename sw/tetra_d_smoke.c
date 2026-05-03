/* tetra_d_smoke.c — placeholder daemon entry, cross-compile canary.
 *
 * Owned by T0 build-skeleton. This file exists ONLY so `make sw-build`
 * has something to cross-compile and verify the ARM hard-float toolchain
 * is wired correctly per HARDWARE.md §2 (the PATH-precedence trap that
 * makes a bare `arm-linux-gnueabihf-gcc` resolve to Vitis' 11.2 wrapper
 * instead of Ubuntu's 13.3).
 *
 * Phase-2 agent S7 (`S7-sw-tetra-d`) replaces this with the real daemon
 * (`sw/tetra_d.c`). Once that lands, the top-level Makefile's `sw-build`
 * target switches from this smoke source to the real source set, and
 * this file is deleted.
 *
 * The build is verified by checking that `file build/arm/tetra_d_smoke`
 * reports an "ARM, EABI5" hard-float ELF.
 */

#include <stdio.h>
#include <stdint.h>

int main(int argc, char **argv)
{
    /* Touch argc/argv so -Werror=unused-parameter does not bite. */
    (void)argc;
    (void)argv;

    /* Reference a uintptr_t-sized symbol so the resulting binary
     * actually pulls in the runtime and is not optimised to nothing.
     */
    volatile uintptr_t marker = (uintptr_t)&main;
    printf("tetra_d smoke build, main=%p\n", (void *)marker);
    return 0;
}
