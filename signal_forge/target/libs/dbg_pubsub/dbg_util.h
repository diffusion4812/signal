/******************************************************************************
 * dbg_util.h
 *
 * Utility functions for the debug pub/sub system.
 *
 * (c) 2025 — Internal use only.
 *****************************************************************************/

#ifndef DBG_UTIL_H
#define DBG_UTIL_H

#include "dbg_protocol.h"
#include <stdint.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════════════════════
   MONOTONIC TIMESTAMP
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Get monotonic time in microseconds.
 * @return Microseconds since an arbitrary fixed epoch.
 */
uint64_t dbg_get_time_us(void);

/* ══════════════════════════════════════════════════════════════════════════
   CRC-32
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Compute CRC-32 (IEEE 802.3) over a buffer.
 *
 * @param  data  Input data.
 * @param  len   Length in bytes.
 * @return CRC-32 value.
 */
uint32_t dbg_crc32(const void *data, uint32_t len);

/**
 * @brief  Incrementally update a CRC-32.
 *
 * Usage:
 *   uint32_t crc = dbg_crc32(first_chunk, first_len);
 *   crc = dbg_crc32_update(crc, second_chunk, second_len);
 *
 * @param  crc   Previous CRC value.
 * @param  data  Next data chunk.
 * @param  len   Chunk length in bytes.
 * @return Updated CRC-32.
 */
uint32_t dbg_crc32_update(uint32_t crc, const void *data, uint32_t len);

/* ══════════════════════════════════════════════════════════════════════════
   BYTE SWAP
   ══════════════════════════════════════════════════════════════════════════ */

uint16_t dbg_bswap16(uint16_t val);
uint32_t dbg_bswap32(uint32_t val);
uint64_t dbg_bswap64(uint64_t val);
float    dbg_bswap_f32(float val);
double   dbg_bswap_f64(double val);

/**
 * @brief  Detect runtime endianness.
 * @return 1 if little-endian, 0 if big-endian.
 */
int dbg_is_little_endian(void);

/* ══════════════════════════════════════════════════════════════════════════
   HEX DUMP
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Format a hex dump into a string buffer.
 *
 * @param  data      Input data.
 * @param  len       Length in bytes.
 * @param  buf       Output string buffer.
 * @param  buf_size  Buffer capacity.
 */
void dbg_hexdump(const void *data, uint32_t len,
                 char *buf, uint32_t buf_size);

/**
 * @brief  Print a formatted hex dump to stderr.
 *
 * @param  data   Input data.
 * @param  len    Length in bytes.
 * @param  label  Optional label printed above the dump (NULL to omit).
 */
void dbg_hexdump_print(const void *data, uint32_t len,
                       const char *label);

/* ══════════════════════════════════════════════════════════════════════════
   ELAPSED TIMER
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t start_us;
} dbg_timer_t;

/**
 * @brief  Start (or restart) a timer.
 */
void dbg_timer_start(dbg_timer_t *timer);

/**
 * @brief  Read elapsed time since start, in microseconds.
 */
uint64_t dbg_timer_elapsed_us(const dbg_timer_t *timer);

/**
 * @brief  Read elapsed time since start, in milliseconds.
 */
float dbg_timer_elapsed_ms(const dbg_timer_t *timer);

/* ══════════════════════════════════════════════════════════════════════════
   RATE LIMITER
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t interval_us;
    uint64_t last_us;
} dbg_rate_limiter_t;

/**
 * @brief  Initialise a rate limiter.
 *
 * @param  rl           Rate limiter state.
 * @param  interval_us  Minimum interval between allowed events.
 */
void dbg_rate_limiter_init(dbg_rate_limiter_t *rl, uint64_t interval_us);

/**
 * @brief  Check if an event is allowed.
 *
 * @return 1 if allowed (interval elapsed), 0 if rate-limited.
 */
int dbg_rate_limiter_check(dbg_rate_limiter_t *rl);

/* ══════════════════════════════════════════════════════════════════════════
   RUNNING STATISTICS (Welford's online algorithm)
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t count;
    double   mean;
    double   m2;
    double   min_val;
    double   max_val;
} dbg_running_stats_t;

void   dbg_running_stats_init(dbg_running_stats_t *st);
void   dbg_running_stats_update(dbg_running_stats_t *st, double value);
double dbg_running_stats_variance(const dbg_running_stats_t *st);
double dbg_running_stats_stddev(const dbg_running_stats_t *st);
void   dbg_running_stats_reset(dbg_running_stats_t *st);

/* ══════════════════════════════════════════════════════════════════════════
   RING BUFFER
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  *storage;
    uint32_t  element_size;
    uint32_t  capacity;
    uint32_t  head;
    uint32_t  tail;
    uint32_t  count;
} dbg_ring_t;

/**
 * @brief  Initialise a ring buffer with external storage.
 *
 * @param  ring          Ring buffer state.
 * @param  storage       Backing memory (must be ≥ element_size × capacity).
 * @param  element_size  Size of each element in bytes.
 * @param  capacity      Maximum number of elements.
 * @return DBG_OK on success.
 */
dbg_status_t dbg_ring_init(dbg_ring_t *ring,
                           void       *storage,
                           uint32_t    element_size,
                           uint32_t    capacity);

/**
 * @brief  Push an element.  Overwrites oldest if full.
 */
dbg_status_t dbg_ring_push(dbg_ring_t *ring, const void *element);

/**
 * @brief  Pop the oldest element.
 *
 * @return DBG_ERR_OUT_OF_RANGE if empty.
 */
dbg_status_t dbg_ring_pop(dbg_ring_t *ring, void *out);

/**
 * @brief  Peek at an element by index (0 = oldest).
 *
 * @return DBG_ERR_OUT_OF_RANGE if index ≥ count.
 */
dbg_status_t dbg_ring_peek(const dbg_ring_t *ring, uint32_t index,
                           void *out);

/**
 * @brief  Clear all elements (does not zero memory).
 */
void dbg_ring_clear(dbg_ring_t *ring);

/* ══════════════════════════════════════════════════════════════════════════
   LOGGING
   ══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    DBG_LOG_TRACE = 0,
    DBG_LOG_DEBUG = 1,
    DBG_LOG_INFO  = 2,
    DBG_LOG_WARN  = 3,
    DBG_LOG_ERROR = 4,
    DBG_LOG_NONE  = 5,
} dbg_log_level_t;

/**
 * @brief  Log callback signature.
 *
 * @param  level     Severity level.
 * @param  message   Formatted message string.
 * @param  user_ctx  Context pointer.
 */
typedef void (*dbg_log_cb_t)(dbg_log_level_t  level,
                              const char      *message,
                              void            *user_ctx);

/**
 * @brief  Set the minimum log level.  Messages below this are suppressed.
 *         Default: DBG_LOG_WARN.
 */
void dbg_log_set_level(dbg_log_level_t level);

/**
 * @brief  Set a custom log callback.
 *
 * If NULL, messages are printed to stderr with a timestamp.
 */
void dbg_log_set_callback(dbg_log_cb_t cb, void *user_ctx);

/**
 * @brief  Emit a log message.
 *
 * @param  level  Severity.
 * @param  fmt    printf-style format string.
 */
void dbg_log(dbg_log_level_t level, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/** Convenience macros. */
#define DBG_TRACE(...)  dbg_log(DBG_LOG_TRACE, __VA_ARGS__)
#define DBG_DEBUG(...)  dbg_log(DBG_LOG_DEBUG, __VA_ARGS__)
#define DBG_INFO(...)   dbg_log(DBG_LOG_INFO,  __VA_ARGS__)
#define DBG_WARN(...)   dbg_log(DBG_LOG_WARN,  __VA_ARGS__)
#define DBG_ERROR(...)  dbg_log(DBG_LOG_ERROR, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* DBG_UTIL_H */