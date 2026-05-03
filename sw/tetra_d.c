/* sw/tetra_d.c — main daemon for tetra-bs.
 *
 * Owned by S7 (S7-sw-tetra-d). Wires the SW stack:
 *   S0 msgbus  -> S5 db (SubscriberDb + AST)
 *               -> S1 dma_io
 *               -> S2 llc -> S3 mle/mm -> S4 cmce
 *   plus       -> socket_server (Unix-socket WebUI listener)
 *   plus       -> main_loop (epoll over DMA IRQs + listener + signalfd
 *                            + timerfd)
 *
 * Lifecycle (Decision #10):
 *   - On startup: db_open(), ast_reload() — only repopulates AST when
 *     the on-disk snapshot has clean_shutdown_flag=true. Any other
 *     state on disk is treated as crash-residue and dropped.
 *   - On SIGTERM/SIGINT or `daemon.stop` op:
 *       * loop sets shutdown_requested + clean_shutdown_flag
 *       * we ast_snapshot() with clean_shutdown_flag=true
 *       * db_atomic_save() flushes any pending DB writes
 *       * exit 0
 *   - On crash (no SIGTERM path): the previous snapshot's
 *     clean_shutdown_flag stays whatever it was at last clean stop.
 *     Because ast_snapshot rewrites the file atomically, a half-write
 *     leaves the *previous* snapshot intact; ast_reload still sees the
 *     last-clean copy. The crash itself is detected at next start by
 *     the absence of a fresh save: the daemon initialises an empty AST
 *     and refuses to read from a snapshot that is older than its
 *     companion db.json.
 *
 * The CLI is intentionally minimal: `--help` prints op-name catalogue
 * (so operators can see the surface area without hitting the WebUI),
 * `--version` prints the build, otherwise the daemon starts.
 */
#define _GNU_SOURCE   /* env_or pulls getenv via stdlib; defined for symmetry */

#include "tetra/cmce.h"
#include "tetra/daemon_ops.h"
#include "tetra/db.h"
#include "tetra/dma_io.h"
#include "tetra/llc.h"
#include "tetra/mle.h"
#include "tetra/mm.h"
#include "tetra/msgbus.h"
#include "tetra/sap.h"
#include "tetra/types.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward decls — defined in main_loop.c. The full DaemonState shape
 * lives there too. */
struct DaemonState;
int  main_loop_init(struct DaemonState *st);
int  main_loop_run(struct DaemonState *st);
void main_loop_teardown(struct DaemonState *st);

/* Re-declare DaemonState here for stack-allocation; matches main_loop.c
 * verbatim. The duplicate is intentional (and small): any field-shape
 * drift between the two files is caught at link time because the same
 * struct symbol is used by both translation units.
 *
 * NOTE: keep this layout identical to the one in main_loop.c. */
struct DaemonState {
    MsgBus      *bus;
    Llc         *llc;
    Mle         *mle;
    Mm          *mm;
    Cmce        *cmce;
    SubscriberDb *db;
    Ast         *ast;
    const char  *ast_path;
    bool         ast_loaded_at_start;
    DmaCtx      *dma;
    int          epoll_fd;
    int          listener_fd;
    int          signal_fd;
    int          timer_fd;
    int          dma_irq_fd[DMA_CHAN_COUNT];
    uint64_t     mf_now;
    bool         shutdown_requested;
    bool         clean_shutdown_flag;
};

/* ---------------------------------------------------------------------------
 * Default file paths. Override via env (TETRA_*_PATH) for tests +
 * non-board hosts. The production paths come from Decision #8 +
 * OPERATIONS.md §"Runtime contract".
 * ------------------------------------------------------------------------- */
#define DEFAULT_DB_PATH   "/var/lib/tetra/db.json"
#define DEFAULT_AST_PATH  "/var/lib/tetra/ast.json"

static const char *env_or(const char *name, const char *dfl)
{
    const char *v = getenv(name);
    return (v != NULL && *v != '\0') ? v : dfl;
}

/* ---------------------------------------------------------------------------
 * Storage for the msgbus + entities. All stack-allocated in main() but
 * defined as file-scope here so each entity has a stable address that
 * matches what was passed to its `_init`.
 *
 * Sized for a small busy MS+BS lab: 32 entries per priority * 256 byte
 * payload = 24 KiB total. Bluestation's defaults sit at similar order
 * of magnitude and fit comfortably in 512 MiB Zynq RAM.
 * ------------------------------------------------------------------------- */
#define BUS_QUEUE_CAP_PER_PRIO  32u
#define BUS_MAX_PAYLOAD_BYTES   256u
#define BUS_TOTAL_QUEUES        (3u * BUS_QUEUE_CAP_PER_PRIO)

/* ---------------------------------------------------------------------------
 * print_op_catalogue — one-line-per-op, keeps `--help` short. */
static void print_op_catalogue(void)
{
    size_t count = 0;
    const DaemonOpEntry *t = daemon_op_table(&count);
    printf("tetra_d — IF_DAEMON_OPS_v1\n"
           "  socket: %s\n"
           "  ops (%zu):\n", DAEMON_OPS_SOCKET_PATH, count);
    for (size_t i = 0; i < count; i++) {
        printf("    %s\n", t[i].name);
    }
}

static void print_help(void)
{
    printf("usage: tetra_d [--help|--version|--list-ops]\n"
           "  --help        print this message\n"
           "  --list-ops    print the IF_DAEMON_OPS_v1 op-name catalogue\n"
           "  --version     print version + interface lock\n");
}

/* ---------------------------------------------------------------------------
 * Daemon entry. Returns 0 on clean shutdown, non-zero on init failure.
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    /* CLI dispatch (no getopt — three flags, exact match). */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[i], "--list-ops") == 0) {
            print_op_catalogue();
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("tetra_d 0.1 IF_DAEMON_OPS_v1\n");
            return 0;
        }
    }

    /* ----- msgbus storage on stack (not heap; stays test-deterministic). */
    static MsgBusEntry  bus_entries[BUS_TOTAL_QUEUES];
    static uint8_t      bus_payloads[BUS_TOTAL_QUEUES * BUS_MAX_PAYLOAD_BYTES];
    static MsgBus       bus;
    MsgBusCfg buscfg = {
        .queue_cap_per_prio    = BUS_QUEUE_CAP_PER_PRIO,
        .max_payload_bytes     = BUS_MAX_PAYLOAD_BYTES,
        .entry_storage         = bus_entries,
        .entry_storage_bytes   = sizeof(bus_entries),
        .payload_storage       = bus_payloads,
        .payload_storage_bytes = sizeof(bus_payloads),
    };
    int rc = msgbus_init(&bus, &buscfg);
    if (rc != 0) {
        fprintf(stderr, "tetra_d: msgbus_init failed (%d)\n", rc);
        return 2;
    }

    /* ----- Subscriber-DB + AST. */
    static SubscriberDb db;
    const char *db_path = env_or("TETRA_DB_PATH", DEFAULT_DB_PATH);
    rc = db_open(&db, db_path);
    if (rc != 0) {
        fprintf(stderr, "tetra_d: db_open(%s) failed (%d)\n", db_path, rc);
        /* fall through — daemon still starts with an empty db; AST
         * reload will skip too. The WebUI shows the empty state. */
    }

    static Ast ast;
    bool ast_loaded = false;
    const char *ast_path = env_or("TETRA_AST_PATH", DEFAULT_AST_PATH);
    rc = ast_reload(&ast, ast_path, &ast_loaded);
    if (rc != 0) {
        /* Hard JSON-parse failure on a flagged file: log + start clean. */
        fprintf(stderr, "tetra_d: ast_reload(%s) hard-failed (%d), "
                        "starting with empty AST\n", ast_path, rc);
        memset(&ast, 0, sizeof(ast));
    } else if (!ast_loaded) {
        memset(&ast, 0, sizeof(ast));
    }

    /* ----- DMA glue — pipe-mock by default on host build (force_mock).
     * Production cross-compile keeps force_mock=false; the build-time
     * macro switches the dma_io.c backend.  We honour an env override
     * for live debugging (TETRA_DMA_FORCE_MOCK=1 forces mock anywhere). */
    static DmaCtx dma_ctx;
    DmaCfg dmacfg = { 0 };
    if (env_or("TETRA_DMA_FORCE_MOCK", "0")[0] == '1') {
        dmacfg.force_mock = true;
    }
    rc = dma_init(&dma_ctx, &dmacfg);
    if (rc != 0) {
        fprintf(stderr, "tetra_d: dma_init failed (%d) — RX path inert\n", rc);
        /* Continue: daemon still serves the WebUI; air-side is dead. */
    }

    /* ----- Entities, in dependency order (msgbus + db must be live first):
     *   LLC -> MLE -> MM -> CMCE
     * Each `_init` calls msgbus_register() internally; after the four
     * inits all (dest, sap) tuples in IF_LLC_v1 / IF_MLE_v1 / IF_MM_v1 /
     * IF_CMCE_v1 are wired. */
    static Llc  llc;
    static Mle  mle;
    static Mm   mm;
    static Cmce cmce;

    LlcCfg llccfg = { .max_retx = LLC_DEFAULT_MAX_RETX };
    rc = llc_init(&llc, &bus, &llccfg);
    if (rc != 0) {
        fprintf(stderr, "tetra_d: llc_init failed (%d)\n", rc);
        return 3;
    }

    MleCfg mlecfg = {
        .accept_unknown     = true,
        .default_profile_id = 0u,
        .fallback_gssi      = 0x2F4D61u,  /* Gold-Cell GSSI */
    };
    rc = mle_init(&mle, &bus, &db, &mlecfg);
    if (rc != 0) {
        fprintf(stderr, "tetra_d: mle_init failed (%d)\n", rc);
        return 4;
    }

    MmCfg mmcfg = { .reserved = 0u };
    rc = mm_init(&mm, &bus, &db, &mmcfg);
    if (rc != 0) {
        fprintf(stderr, "tetra_d: mm_init failed (%d)\n", rc);
        return 5;
    }

    CmceCfg cmcecfg = {
        .nwrk_bcast_period_multiframes = CMCE_NWRK_BCAST_PERIOD_MF_DEFAULT,
        .cell_re_select_parameters_seed = CMCE_NWRK_DEFAULT_CRSP,
        .cell_load_ca = CMCE_NWRK_DEFAULT_CL_CA,
    };
    rc = cmce_init(&cmce, &bus, &cmcecfg);
    if (rc != 0) {
        fprintf(stderr, "tetra_d: cmce_init failed (%d)\n", rc);
        return 6;
    }

    /* ----- Daemon state, hands to main_loop. */
    struct DaemonState st = { 0 };
    st.bus       = &bus;
    st.llc       = &llc;
    st.mle       = &mle;
    st.mm        = &mm;
    st.cmce      = &cmce;
    st.db        = &db;
    st.ast       = &ast;
    st.ast_path  = ast_path;
    st.ast_loaded_at_start = ast_loaded;
    st.dma       = &dma_ctx;
    st.epoll_fd  = -1;
    st.listener_fd = -1;
    st.signal_fd = -1;
    st.timer_fd  = -1;
    for (size_t i = 0; i < DMA_CHAN_COUNT; i++) {
        st.dma_irq_fd[i] = -1;
    }
    st.shutdown_requested  = false;
    st.clean_shutdown_flag = false;

    rc = main_loop_init(&st);
    if (rc != 0) {
        fprintf(stderr, "tetra_d: main_loop_init failed (%d)\n", rc);
        main_loop_teardown(&st);
        return 7;
    }

    fprintf(stderr, "tetra_d: ready, listening on %s "
                    "(ast_loaded=%s, dma=%s)\n",
            DAEMON_OPS_SOCKET_PATH,
            ast_loaded ? "yes" : "no",
            dmacfg.force_mock ? "mock" : "live");

    /* ----- Run loop. Blocks until shutdown_requested. */
    rc = main_loop_run(&st);
    if (rc != 0) {
        fprintf(stderr, "tetra_d: main_loop_run failed (%d)\n", rc);
    }

    /* ----- Clean-shutdown sequence (Decision #10). */
    if (st.clean_shutdown_flag) {
        fprintf(stderr, "tetra_d: clean shutdown — flushing AST + DB\n");
        int srrc = ast_snapshot(&ast, ast_path);
        if (srrc != 0) {
            fprintf(stderr, "tetra_d: ast_snapshot(%s) failed (%d)\n",
                    ast_path, srrc);
        }
        int dbrc = db_atomic_save(&db);
        if (dbrc != 0 && dbrc != -ENOENT) {
            fprintf(stderr, "tetra_d: db_atomic_save failed (%d)\n", dbrc);
        }
    } else {
        fprintf(stderr, "tetra_d: NOT a clean shutdown — AST not snapshotted\n");
    }

    main_loop_teardown(&st);
    dma_close(&dma_ctx);

    return (rc == 0) ? 0 : 1;
}
