/******************************************************************************
 * dbg_util.c
 *
 * Utility functions for the debug pub/sub system.
 *
 * Provides:
 *   - Monotonic timestamps (dbg_get_time_us)
 *   - CRC-32 (IEEE 802.3)
 *   - Byte-swap helpers
 *   - Hex dump for diagnostics
 *   - Ring buffer for statistics
 *
 * (c) 2025 — Internal use only.
 *****************************************************************************/

/* Request POSIX.1-2008 extensions (struct timespec, clock_gettime,
   CLOCK_MONOTONIC) when compiling under strict C99.
   Must appear before any system header is included. */
#ifndef DBG_PLATFORM_WINDOWS
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "dbg_util.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ── Platform time includes ─────────────────────────────────────────────── */
#ifdef DBG_PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach/mach_time.h>
#else
    /* POSIX / Linux */
    #include <time.h>
#endif

/* ══════════════════════════════════════════════════════════════════════════
   MONOTONIC TIMESTAMP
   ══════════════════════════════════════════════════════════════════════════ */

uint64_t dbg_get_time_us(void)
{
#ifdef DBG_PLATFORM_WINDOWS
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000LL) / freq.QuadPart);

#elif defined(__APPLE__)
    static mach_timebase_info_data_t tb = {0, 0};
    if (tb.denom == 0) {
        mach_timebase_info(&tb);
    }
    uint64_t ticks = mach_absolute_time();
    /* mach_absolute_time returns nanoseconds * (numer/denom) */
    uint64_t ns = ticks * tb.numer / tb.denom;
    return ns / 1000u;

#else
    /* POSIX: CLOCK_MONOTONIC */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u +
           (uint64_t)ts.tv_nsec / 1000u;
#endif
}

/* ══════════════════════════════════════════════════════════════════════════
   CRC-32 (IEEE 802.3 polynomial 0xEDB88320, reflected)
   ══════════════════════════════════════════════════════════════════════════ */

static uint32_t g_crc32_table[256];
static int      g_crc32_initialised = 0;

static void crc32_init_table(void)
{
    if (g_crc32_initialised) return;

    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
                        if (crc & 1u) {
                crc = (crc >> 1u) ^ 0xEDB88320u;
            } else {
                crc >>= 1u;
            }
        }
        g_crc32_table[i] = crc;
    }
    g_crc32_initialised = 1;
}

uint32_t dbg_crc32(const void *data, uint32_t len)
{
    crc32_init_table();

    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)(crc ^ p[i]);
        crc = (crc >> 8u) ^ g_crc32_table[idx];
    }

    return crc ^ 0xFFFFFFFFu;
}

uint32_t dbg_crc32_update(uint32_t crc, const void *data, uint32_t len)
{
    crc32_init_table();

    const uint8_t *p = (const uint8_t *)data;
    crc ^= 0xFFFFFFFFu;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)(crc ^ p[i]);
        crc = (crc >> 8u) ^ g_crc32_table[idx];
    }

    return crc ^ 0xFFFFFFFFu;
}

/* ══════════════════════════════════════════════════════════════════════════
   BYTE SWAP
   ══════════════════════════════════════════════════════════════════════════ */

uint16_t dbg_bswap16(uint16_t val)
{
    return (uint16_t)((val >> 8u) | (val << 8u));
}

uint32_t dbg_bswap32(uint32_t val)
{
    return ((val >> 24u) & 0x000000FFu) |
           ((val >>  8u) & 0x0000FF00u) |
           ((val <<  8u) & 0x00FF0000u) |
           ((val << 24u) & 0xFF000000u);
}

uint64_t dbg_bswap64(uint64_t val)
{
    val = ((val & 0x00000000FFFFFFFFull) << 32u) |
          ((val & 0xFFFFFFFF00000000ull) >> 32u);
    val = ((val & 0x0000FFFF0000FFFFull) << 16u) |
          ((val & 0xFFFF0000FFFF0000ull) >> 16u);
    val = ((val & 0x00FF00FF00FF00FFull) <<  8u) |
          ((val & 0xFF00FF00FF00FF00ull) >>  8u);
    return val;
}

float dbg_bswap_f32(float val)
{
    uint32_t tmp;
    memcpy(&tmp, &val, sizeof(tmp));
    tmp = dbg_bswap32(tmp);
    memcpy(&val, &tmp, sizeof(val));
    return val;
}

double dbg_bswap_f64(double val)
{
    uint64_t tmp;
    memcpy(&tmp, &val, sizeof(tmp));
    tmp = dbg_bswap64(tmp);
    memcpy(&val, &tmp, sizeof(val));
    return val;
}

int dbg_is_little_endian(void)
{
    uint16_t test = 0x0001;
    return *((uint8_t *)&test) == 0x01;
}

/* ══════════════════════════════════════════════════════════════════════════
   HEX DUMP (diagnostic)
   ══════════════════════════════════════════════════════════════════════════ */

void dbg_hexdump(const void *data, uint32_t len,
                 char *buf, uint32_t buf_size)
{
    if (!data || !buf || buf_size == 0) return;

    const uint8_t *p = (const uint8_t *)data;
    uint32_t pos = 0;

    for (uint32_t i = 0; i < len && pos + 4 < buf_size; i++) {
        if (i > 0 && (i % 16) == 0) {
            buf[pos++] = '\n';
        } else if (i > 0) {
            buf[pos++] = ' ';
        }

        int n = snprintf(buf + pos, buf_size - pos, "%02X", p[i]);
        if (n < 0) break;
        pos += (uint32_t)n;
    }

    buf[pos] = '\0';
}

void dbg_hexdump_print(const void *data, uint32_t len,
                       const char *label)
{
    if (!data) return;

    const uint8_t *p = (const uint8_t *)data;
    const uint32_t bytes_per_line = 16;

    if (label) {
        fprintf(stderr, "--- %s (%u bytes) ---\n", label, len);
    }

    for (uint32_t offset = 0; offset < len; offset += bytes_per_line) {
        /* Address */
        fprintf(stderr, "  %04X: ", offset);

        /* Hex bytes */
        for (uint32_t i = 0; i < bytes_per_line; i++) {
            if (offset + i < len) {
                fprintf(stderr, "%02X ", p[offset + i]);
            } else {
                fprintf(stderr, "   ");
            }
            if (i == 7) fprintf(stderr, " ");
        }

        /* ASCII */
        fprintf(stderr, " |");
        for (uint32_t i = 0; i < bytes_per_line && (offset + i) < len; i++) {
            uint8_t c = p[offset + i];
            fprintf(stderr, "%c", (c >= 0x20 && c <= 0x7E) ? c : '.');
        }
        fprintf(stderr, "|\n");
    }

    if (label) {
        fprintf(stderr, "---\n");
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   ELAPSED TIMER
   ══════════════════════════════════════════════════════════════════════════ */

void dbg_timer_start(dbg_timer_t *timer)
{
    if (!timer) return;
    timer->start_us = dbg_get_time_us();
}

uint64_t dbg_timer_elapsed_us(const dbg_timer_t *timer)
{
    if (!timer) return 0;
    return dbg_get_time_us() - timer->start_us;
}

float dbg_timer_elapsed_ms(const dbg_timer_t *timer)
{
    return (float)dbg_timer_elapsed_us(timer) / 1000.0f;
}

/* ══════════════════════════════════════════════════════════════════════════
   RATE LIMITER
   ══════════════════════════════════════════════════════════════════════════ */

void dbg_rate_limiter_init(dbg_rate_limiter_t *rl, uint64_t interval_us)
{
    if (!rl) return;
    rl->interval_us = interval_us;
    rl->last_us     = 0;
}

int dbg_rate_limiter_check(dbg_rate_limiter_t *rl)
{
    if (!rl) return 0;

    uint64_t now = dbg_get_time_us();
    uint64_t elapsed = now - rl->last_us;

    if (elapsed >= rl->interval_us) {
        rl->last_us = now;
        return 1;  /* allowed */
    }
    return 0;  /* rate-limited */
}

/* ══════════════════════════════════════════════════════════════════════════
   RUNNING STATISTICS (Welford's online algorithm)
   ══════════════════════════════════════════════════════════════════════════ */

void dbg_running_stats_init(dbg_running_stats_t *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->min_val =  1e30;
    st->max_val = -1e30;
}

void dbg_running_stats_update(dbg_running_stats_t *st, double value)
{
    if (!st) return;

    st->count++;

    if (value < st->min_val) st->min_val = value;
    if (value > st->max_val) st->max_val = value;

    /* Welford's online mean + variance */
    double delta  = value - st->mean;
    st->mean     += delta / (double)st->count;
    double delta2 = value - st->mean;
    st->m2       += delta * delta2;
}

double dbg_running_stats_variance(const dbg_running_stats_t *st)
{
    if (!st || st->count < 2) return 0.0;
    return st->m2 / (double)(st->count - 1);
}

double dbg_running_stats_stddev(const dbg_running_stats_t *st)
{
    double var = dbg_running_stats_variance(st);
    if (var <= 0.0) return 0.0;

    /* Newton's method for sqrt — avoids linking libm in some
       bare-metal environments */
    double guess = var;
    for (int i = 0; i < 20; i++) {
        guess = 0.5 * (guess + var / guess);
    }
    return guess;
}

void dbg_running_stats_reset(dbg_running_stats_t *st)
{
    dbg_running_stats_init(st);
}

/* ══════════════════════════════════════════════════════════════════════════
   RING BUFFER (fixed-size, overwrite-on-full)
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_ring_init(dbg_ring_t *ring,
                           void       *storage,
                           uint32_t    element_size,
                           uint32_t    capacity)
{
    if (!ring || !storage || element_size == 0 || capacity == 0) {
        return DBG_ERR_INTERNAL;
    }

    ring->storage      = (uint8_t *)storage;
    ring->element_size = element_size;
    ring->capacity     = capacity;
    ring->head         = 0;
    ring->tail         = 0;
    ring->count        = 0;

    return DBG_OK;
}

dbg_status_t dbg_ring_push(dbg_ring_t *ring, const void *element)
{
    if (!ring || !element) return DBG_ERR_INTERNAL;

    uint32_t offset = ring->head * ring->element_size;
    memcpy(ring->storage + offset, element, ring->element_size);

    ring->head = (ring->head + 1u) % ring->capacity;

    if (ring->count < ring->capacity) {
        ring->count++;
    } else {
        /* Overwrite oldest — advance tail */
        ring->tail = (ring->tail + 1u) % ring->capacity;
    }

    return DBG_OK;
}

dbg_status_t dbg_ring_pop(dbg_ring_t *ring, void *out)
{
    if (!ring || !out) return DBG_ERR_INTERNAL;
    if (ring->count == 0) return DBG_ERR_OUT_OF_RANGE;

    uint32_t offset = ring->tail * ring->element_size;
    memcpy(out, ring->storage + offset, ring->element_size);

    ring->tail = (ring->tail + 1u) % ring->capacity;
    ring->count--;

    return DBG_OK;
}

dbg_status_t dbg_ring_peek(const dbg_ring_t *ring, uint32_t index,
                           void *out)
{
    if (!ring || !out) return DBG_ERR_INTERNAL;
    if (index >= ring->count) return DBG_ERR_OUT_OF_RANGE;

    uint32_t actual = (ring->tail + index) % ring->capacity;
    uint32_t offset = actual * ring->element_size;
    memcpy(out, ring->storage + offset, ring->element_size);

    return DBG_OK;
}

void dbg_ring_clear(dbg_ring_t *ring)
{
    if (!ring) return;
    ring->head  = 0;
    ring->tail  = 0;
    ring->count = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   LOGGING
   ══════════════════════════════════════════════════════════════════════════ */

static dbg_log_level_t g_log_level   = DBG_LOG_WARN;
static dbg_log_cb_t    g_log_cb      = NULL;
static void           *g_log_ctx     = NULL;

void dbg_log_set_level(dbg_log_level_t level)
{
    g_log_level = level;
}

void dbg_log_set_callback(dbg_log_cb_t cb, void *user_ctx)
{
    g_log_cb  = cb;
    g_log_ctx = user_ctx;
}

static const char* log_level_str(dbg_log_level_t level)
{
    switch (level) {
    case DBG_LOG_TRACE: return "TRACE";
    case DBG_LOG_DEBUG: return "DEBUG";
    case DBG_LOG_INFO:  return "INFO ";
    case DBG_LOG_WARN:  return "WARN ";
    case DBG_LOG_ERROR: return "ERROR";
    case DBG_LOG_NONE:  return "     ";
    default:            return "?????";
    }
}

void dbg_log(dbg_log_level_t level, const char *fmt, ...)
{
    if (level < g_log_level) return;

    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (g_log_cb) {
        g_log_cb(level, buf, g_log_ctx);
    } else {
        uint64_t t = dbg_get_time_us();
        fprintf(stderr, "[%012" PRIu64 "] [%s] %s\n",
                t, log_level_str(level), buf);
    }
}