// task_runner.c — generic per-task executable
// Spawned once per task by signal_forge_target supervisor.
// argv[1] = slot index into shared spawn_records[].

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include "task_host_core/task_entry.h"
#include "signal_forge_target/framework.h"

typedef struct {
    void              *dl_handle;
    const task_entry_t *entry;

    // Block registry and allocated state
    block_reg_entry_t *reg;
    task_slot_ctx_t    slot_ctx;
    uint64_t           allocated_slots;

    task_descriptor_t  desc;
    task_context_t     ctx;
} runner_state_t;

static task_result_t compute_wrapper(task_context_t *ctx);
void hot_swap(runner_state_t *r, const char *so_path);