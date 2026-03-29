// sf_receiver.h
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "task_host_core/project.h"

// ── Callbacks into supervisor ─────────────────────────────────────
typedef void (*sf_on_swap_cb_t)     (int slot, const char *so_path);
typedef void (*sf_on_load_cb_t)     (const char *project_path);
typedef void (*sf_on_shutdown_cb_t) (void);

typedef struct {
    uint16_t             port;
    const char          *task_dir;          // where .so files are written
    sf_on_swap_cb_t      on_swap;
    sf_on_load_cb_t      on_load_project;
    sf_on_shutdown_cb_t  on_shutdown;
} sf_receiver_config_t;

// ── Lifecycle ─────────────────────────────────────────────────────
int  sf_receiver_start (const sf_receiver_config_t *cfg);
void sf_receiver_stop  (void);

// ── Send helpers — callable from any thread ───────────────────────
// Used by debug session manager and OD poll thread to push
// responses back to the connected host.
bool sf_send_packet    (uint16_t command,
                        const void *payload, uint32_t payload_len);