#pragma once

#include "manifest.h"

// ── Task descriptor — filled in by task author ────────────────────
typedef enum {
    TASK_OK, TASK_WARN, TASK_ERROR, TASK_SHUTDOWN_REQ
} task_result_t;
typedef block_reg_entry_t*  (*task_reg_t)(uint64_t*);

typedef struct task_context task_context_t;
typedef struct task_shared_block task_shared_block_t;

// ── Slot context — passed to init and available via task_context ──
// Holds all allocated block pointers for the task
typedef struct {
    void    **slots;            // array of block pointers
    uint64_t  count;
} task_slot_ctx_t;

typedef struct {
    uint8_t      id;
    const char  *name;
    uint32_t     period_us;
    int          cpu_affinity;
    int          sched_priority;
    int          task_slot;

    task_result_t (*init)            (task_slot_ctx_t *ctx);
    task_result_t (*wrapped_compute) (task_context_t  *ctx);
    task_result_t (*shutdown)        (task_slot_ctx_t *ctx);
    void          (*on_error)        (task_slot_ctx_t *ctx, task_result_t err);

    block_reg_entry_t*   reg;
    uint64_t             allocated_slots;
} task_descriptor_t;

// ── Per-task stats — written by task, readable by supervisor ──────
typedef struct {
    uint64_t cycle_count;
    uint64_t overruns;
    uint32_t last_exec_us;
    uint32_t max_exec_us;
    uint64_t mailbox_drops;
    uint64_t debug_drops;
    uint64_t watchdog_kicks;
} framework_task_stats_t;

// ── Task context — passed into every hook ─────────────────────────
struct task_context {
    const task_descriptor_t *desc;
    uint8_t                  state;     // task_state_t

    task_slot_ctx_t          slot_ctx;  // pointer to block pointers
    task_shared_block_t     *shared;    // pointer into shared memory block
    framework_task_stats_t  *stats;     // same as &shared->stats

    // Internal framework state
    uint32_t                 _sequence; // outbound telegram sequence counter
};

typedef int                (*migrate_func_t)  (uint64_t   old_signature,
                                                void      *old_ctx,
                                                void      *new_ctx);

typedef struct {
    // Lifecycle — only compute is required
    task_result_t (*init)    (task_slot_ctx_t *ctx);   // optional
    task_result_t (*compute) (task_slot_ctx_t *ctx);   // REQUIRED
    task_result_t (*shutdown)(task_slot_ctx_t *ctx);   // optional
    migrate_func_t  migrate;                           // optional
    void          (*on_error)(task_slot_ctx_t *ctx,
                              task_result_t err);      // optional

    // SF slot definitions (blocks/statics)
    const block_reg_entry_t *(*reg)(uint64_t *);
    uint64_t             allocated_slots;
} task_entry_t;

extern const task_entry_t task_entry;