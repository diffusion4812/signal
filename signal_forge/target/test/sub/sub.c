/******************************************************************************
 * sub_main.c
 *
 * Example subscriber (client) application with debug pub/sub integration
 * and diagnostic printf statements.
 *
 * Updated: subscribes to ALL fields returned by the discover-fields request.
 *
 * Usage:
 *   ./subscriber [host] [options]
 *
 *   Options:
 *     -h <host>       Target IP/hostname   (default: 127.0.0.1)
 *     -d <port>       Data port            (default: 9500)
 *     -c <port>       Config port          (default: 9501)
 *     -i <us>         Subscribe interval   (default: 1000 µs)
 *     -l              List fields and exit
 *     -w <id> <val>   Write float value to field
 *     -v              Verbose frame output
 *     -q              Quiet (stats only)
 *
 * (c) 2025 — Internal use only.
 *****************************************************************************/

#include "dbg_pubsub.h"
#include "dbg_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <inttypes.h>

/* ══════════════════════════════════════════════════════════════════════════
   CONFIGURATION
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *host;
    uint16_t    data_port;
    uint16_t    config_port;
    uint32_t    interval_us;
    int         list_fields;
    int         verbose;
    int         quiet;
    int         do_write;
    uint64_t    write_field_id;
    float       write_value;
} app_options_t;

static const app_options_t DEFAULT_OPTIONS = {
    .host           = "127.0.0.1",
    .data_port      = 9500,
    .config_port    = 9501,
    .interval_us    = 1000,
    .list_fields    = 0,
    .verbose        = 0,
    .quiet          = 0,
    .do_write       = 0,
    .write_field_id = 0,
    .write_value    = 0.0f,
};

/* ══════════════════════════════════════════════════════════════════════════
   DYNAMICALLY DISCOVERED FIELDS
   ══════════════════════════════════════════════════════════════════════════ */

/** Maximum number of fields we can discover and subscribe to */
#define MAX_DISCOVERED_FIELDS  256

/** Maximum length of a field name (including null terminator) */
#define MAX_FIELD_NAME_LEN      64

/** Storage for a single discovered field descriptor */
typedef struct {
    uint64_t            field_id;
    dbg_value_type_t    value_type;
    uint8_t             access;
    char                name[MAX_FIELD_NAME_LEN];
    char                unit[MAX_FIELD_NAME_LEN];
} discovered_field_t;

/** Dynamic arrays built from the discover-fields response */
static discovered_field_t  g_discovered[MAX_DISCOVERED_FIELDS];
static uint16_t            g_discovered_count = 0;
static uint16_t            g_discovered_total = 0;

/** Arrays passed into dbg_sub_subscribe — built from g_discovered[] */
static uint64_t            g_sub_field_ids[MAX_DISCOVERED_FIELDS];
static dbg_value_type_t    g_sub_field_types[MAX_DISCOVERED_FIELDS];

#define SUB_ID  1

/* ══════════════════════════════════════════════════════════════════════════
   GLOBALS
   ══════════════════════════════════════════════════════════════════════════ */

static dbg_subscriber_t  *g_sub = NULL;
static dbg_sub_layout_t   g_layout;
static volatile int        g_running = 1;
static app_options_t       g_opts;

/* Frame statistics */
static dbg_running_stats_t g_jitter_stats;
static dbg_running_stats_t g_interval_stats;
static uint64_t            g_last_frame_time_us = 0;
static uint32_t            g_total_frames = 0;
static uint32_t            g_total_drops  = 0;

/* ══════════════════════════════════════════════════════════════════════════
   SIGNAL HANDLER
   ══════════════════════════════════════════════════════════════════════════ */

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
    printf("\n[SUB] Shutdown requested (signal %d)\n", sig);
}

/* ══════════════════════════════════════════════════════════════════════════
   ARGUMENT PARSING
   ══════════════════════════════════════════════════════════════════════════ */

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -h <host>       Target IP/hostname   (default: 127.0.0.1)\n");
    printf("  -d <port>       Data port            (default: 9500)\n");
    printf("  -c <port>       Config port          (default: 9501)\n");
    printf("  -i <us>         Subscribe interval   (default: 1000 µs)\n");
    printf("  -l              List fields and exit\n");
    printf("  -w <id> <val>   Write float value to field id (hex)\n");
    printf("  -v              Verbose frame output\n");
    printf("  -q              Quiet (stats only)\n");
    printf("  -?              Show this help\n\n");
}

static int parse_args(int argc, char **argv, app_options_t *opts)
{
    *opts = DEFAULT_OPTIONS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            opts->host = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            opts->data_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            opts->config_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            opts->interval_us = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-l") == 0) {
            opts->list_fields = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            opts->verbose = 1;
        } else if (strcmp(argv[i], "-q") == 0) {
            opts->quiet = 1;
        } else if (strcmp(argv[i], "-w") == 0 && i + 2 < argc) {
            opts->do_write = 1;
            opts->write_field_id = (uint64_t)strtoul(argv[++i], NULL, 16);
            opts->write_value = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "-?") == 0) {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        } else {
            printf("[SUB] Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   FIELD LIST CALLBACK  —  stores ALL discovered fields
   ══════════════════════════════════════════════════════════════════════════ */

static void on_field_list(const dbg_field_descriptor_t *fields,
                          uint16_t count, uint16_t total,
                          void *ctx)
{
    (void)ctx;

    /* Print header once on the first batch */
    if (g_discovered_count == 0) {
        g_discovered_total = total;
        printf("[SUB] ──── Available Fields (%u total) ────\n", total);
        printf("[SUB]   %-10s  %-32s  %-6s  %s\n",
               "ID", "NAME", "TYPE", "ACCESS");
        printf("[SUB]   %-10s  %-32s  %-6s  %s\n",
               "──────────", "────────────────────────────────",
               "──────", "──────");
    }

    for (uint16_t i = 0; i < count; i++) {
        const dbg_field_descriptor_t *f = &fields[i];

        /* Print to console */
        printf("[SUB]   0x%08" PRIX64 "  %-32s  %-6s  %s\n",
               f->field_id,
               f->name,
               dbg_value_type_str((dbg_value_type_t)f->value_type),
               f->access == DBG_ACCESS_READ_WRITE ? "RW" : "RO");

        /* Store into discovered array */
        if (g_discovered_count < MAX_DISCOVERED_FIELDS) {
            discovered_field_t *d = &g_discovered[g_discovered_count];
            d->field_id   = f->field_id;
            d->value_type = (dbg_value_type_t)f->value_type;
            d->access     = f->access;

            strncpy(d->name, f->name, MAX_FIELD_NAME_LEN - 1);
            d->name[MAX_FIELD_NAME_LEN - 1] = '\0';

            /* Default unit — could be extended if descriptors carry units */
            d->unit[0] = '\0';

            g_discovered_count++;
        } else {
            printf("[SUB] WARNING: Discovered field count exceeds "
                   "MAX_DISCOVERED_FIELDS (%d) — skipping 0x%08" PRIX64 "\n",
                   MAX_DISCOVERED_FIELDS, f->field_id);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   HELPER — Build subscription arrays from discovered fields
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Populate g_sub_field_ids[] and g_sub_field_types[] from the
 *         discovered field list so that we subscribe to every field the
 *         publisher exposes.
 */
static void build_subscription_arrays(void)
{
    for (uint16_t i = 0; i < g_discovered_count; i++) {
        g_sub_field_ids[i]   = g_discovered[i].field_id;
        g_sub_field_types[i] = g_discovered[i].value_type;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   DISCONNECT CALLBACK
   ══════════════════════════════════════════════════════════════════════════ */

static void on_disconnect(dbg_status_t reason, void *ctx)
{
    (void)ctx;
    printf("[SUB] *** DISCONNECT: %s ***\n", dbg_status_str(reason));
}

/* ══════════════════════════════════════════════════════════════════════════
   DISPLAY HELPERS
   ══════════════════════════════════════════════════════════════════════════ */

/** ANSI escape codes for terminal output */
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_RED     "\033[31m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_CLEAR   "\033[2J\033[H"

/** Column widths for table display */
#define COL_NAME   32
#define COL_VALUE  14
#define COL_TYPE    6
#define COL_UNIT   6

/**
 * @brief  Print a separator line.
 */
static void print_separator(void)
{
    printf("[SUB] +-%-*s-+-%-*s-+-%-*s-+-%-*s-+\n",
           COL_NAME,  "--------------------------------",
           COL_VALUE, "--------------",
           COL_TYPE,  "------",
           COL_UNIT,  "------");
}

/**
 * @brief  Print the table header.
 */
static void print_table_header(void)
{
    print_separator();
    printf("[SUB] | " ANSI_BOLD "%-*s" ANSI_RESET
           " | " ANSI_BOLD "%-*s" ANSI_RESET
           " | " ANSI_BOLD "%-*s" ANSI_RESET
           " | " ANSI_BOLD "%-*s" ANSI_RESET " |\n",
           COL_NAME,  "Field",
           COL_VALUE, "Value",
           COL_TYPE,  "Type",
           COL_UNIT,  "Unit");
    print_separator();
}

/**
 * @brief  Print a single field row.
 */
static void print_field_row(const char *name,
                            const char *value_str,
                            dbg_value_type_t type,
                            const char *unit)
{
    printf("[SUB] | %-*s | %*s | %-*s | %-*s |\n",
           COL_NAME,  name,
           COL_VALUE, value_str,
           COL_TYPE,  dbg_value_type_str(type),
           COL_UNIT,  unit ? unit : "");
}

/* ══════════════════════════════════════════════════════════════════════════
   FRAME DISPLAY MODES
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Default display: live-updating table, refreshed in-place.
 */
static void display_default(uint32_t sequence, uint64_t timestamp_us,
                            const uint8_t *payload)
{
    /* Clear screen and reposition cursor */
    printf(ANSI_CLEAR);

    /* Header */
    printf("[SUB] " ANSI_BOLD "═══ Live Debug Monitor ═══" ANSI_RESET "\n");
    printf("[SUB] Host:     %s:%u\n", g_opts.host, g_opts.data_port);
    printf("[SUB] Fields:   %u (all discovered)\n", g_discovered_count);
    printf("[SUB] Seq:      %u\n", sequence);
    printf("[SUB] Time:     %" PRIu64 " µs\n", timestamp_us);

    /* Frame rate */
    if (g_interval_stats.count > 0) {
        double rate = 1e6 / g_interval_stats.mean;
        const char *color = (rate > 900.0) ? ANSI_GREEN :
                            (rate > 500.0) ? ANSI_YELLOW : ANSI_RED;
        printf("[SUB] Rate:     %s%.1f Hz" ANSI_RESET "\n", color, rate);
    }

    /* Drop info */
    dbg_sub_stats_t stats;
    if (dbg_sub_get_stats(g_sub, SUB_ID, &stats) == DBG_OK) {
        if (stats.frames_dropped > 0) {
            printf("[SUB] Drops:    " ANSI_RED "%u" ANSI_RESET "\n",
                   stats.frames_dropped);
        } else {
            printf("[SUB] Drops:    " ANSI_GREEN "0" ANSI_RESET "\n");
        }
        printf("[SUB] RTT:      %.2f ms\n", stats.rtt_ms);
    }

    printf("[SUB]\n");

    /* Value table */
    print_table_header();

    dbg_frame_iter_t it;
    dbg_frame_iter_init(&it, payload, &g_layout);
    dbg_value_t val;

    int idx = 0;
    while (dbg_frame_iter_next(&it, &val) == DBG_OK) {
        char val_str[32];
        dbg_value_snprintf(it.current_type, &val, val_str, sizeof(val_str));

        const char *name = (idx < (int)g_discovered_count)
                           ? g_discovered[idx].name : "unknown";
        const char *unit = (idx < (int)g_discovered_count)
                           ? g_discovered[idx].unit : "";

        print_field_row(name, val_str, it.current_type, unit);
        idx++;
    }

    print_separator();

    /* Jitter info at bottom */
    if (g_jitter_stats.count > 1) {
        printf("[SUB]\n");
        printf("[SUB] Jitter:   mean=%.1f µs  max=%.0f µs  "
               "stddev=%.1f µs\n",
               g_jitter_stats.mean,
               g_jitter_stats.max_val,
               dbg_running_stats_stddev(&g_jitter_stats));
    }

    printf("[SUB]\n");
    printf("[SUB] Press Ctrl+C to exit\n");
}

/**
 * @brief  Verbose display: one-line-per-frame scrolling log.
 */
static void display_verbose(uint32_t sequence, uint64_t timestamp_us,
                            const uint8_t *payload)
{
    printf("[SUB] FRAME seq=%-8u ts=%-14" PRIu64 "  ",
           sequence, timestamp_us);

    dbg_frame_iter_t it;
    dbg_frame_iter_init(&it, payload, &g_layout);
    dbg_value_t val;

    int idx = 0;
    while (dbg_frame_iter_next(&it, &val) == DBG_OK) {
        char val_str[32];
        dbg_value_snprintf(it.current_type, &val, val_str, sizeof(val_str));

        const char *name = (idx < (int)g_discovered_count)
                           ? g_discovered[idx].name : "unknown";
        printf("%s=%s  ", name, val_str);
        idx++;
    }
    printf("\n");
}

/**
 * @brief  Scrolling display: periodic summary line (non-clearing).
 *
 *         Now generic — prints the first few discovered fields regardless
 *         of what they are, plus the effective frame rate.
 */
static void display_scrolling(uint32_t sequence, const uint8_t *payload)
{
    double rate = (g_interval_stats.count > 0)
                  ? 1e6 / g_interval_stats.mean : 0.0;

    printf("[SUB] [seq=%u] ", sequence);

    dbg_frame_iter_t it;
    dbg_frame_iter_init(&it, payload, &g_layout);
    dbg_value_t val;

    int idx = 0;
    /* Print up to the first 8 fields to keep the line readable */
    const int max_scroll_fields = 8;

    while (dbg_frame_iter_next(&it, &val) == DBG_OK && idx < max_scroll_fields) {
        char val_str[32];
        dbg_value_snprintf(it.current_type, &val, val_str, sizeof(val_str));

        const char *name = (idx < (int)g_discovered_count)
                           ? g_discovered[idx].name : "?";
        printf("%s=%s ", name, val_str);
        idx++;
    }

    if (g_discovered_count > (uint16_t)max_scroll_fields) {
        printf("[+%d more] ", g_discovered_count - max_scroll_fields);
    }

    printf(" (%.0f Hz)\n", rate);
}

/* ══════════════════════════════════════════════════════════════════════════
   FRAME CALLBACK
   ══════════════════════════════════════════════════════════════════════════ */

static void on_frame(uint16_t sub_id, uint32_t sequence,
                     uint64_t timestamp_us, const uint8_t *payload,
                     uint16_t payload_size, void *ctx)
{
    (void)sub_id;
    (void)payload_size;
    (void)ctx;

    uint64_t now = dbg_get_time_us();
    g_total_frames++;

    /* ── Inter-frame interval tracking ───────────────────── */
    if (g_last_frame_time_us > 0) {
        double interval = (double)(now - g_last_frame_time_us);
        dbg_running_stats_update(&g_interval_stats, interval);

        double expected = (double)g_opts.interval_us;
        double jitter = interval - expected;
        if (jitter < 0) jitter = -jitter;
        dbg_running_stats_update(&g_jitter_stats, jitter);
    }
    g_last_frame_time_us = now;

    /* ── Display ─────────────────────────────────────────── */
    if (g_opts.quiet) {
        /* No per-frame output — stats only via periodic report */
        return;
    }

    if (g_opts.verbose) {
        display_verbose(sequence, timestamp_us, payload);
        return;
    }

    /* Default mode: live table updated at a readable rate */
    static dbg_rate_limiter_t display_limiter = {0, 0};
    if (display_limiter.interval_us == 0) {
        /* Refresh at ~10 Hz to keep terminal readable */
        dbg_rate_limiter_init(&display_limiter, 100000);  /* 100 ms */
    }

    if (dbg_rate_limiter_check(&display_limiter)) {
        display_default(sequence, timestamp_us, payload);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   STATUS REPORT
   ══════════════════════════════════════════════════════════════════════════ */

static void print_status(void)
{
    dbg_sub_stats_t stats;
    dbg_status_t rc = dbg_sub_get_stats(g_sub, SUB_ID, &stats);

    printf("\n[SUB] ──── Connection Status ────\n");

    if (rc == DBG_OK) {
        printf("[SUB] Frames:    received=%u  dropped=%u  loss=%.2f%%\n",
               stats.frames_received,
               stats.frames_dropped,
               stats.frames_received > 0
                   ? 100.0 * (double)stats.frames_dropped /
                     (double)(stats.frames_received + stats.frames_dropped)
                   : 0.0);
        printf("[SUB] Bytes:     total=%u  (%.1f KB/s)\n",
               stats.bytes_received,
               g_interval_stats.count > 0
                   ? (double)stats.bytes_received /
                     ((double)g_interval_stats.count *
                      g_interval_stats.mean / 1e6)
                   : 0.0);
        printf("[SUB] RTT:       %.2f ms\n", stats.rtt_ms);
        printf("[SUB] Last seq:  %u\n", stats.last_sequence);
    } else {
        printf("[SUB] Stats unavailable: %s\n", dbg_status_str(rc));
    }

    if (g_interval_stats.count > 1) {
        printf("[SUB] Interval:  mean=%.1f µs  stddev=%.1f µs  "
               "min=%.0f µs  max=%.0f µs\n",
               g_interval_stats.mean,
               dbg_running_stats_stddev(&g_interval_stats),
               g_interval_stats.min_val,
               g_interval_stats.max_val);
    }

    if (g_jitter_stats.count > 1) {
        printf("[SUB] Jitter:    mean=%.1f µs  stddev=%.1f µs  "
               "max=%.0f µs\n",
               g_jitter_stats.mean,
               dbg_running_stats_stddev(&g_jitter_stats),
               g_jitter_stats.max_val);
    }

    printf("[SUB] Subscribed: %u fields (of %u discovered)\n",
           g_discovered_count, g_discovered_total);
    printf("[SUB] ────────────────────────────\n\n");
}

/* ══════════════════════════════════════════════════════════════════════════
   CONNECT AND SUBSCRIBE
   ══════════════════════════════════════════════════════════════════════════ */

static int app_connect(void)
{
    printf("[SUB] ========================================\n");
    printf("[SUB] Debug Subscriber — Connecting\n");
    printf("[SUB] ========================================\n");
    printf("[SUB] Target:   %s (data=%u, config=%u)\n",
           g_opts.host, g_opts.data_port, g_opts.config_port);
    printf("[SUB] Interval: %u µs\n", g_opts.interval_us);

    /* Configure logging */
    dbg_log_set_level(DBG_LOG_INFO);

    /* Create subscriber */
    dbg_sub_config_t cfg = DBG_SUB_CONFIG_DEFAULT;
    cfg.host        = g_opts.host;
    cfg.data_port   = g_opts.data_port;
    cfg.config_port = g_opts.config_port;

    printf("[SUB] Creating subscriber with host=%s data=%u config=%u\n",
           cfg.host, cfg.data_port, cfg.config_port);

    printf("[SUB] cfg.host=%s cfg.data_port=%u cfg.config_port=%u\n"
           "      cfg.timeout=%u cfg.hb=%u\n",
       cfg.host ? cfg.host : "(null)",
       cfg.data_port,
       cfg.config_port,
       cfg.config_timeout_ms,
       cfg.heartbeat_interval_ms);

    g_sub = dbg_sub_create(&cfg);

    printf("[SUB] dbg_sub_create returned: %p\n", (void *)g_sub);

    if (!g_sub) {
        printf("[SUB] ERROR: Failed to create subscriber\n");
        return -1;
    }
    printf("[SUB] Subscriber created\n");

    /* Set disconnect callback */
    dbg_sub_set_disconnect_cb(g_sub, on_disconnect, NULL);

    return 0;
}

static int app_discover_fields(void)
{
    printf("[SUB] Requesting field list...\n");

    /* Reset discovered fields before the request */
    g_discovered_count = 0;
    g_discovered_total = 0;

    dbg_timer_t timer;
    dbg_timer_start(&timer);

    dbg_status_t rc = dbg_sub_request_field_list(g_sub, on_field_list, NULL);

    float elapsed = dbg_timer_elapsed_ms(&timer);

    if (rc != DBG_OK) {
        printf("[SUB] ERROR: Field list request failed: %s\n",
               dbg_status_str(rc));
        return -1;
    }

    printf("[SUB] Field list received in %.1f ms  "
           "(%u fields captured)\n",
           (double)elapsed, g_discovered_count);
    printf("[SUB] ────────────────────────────\n\n");
    return 0;
}

static int app_subscribe(void)
{
    if (g_discovered_count == 0) {
        printf("[SUB] ERROR: No fields discovered — cannot subscribe\n");
        return -1;
    }

    /* Build the id/type arrays from the discovered field list */
    build_subscription_arrays();

    printf("[SUB] Subscribing to ALL %u discovered fields...\n",
           g_discovered_count);

    for (uint16_t i = 0; i < g_discovered_count; i++) {
        printf("[SUB]   [%2u] 0x%08" PRIX64 "  %-32s  %s  %s\n",
               i,
               g_discovered[i].field_id,
               g_discovered[i].name,
               dbg_value_type_str(g_discovered[i].value_type),
               g_discovered[i].access == DBG_ACCESS_READ_WRITE ? "RW" : "RO");
    }

    dbg_timer_t timer;
    dbg_timer_start(&timer);

    dbg_status_t rc = dbg_sub_subscribe(
        g_sub,
        SUB_ID,
        g_sub_field_ids,
        g_sub_field_types,
        g_discovered_count,
        g_opts.interval_us,
        &g_layout);

    float elapsed = dbg_timer_elapsed_ms(&timer);

    if (rc != DBG_OK) {
        printf("[SUB] ERROR: Subscribe failed: %s\n", dbg_status_str(rc));
        return -1;
    }

    printf("[SUB] Subscribe ACK received in %.1f ms\n", (double)elapsed);
    printf("[SUB] Layout:\n");
    printf("[SUB]   sub_id:      %u\n", g_layout.sub_id);
    printf("[SUB]   field_count: %u\n", g_layout.field_count);
    printf("[SUB]   frame_size:  %u bytes\n", g_layout.frame_size);
    printf("[SUB]   interval:    %u µs\n", g_layout.actual_interval_us);
    printf("[SUB]   Fields:\n");

    for (uint16_t i = 0; i < g_layout.field_count; i++) {
        const dbg_sub_field_layout_t *fl = &g_layout.fields[i];
        printf("[SUB]     [%2u] id=0x%08" PRIX64 "  offset=%3u  "
               "size=%u  type=%s",
               i, fl->field_id, fl->offset, fl->size,
               dbg_value_type_str(fl->type));

        if (i < g_discovered_count) {
            printf("  (%s)", g_discovered[i].name);
        }
        printf("\n");
    }

    uint32_t wire_size = dbg_frame_wire_size(g_layout.frame_size);
    double bandwidth = (double)wire_size * (1e6 / (double)g_opts.interval_us);

    printf("[SUB] Wire size: %u bytes/frame  (%.1f KB/s)\n",
           wire_size, bandwidth / 1024.0);
    printf("[SUB] ========================================\n\n");

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   WRITE COMMAND
   ══════════════════════════════════════════════════════════════════════════ */

static int app_do_write(void)
{
    printf("[SUB] Writing field 0x%04" PRIX64 " = %.6f\n",
           g_opts.write_field_id, (double)g_opts.write_value);

    dbg_value_t val;
    memset(&val, 0, sizeof(val));
    val.f32 = g_opts.write_value;

    dbg_timer_t timer;
    dbg_timer_start(&timer);

    dbg_status_t rc = dbg_sub_write(g_sub,
                                    g_opts.write_field_id,
                                    DBG_VT_F32,
                                    &val);

    float elapsed = dbg_timer_elapsed_ms(&timer);

    if (rc == DBG_OK) {
        printf("[SUB] Write succeeded in %.1f ms\n", (double)elapsed);
    } else {
        printf("[SUB] Write FAILED: %s (%.1f ms)\n",
               dbg_status_str(rc), (double)elapsed);
    }

    return (rc == DBG_OK) ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════════════════
   RECEIVE LOOP
   ══════════════════════════════════════════════════════════════════════════ */

static void app_receive_loop(void)
{
    printf("[SUB] Entering receive loop\n");

    if (g_opts.verbose) {
        printf("[SUB] Mode: VERBOSE (printing every frame)\n\n");
    } else if (g_opts.quiet) {
        printf("[SUB] Mode: QUIET (stats only)\n\n");
    } else {
        printf("[SUB] Mode: NORMAL (live table at ~10 Hz)\n\n");
    }

    /* Init statistics */
    dbg_running_stats_init(&g_jitter_stats);
    dbg_running_stats_init(&g_interval_stats);

    /* Status report timer */
    dbg_rate_limiter_t status_limiter;
    dbg_rate_limiter_init(&status_limiter, 5000000);  /* 5 seconds */

    /* Frame rate measurement */
    uint64_t rate_start     = dbg_get_time_us();
    uint32_t rate_frame_cnt = 0;

    while (g_running) {
        /* Poll for frames (non-blocking) */
        int frames = dbg_sub_poll(g_sub, on_frame, NULL);

        if (frames > 0) {
            rate_frame_cnt += (uint32_t)frames;
        } else {
            /* No frames — brief sleep to avoid busy-wait */
            usleep(100);
        }

        /* Periodic frame rate measurement */
        uint64_t now = dbg_get_time_us();
        uint64_t rate_elapsed = now - rate_start;

        if (rate_elapsed >= 1000000) {  /* every 1 second */
            double rate_hz = (double)rate_frame_cnt * 1e6 / (double)rate_elapsed;

            if (!g_opts.quiet) {
                /* Only print in verbose mode; default mode redraws the table */
                if (g_opts.verbose) {
                    printf("[SUB] Rate: %.1f Hz  (%u frames / %.3f s)\n",
                           rate_hz, rate_frame_cnt,
                           (double)rate_elapsed / 1e6);
                }
            }

            rate_start     = now;
            rate_frame_cnt = 0;
        }

        /* Periodic detailed status */
        if (dbg_rate_limiter_check(&status_limiter)) {
            if (g_opts.quiet || g_opts.verbose) {
                print_status();
            }
        }
    }
}

static int app_diagnose_connection(void)
{
    printf("[SUB] ──── Connection Diagnostics ────\n");
    printf("[SUB] Config port:  OK (subscribe succeeded)\n");
    printf("[SUB] Subscribed:   %u fields (all discovered)\n", g_discovered_count);

    /* Test: Send heartbeat and measure RTT */
    printf("[SUB] Heartbeat:   ");
    dbg_timer_t hb_timer;
    dbg_timer_start(&hb_timer);
    dbg_status_t rc = dbg_sub_send_heartbeat(g_sub);
    float hb_ms = dbg_timer_elapsed_ms(&hb_timer);

    if (rc == DBG_OK) {
        printf("OK (%.1f ms RTT)\n", (double)hb_ms);
    } else {
        printf("FAILED: %s\n", dbg_status_str(rc));
    }

    /* Test: Wait briefly for a frame */
    printf("[SUB] Waiting 3s for first frame...\n");

    int got_frame = 0;
    uint64_t wait_start = dbg_get_time_us();

    while (dbg_get_time_us() - wait_start < 3000000) {
        int n = dbg_sub_poll(g_sub, on_frame, NULL);
        if (n > 0) {
            printf("[SUB] ✓ Received %d frame(s)!\n", n);
            got_frame = 1;
            break;
        }
        usleep(1000);
    }

    if (!got_frame) {
        printf("[SUB] ✗ No frames received in 3 seconds\n");
        printf("[SUB]\n");
        printf("[SUB] Possible causes:\n");
        printf("[SUB]   1. Publisher not calling dbg_pub_send_all()\n");
        printf("[SUB]   2. Publisher sending to wrong port\n");
        printf("[SUB]      (check [PUB] log for destination port)\n");
        printf("[SUB]   3. Different subnets / no route\n");
        printf("[SUB]   4. Loopback? Try -h 127.0.0.1\n");
    }

    printf("[SUB] ────────────────────────────────\n\n");
    return got_frame ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    printf("[SUB] Subscriber application starting\n");

    /* Parse arguments */
    if (parse_args(argc, argv, &g_opts) < 0) {
        return EXIT_FAILURE;
    }

    /* Connect */
    if (app_connect() < 0) {
        return EXIT_FAILURE;
    }

    /* List fields mode — discover, print, and exit */
    if (g_opts.list_fields) {
        int rc = app_discover_fields();
        dbg_sub_destroy(g_sub);
        return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /* ── Discovery is now MANDATORY for subscribe ───────────────────────
     * We subscribe to ALL fields returned by discovery, so if it fails
     * we cannot proceed.
     * ─────────────────────────────────────────────────────────────────── */
    printf("[SUB] Discovering fields from publisher...\n");
    if (app_discover_fields() < 0) {
        printf("[SUB] ERROR: Field discovery failed — "
               "cannot determine fields to subscribe to.\n");
        dbg_sub_destroy(g_sub);
        return EXIT_FAILURE;
    }

    if (g_discovered_count == 0) {
        printf("[SUB] ERROR: Publisher reported 0 fields — nothing to "
               "subscribe to.\n");
        dbg_sub_destroy(g_sub);
        return EXIT_FAILURE;
    }

    printf("[SUB] Discovered %u field(s) — subscribing to all\n\n",
           g_discovered_count);

    /* Subscribe to every discovered field */
    if (app_subscribe() < 0) {
        dbg_sub_destroy(g_sub);
        return EXIT_FAILURE;
    }

    app_diagnose_connection();

    /* Optional write before entering receive loop */
    if (g_opts.do_write) {
        app_do_write();
        printf("\n");
    }

    /* Receive loop */
    app_receive_loop();

    /* Shutdown */
    printf("\n[SUB] ========================================\n");
    printf("[SUB] Shutting down\n");

    print_status();

    printf("[SUB] Unsubscribing...\n");
    dbg_status_t rc = dbg_sub_unsubscribe(g_sub, SUB_ID);
    printf("[SUB] Unsubscribe: %s\n", dbg_status_str(rc));

    dbg_sub_destroy(g_sub);
    printf("[SUB] Destroyed — goodbye\n");
    printf("[SUB] ========================================\n");

    return EXIT_SUCCESS;
}