// sf_target.h
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SF_TARGET_VERSION_MAJOR  1
#define SF_TARGET_VERSION_MINOR  0
#define SF_TARGET_DEFAULT_PORT   7600

// ── Target configuration — populated from argv ────────────────────
typedef struct {
    uint16_t    port;               // TCP control port
    const char *project_path;       // optional — NULL if none provided
    const char *task_dir;           // where .so files are written after transfer
    const char *runner_path;        // path to task_runner executable
    bool        autostart;          // start tasks immediately after project load
} sf_target_config_t;

// ── Argument parsing ──────────────────────────────────────────────
int  parse_args   (int argc, char *argv[], sf_target_config_t *cfg);
void print_usage  (const char *prog);

// ── Runtime environment checks ────────────────────────────────────
int  sf_check_rt_environment(void);

// ── Banner ────────────────────────────────────────────────────────
void print_banner(void);