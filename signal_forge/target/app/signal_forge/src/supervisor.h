// supervisor.h
#pragma once

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include "task_host_core/project.h"

// ── Process states ────────────────────────────────────────────────
typedef enum {
    PROC_STATE_IDLE        = 0,
    PROC_STATE_SPAWNING,
    PROC_STATE_RUNNING,
    PROC_STATE_STOPPING,
    PROC_STATE_STOPPED,
    PROC_STATE_CRASHED,
    PROC_STATE_SWAP_PENDING,
} proc_state_t;

static inline const char *proc_state_str(proc_state_t s) {
    switch (s) {
        case PROC_STATE_IDLE:         return "IDLE";
        case PROC_STATE_SPAWNING:     return "SPAWNING";
        case PROC_STATE_RUNNING:      return "RUNNING";
        case PROC_STATE_STOPPING:     return "STOPPING";
        case PROC_STATE_STOPPED:      return "STOPPED";
        case PROC_STATE_CRASHED:      return "CRASHED";
        case PROC_STATE_SWAP_PENDING: return "SWAP_PENDING";
        default:                      return "UNKNOWN";
    }
}

// ── Restart policy ────────────────────────────────────────────────
typedef enum {
    RESTART_NEVER  = 0,    // crashed task stays down
    RESTART_ALWAYS,        // always restart on crash
    RESTART_LIMITED,       // restart up to max_restarts times
} restart_policy_t;

// ── Per-process record ────────────────────────────────────────────
typedef struct {
    pid_t               pid;
    int                 slot;
    proc_state_t        state;
    task_manifest_entry_t manifest;

    // Watchdog
    uint64_t            last_kick;
    uint64_t            missed_kicks;
    uint32_t            watchdog_timeout_ms;    // 0 = disabled

    // Restart tracking
    restart_policy_t    restart_policy;
    uint32_t            restart_count;
    uint32_t            max_restarts;
    struct timespec     last_start_time;
    struct timespec     last_crash_time;

    // Swap
    char                pending_so_path[MAX_PATH_LEN];
    bool                swap_on_next_stop;

} task_process_t;

// ── Supervisor configuration ──────────────────────────────────────
typedef struct {
    const char *runner_path;            // path to task_runner binary
    const char *task_dir;               // where .so files live
    uint32_t    watchdog_interval_ms;   // how often to check kicks
    uint32_t    watchdog_timeout_ms;    // missed time before action
    uint32_t    spawn_stagger_ms;       // delay between task spawns
    uint32_t    ec_settle_ms;           // wait after EtherCAT before tasks
} supervisor_config_t;

// ── Supervisor state transitions ──────────────────────────────────
typedef enum {
    SUPERVISOR_STOPPED = 0,
    SUPERVISOR_STARTING,
    SUPERVISOR_RUNNING,
    SUPERVISOR_STOPPING,
} supervisor_state_t;

// ── Callback types (used by TCP receiver to call into supervisor) ─
typedef void (*supervisor_swap_cb_t)    (int slot, const char *new_so_path);
typedef void (*supervisor_load_cb_t)    (const char *project_path);
typedef void (*supervisor_shutdown_cb_t)(void);

// ── Public API ────────────────────────────────────────────────────
int  supervisor_init          (const supervisor_config_t *cfg);
int  supervisor_load_project  (const char *project_path);
int  supervisor_start_all     (void);
void supervisor_run           (void);         // blocks until shutdown
void supervisor_shutdown_all  (void);
void supervisor_cleanup       (void);

// Task control — safe to call from TCP receiver thread
int  supervisor_start_task    (int slot);
int  supervisor_stop_task     (int slot);
int  supervisor_restart_task  (int slot);
void supervisor_request_swap  (int slot, const char *new_so_path);
void supervisor_request_load  (const char *project_path);
void supervisor_request_shutdown(void);

// Query — safe to call from any thread
bool     supervisor_task_is_running (uint8_t id);
uint32_t supervisor_task_period     (uint8_t id);
int      supervisor_slot_for_id     (uint8_t id);
void     supervisor_get_status      (supervisor_status_t *out);

// Called from SIGCHLD handler
void supervisor_notify_child_exit   (pid_t pid, int status);