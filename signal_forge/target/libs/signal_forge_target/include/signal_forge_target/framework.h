// framework.h
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>
#include "task_host_core/project.h"
#include "task_host_core/manifest.h"
#include "task_host_core/task_entry.h"

// ── Sizing constants ──────────────────────────────────────────────
#define MAX_TASKS               16
#define MAX_ANALOG_IN           8
#define MAX_ANALOG_OUT          8
#define MAX_ENCODERS            4
#define PDO_INPUT_SIZE          128
#define PDO_OUTPUT_SIZE         128
#define MAILBOX_DEPTH           64      // must be power of 2
#define DEBUG_RING_DEPTH        512     // must be power of 2
#define OD_REQ_RING_DEPTH       16      // must be power of 2
#define MAX_PAYLOAD_SIZE        128
#define TASK_USER_DATA_MAX      4096
#define MAX_OD_ENTRIES          64
#define SHM_NAME                "/signal_forge_framework"

// ── Telegram ──────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  source_id;
    uint8_t  dest_id;
    uint8_t  flags;
    uint16_t telegram_id;
    uint16_t sequence;
    uint32_t timestamp_us;
    uint16_t payload_len;
    uint8_t  payload[MAX_PAYLOAD_SIZE];
    uint16_t crc;
} telegram_t;

// ── Object dictionary ─────────────────────────────────────────────
typedef enum { OD_TYPE_U8, OD_TYPE_U16, OD_TYPE_U32,
               OD_TYPE_S8, OD_TYPE_S16, OD_TYPE_S32,
               OD_TYPE_F32, OD_TYPE_F64, OD_TYPE_BOOL } od_type_t;

typedef enum { OD_ACCESS_RW, OD_ACCESS_RO, OD_ACCESS_WO } od_access_t;

typedef struct {
    uint16_t     index;
    uint8_t      subindex;
    od_type_t    type;
    od_access_t  access;
    void        *ptr;           // direct pointer — valid in task's process only
    size_t       size;
    const char  *name;
    bool         publish;       // include in debug stream
} od_entry_t;

// ── OD request/response (cross-process via shared ring) ───────────
typedef struct {
    uint16_t  index;
    uint8_t   subindex;
    uint8_t   task_id;
    bool      is_write;
    uint8_t   data[8];
    uint32_t  request_id;       // echoed in response for matching
} od_request_t;

typedef struct {
    uint32_t  request_id;
    bool      success;
    uint8_t   error_code;
    uint8_t   data[8];
    od_type_t type;
    size_t    size;
} od_response_t;

// ── Debug sample ──────────────────────────────────────────────────
typedef struct {
    uint16_t    index;
    uint8_t     subindex;
    uint8_t     task_id;
    uint32_t    timestamp_us;
    od_type_t   type;
    uint8_t     data[8];
} debug_sample_t;

// ── SPSC ring — generic, index-based, lock-free ───────────────────
// Instantiated separately for each element type via macros below.

#define DECLARE_SPSC_RING(NAME, TYPE, DEPTH)                            \
typedef struct {                                                        \
    TYPE     slots[DEPTH];                                              \
    _Alignas(64) atomic_size_t head;                                    \
    _Alignas(64) atomic_size_t tail;                                    \
} NAME##_ring_t;                                                        \
                                                                        \
static inline bool NAME##_ring_push(NAME##_ring_t *r, const TYPE *v) {  \
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed); \
    size_t next = (tail + 1) & (DEPTH - 1);                             \
    if (next == atomic_load_explicit(&r->head, memory_order_acquire))   \
        return false;   /* full */                                      \
    r->slots[tail] = *v;                                                \
    atomic_store_explicit(&r->tail, next, memory_order_release);        \
    return true;                                                        \
}                                                                       \
                                                                        \
static inline bool NAME##_ring_pop(NAME##_ring_t *r, TYPE *v) {         \
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed); \
    if (head == atomic_load_explicit(&r->tail, memory_order_acquire))   \
        return false;   /* empty */                                     \
    *v = r->slots[head];                                                \
    atomic_store_explicit(&r->head,                                     \
        (head + 1) & (DEPTH - 1), memory_order_release);                \
    return true;                                                        \
}

DECLARE_SPSC_RING(mailbox,   telegram_t,   MAILBOX_DEPTH)

// ── Spawn record — written by supervisor, read by task_runner ─────
typedef struct {
    task_manifest_entry_t   manifest;
    bool                    valid;
} task_spawn_record_t;

// ── Per-task shared block ─────────────────────────────────────────
typedef struct task_shared_block task_shared_block_t;
struct task_shared_block {
    // Watchdog — task increments, supervisor reads
    _Alignas(64) atomic_uint_fast64_t watchdog_kick;

    // Stats — task writes, supervisor/host reads
    _Alignas(64) framework_task_stats_t stats;

    // Mailbox — TCP gateway pushes telegrams, task pops them
    _Alignas(64) mailbox_ring_t mailbox;
};

// ── Top-level shared memory region ───────────────────────────────
typedef struct {
    uint32_t            magic;              // 0x5346524D "SFRM"
    uint32_t            version;
    atomic_int          task_count;

    task_spawn_record_t spawn_records [MAX_TASKS];
    task_shared_block_t task_blocks   [MAX_TASKS];
    atomic_bool         swap_pending  [MAX_TASKS];
} framework_shared_t;

#define FRAMEWORK_SHM_MAGIC  0x5346524D


// ─────────────────────────────────────────────────────────────────
// Public API — called by supervisor (host process)
// ─────────────────────────────────────────────────────────────────
int  framework_shm_init    (void);
void framework_shm_destroy (void);

void framework_write_spawn_record(int slot,
                                  const task_manifest_entry_t *manifest);
task_spawn_record_t *framework_get_spawn_record(int slot);

uint64_t framework_get_watchdog_kick(int slot);
int      framework_get_task_stats   (int slot, framework_task_stats_t *out);

framework_shared_t *framework_get_shared(void);

// ─────────────────────────────────────────────────────────────────
// Public API — called by task_runner (task process)
// ─────────────────────────────────────────────────────────────────
int  framework_task_attach (void);   // map existing shm
void framework_task_detach (void);

void framework_context_init(task_context_t *ctx,
                             const task_descriptor_t *desc);

void framework_task_runner (task_context_t *ctx); // blocks until shutdown

// Called inside compute() to send/receive telegrams
bool task_recv (task_context_t *ctx, telegram_t *out);
bool task_send (task_context_t *ctx, uint8_t dest,
                uint16_t tid, const void *payload, uint16_t len);

// ─────────────────────────────────────────────────────────────────
// Internal framework steps — not called directly by task authors
// ─────────────────────────────────────────────────────────────────
void framework_take_snapshot  (task_context_t *ctx);
void framework_flush_outputs  (task_context_t *ctx);

// ── Utilities ─────────────────────────────────────────────────────
uint32_t framework_now_us     (void);
void     timespec_add_us      (struct timespec *ts, uint32_t us);
uint32_t timespec_diff_us     (const struct timespec *a,
                                const struct timespec *b);