// project.h
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_TASKS_PER_PROJECT   16
#define MAX_PATH_LEN            256
#define MAX_NAME_LEN            64

// ── Per-task manifest entry ───────────────────────────────────────
typedef struct {
    char        name      [MAX_NAME_LEN];
    char        so_path   [MAX_PATH_LEN];
    uint8_t     id;
    uint32_t    period_us;
    int         cpu_affinity;
    int         sched_priority;
    bool        allow_hot_swap;         // default false — must be explicitly enabled
    uint32_t    watchdog_timeout_ms;    // 0 = disabled for this task
    uint8_t     restart_policy;         // restart_policy_t
    uint32_t    max_restarts;
    bool        enabled;
    bool        is_ethercat;            // true for the EtherCAT task
} task_manifest_entry_t;

// ── Project manifest ──────────────────────────────────────────────
typedef struct {
    char                  name       [MAX_NAME_LEN];
    char                  description[MAX_NAME_LEN];
    uint32_t              version;
    uint32_t              ec_period_us;
    int                   ec_cpu_affinity;
    size_t                task_count;
    task_manifest_entry_t tasks[MAX_TASKS_PER_PROJECT];
} project_manifest_t;

// ── Status snapshot (for host queries) ───────────────────────────
typedef struct {
    char         name[MAX_NAME_LEN];
    uint8_t    id;
    uint8_t      proc_state;        // proc_state_t
    pid_t        pid;
    uint32_t     restart_count;
    uint64_t     cycle_count;
    uint32_t     last_exec_us;
    uint32_t     max_exec_us;
    uint64_t     overruns;
    uint64_t     watchdog_kicks;
    uint32_t     debug_drops;
} task_status_t;

typedef struct {
    char          project_name[MAX_NAME_LEN];
    uint8_t       supervisor_state;     // supervisor_state_t
    size_t        task_count;
    task_status_t tasks[MAX_TASKS_PER_PROJECT];
} supervisor_status_t;

// ── Project load/save ─────────────────────────────────────────────
int  project_load  (const char *path, project_manifest_t *out);
int  project_save  (const char *path, const project_manifest_t *manifest);
void project_print (const project_manifest_t *manifest);
int  project_validate(const project_manifest_t *manifest);