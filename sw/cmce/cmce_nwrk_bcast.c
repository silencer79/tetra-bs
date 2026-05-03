/* sw/cmce/cmce_nwrk_bcast.c — D-NWRK-BROADCAST periodic-driver.
 *
 * Owned by S4 (S4-sw-cmce). Locked under IF_CMCE_v1.
 *
 * Emits D-NWRK-BROADCAST every CMCE_NWRK_BCAST_PERIOD_MF_DEFAULT
 * multiframes (≈10s, matching Gold-Cell cadence per
 * docs/references/reference_gold_full_attach_timeline.md
 * §"D-NWRK-BROADCAST-Cadence").
 *
 * Driver shape: pure tick-evaluator. The daemon main loop (S7) calls
 * cmce_nwrk_bcast_tick(now_mf) once per multiframe; if the function
 * returns true, the daemon then calls cmce_send_d_nwrk_broadcast(). This
 * decouples the periodic state from any real clock (tests pass synthetic
 * multiframe counters).
 *
 * Why multiframes (not seconds): the daemon's main loop is multiframe-
 * aligned. Counting in multiframes keeps the period exact-mod-loop-tick
 * rather than drifting via wall-clock.
 *
 * Cadence math (gold_full_attach_timeline.md, ETSI EN 300 392-2 §7.6):
 *   1 frame      = 56.67 ms (4 timeslots × 14.167 ms)
 *   1 multiframe = 18 frames = ~1.02 s  (frame 18 reserved → 17 traffic + 1 control)
 *   Gold cadence = 10.0 s ± 30 ms over 10 bursts (Burst #423 … #6775)
 *   ⇒ period = round(10.0 / 1.02) = 10 multiframes (≤2 % drift vs Gold)
 *
 * Default = 10 multiframes (CMCE_NWRK_BCAST_PERIOD_MF_DEFAULT in cmce.h).
 * Operator overrides via cfg.nwrk_bcast_period_multiframes if needed.
 */

#include "tetra/cmce.h"

#include <stddef.h>
#include <stdint.h>

bool cmce_nwrk_bcast_tick(Cmce *cmce, uint64_t now_mf)
{
    if (cmce == NULL || !cmce->initialised) {
        return false;
    }
    const uint16_t period = cmce->cfg.nwrk_bcast_period_multiframes;
    if (period == 0u) {
        /* Defensive: cfg validation in cmce_init enforces nonzero, but
         * recheck so a zeroed entity does not run-away-fire. */
        return false;
    }
    /* First-tick: fire immediately if last_bcast_tick_mf == 0 AND now_mf
     * has advanced past period. This avoids spurious immediate-fire on
     * cmce_init() at multiframe 0. */
    if (cmce->last_bcast_tick_mf == 0u) {
        /* Treat as "armed at boot, fire on first scheduled boundary".
         * The boundary is the first multiframe where now_mf >= period. */
        if (now_mf >= (uint64_t) period) {
            cmce->last_bcast_tick_mf = now_mf;
            return true;
        }
        return false;
    }
    /* Steady-state: fire when now_mf - last >= period (handles wrap-
     * around of the daemon counter via uint64_t subtraction). */
    if ((now_mf - cmce->last_bcast_tick_mf) >= (uint64_t) period) {
        cmce->last_bcast_tick_mf = now_mf;
        return true;
    }
    return false;
}
