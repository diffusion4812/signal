#include "task_runner.h"

static runner_state_t g_runner = {0};

// ── Swap signal — set by SIGUSR1 handler ──────────────────────────
static atomic_bool     g_swap_requested = false;
static volatile sig_atomic_t g_shutdown = 0;

static void sigusr1_handler(int sig) {
    (void)sig;
    atomic_store(&g_swap_requested, true);
}

static void sigterm_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

// ── Compute wrapper — checks swap and shutdown flags ──────────────
static task_result_t compute_wrapper(task_context_t *ctx) {
    if (g_shutdown)         return TASK_SHUTDOWN_REQ;

    if (atomic_load(&g_swap_requested)) {
        atomic_store(&g_swap_requested, false);

        // Read new so_path from shared memory swap record
        task_spawn_record_t *rec =
            framework_get_spawn_record(ctx->desc->task_slot);
        if (rec && strlen(rec->manifest.so_path) > 0) {
            hot_swap(&g_runner, rec->manifest.so_path);
        }
        // Resume — compute will run the new code next cycle
        return TASK_OK;
    }

    return g_runner.entry->compute(&ctx->slot_ctx);
}

// ── Shutdown hook injected by framework ───────────────────────────
// The task's compute() checks g_shutdown via this wrapper.
// We wrap the real compute so task authors don't need to poll a flag.

static task_result_t (*g_real_compute)(task_slot_ctx_t*) = NULL;

// ── Initial load — same logic as hot_swap first time ─────────────
static int initial_load(runner_state_t *r, const char *so_path) {
    r->dl_handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!r->dl_handle) {
        fprintf(stderr, "[task_runner] dlopen failed: %s\n", dlerror());
        return -1;
    }

    r->entry = dlsym(r->dl_handle, "task_entry");
    if (!r->entry || !r->entry->reg || !r->entry->compute) {
        fprintf(stderr, "[task_runner] invalid task_entry\n");
        dlclose(r->dl_handle);
        return -1;
    }

    uint64_t count = 0;
    const block_reg_entry_t *reg = r->entry->reg(&count);

    r->reg             = calloc(count, sizeof(block_reg_entry_t));
    r->slot_ctx.slots  = calloc(count, sizeof(void *));
    r->slot_ctx.count  = count;
    r->allocated_slots = count;

    for (uint64_t i = 0; i < count; i++) {
        memcpy(&r->reg[i], &reg[i], sizeof(block_reg_entry_t));
        r->slot_ctx.slots[i] = calloc(1, reg[i].block_size);
        printf("[task_runner] block '%s' allocated (%zu bytes)\n",
               reg[i].block_name, reg[i].block_size);
    }

    if (r->entry->init)
        r->entry->init(&r->slot_ctx);

    return 0;
}

// ── hot_swap — in-process, preserves/migrates state ──────────────
void hot_swap(runner_state_t *r, const char *so_path) {
    printf("[task_runner] hot swap: loading %s\n", so_path);

    void *new_handle = dlopen(so_path,
                               RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!new_handle) {
        fprintf(stderr, "[task_runner] dlopen failed: %s\n", dlerror());
        return;
    }

    const task_entry_t *new_entry = dlsym(new_handle, "task_entry");
    if (!new_entry || !new_entry->reg || !new_entry->compute) {
        fprintf(stderr, "[task_runner] invalid task_entry in new .so\n");
        dlclose(new_handle);
        return;
    }

    uint64_t new_count = 0;
    const block_reg_entry_t *new_reg = new_entry->reg(&new_count);

    // Allocate new registry copy and slot array
    block_reg_entry_t *new_reg_copy  = calloc(new_count,
                                               sizeof(block_reg_entry_t));
    void             **new_slots     = calloc(new_count, sizeof(void *));
    uint8_t          *old_preserved  = calloc(r->allocated_slots,
                                               sizeof(uint8_t));

    if (!new_reg_copy || !new_slots || !old_preserved) {
        fprintf(stderr, "[task_runner] allocation failed during hot swap\n");
        free(new_reg_copy);
        free(new_slots);
        free(old_preserved);
        dlclose(new_handle);
        return;
    }

    // ── Match new blocks against existing blocks ──────────────────
    for (uint64_t n = 0; n < new_count; n++) {
        const block_reg_entry_t *nb = &new_reg[n];
        memcpy(&new_reg_copy[n], nb, sizeof(block_reg_entry_t));

        // Find matching block in old registry by ID
        int64_t old_idx = -1;
        for (uint64_t e = 0; e < r->allocated_slots; e++) {
            if (r->reg[e].block_id == nb->block_id) {
                old_idx = (int64_t)e;
                break;
            }
        }

        if (old_idx >= 0) {
            block_reg_entry_t *ob  = &r->reg[old_idx];
            void              *old = r->slot_ctx.slots[old_idx];

            if (nb->block_sig == ob->block_sig) {
                // ── Exact match: carry state pointer forward ──────
                printf("[task_runner] block '%s': signature match "
                       "— state preserved\n", nb->block_name);
                new_slots[n]           = old;
                old_preserved[old_idx] = 1;

            } else {
                // ── Layout changed: allocate new, try to migrate ──
                void *new_ctx = calloc(1, nb->block_size);
                if (!new_ctx) {
                    fprintf(stderr, "[task_runner] OOM for block '%s'\n",
                            nb->block_name);
                    // Clean up and abort
                    for (uint64_t k = 0; k < n; k++)
                        if (!old_preserved[k]) free(new_slots[k]);
                    free(new_reg_copy);
                    free(new_slots);
                    free(old_preserved);
                    dlclose(new_handle);
                    return;
                }

                if (new_entry->migrate &&
                    new_entry->migrate(ob->block_sig, old, new_ctx)) {
                    printf("[task_runner] block '%s': migrated "
                           "0x%016lX → 0x%016lX\n",
                           nb->block_name, ob->block_sig, nb->block_sig);
                } else {
                    printf("[task_runner] block '%s': no migration — "
                           "state reset\n", nb->block_name);
                }

                new_slots[n]           = new_ctx;
                old_preserved[old_idx] = 0;
            }

        } else {
            // ── Brand new block: allocate fresh ───────────────────
            printf("[task_runner] block '%s': new — allocated\n", nb->block_name);
            new_slots[n] = calloc(1, nb->block_size);
        }
    }

    // ── Free old blocks not present in new registry ───────────────
    for (uint64_t e = 0; e < r->allocated_slots; e++) {
        if (!old_preserved[e] && r->slot_ctx.slots[e]) {
            free(r->slot_ctx.slots[e]);
        }
    }

    // ── Commit swap ───────────────────────────────────────────────
    free(r->reg);
    free(r->slot_ctx.slots);
    free(old_preserved);

    r->reg             = new_reg_copy;
    r->slot_ctx.slots  = new_slots;
    r->slot_ctx.count  = new_count;
    r->allocated_slots = new_count;

    void *old_handle = r->dl_handle;
    r->dl_handle     = new_handle;
    r->entry         = new_entry;

    dlclose(old_handle);    // safe now — all state is in heap, not .so

    // Update compute wrapper
    r->desc.wrapped_compute = compute_wrapper;   // still wraps new entry->compute

    // Update slot_ctx in context so compute() sees new blocks
    r->ctx.slot_ctx = r->slot_ctx;

    // Call init on new library — rebuilds OD pointers etc.
    if (new_entry->init)
        new_entry->init(&r->slot_ctx);

    printf("[task_runner] hot swap complete: %s\n", so_path);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: task_runner <slot>\n");
        return 1;
    }

    int slot = atoi(argv[1]);
    if (slot < 0 || slot >= MAX_TASKS) {
        fprintf(stderr, "[task_runner] invalid slot: %d\n", slot);
        return 1;
    }

    // ── Signal handlers ───────────────────────────────────────────
    struct sigaction sa_usr1 = { .sa_handler = sigusr1_handler };
    struct sigaction sa_term = { .sa_handler = sigterm_handler };
    sigemptyset(&sa_usr1.sa_mask);
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGUSR1, &sa_usr1, NULL);
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);
    signal(SIGPIPE, SIG_IGN);

    // ── Attach shared memory ──────────────────────────────────────
    if (framework_task_attach() != 0) {
        fprintf(stderr, "[task_runner:%d] failed to attach shared memory\n",
                slot);
        return 1;
    }

    // ── Read spawn record ─────────────────────────────────────────
    task_spawn_record_t *rec = framework_get_spawn_record(slot);
    if (!rec || !rec->valid) {
        fprintf(stderr, "[task_runner:%d] spawn record not valid\n", slot);
        framework_task_detach();
        return 1;
    }

    const task_manifest_entry_t *m = &rec->manifest;

    printf("[task_runner:%d] '%s'  so=%s  period=%uus  cpu=%d  prio=%d\n",
           slot, m->name, m->so_path,
           m->period_us, m->cpu_affinity, m->sched_priority);

    // ── Initial load via block registry ──────────────────────────
    // initial_load() allocates all state blocks, calls migrate() if
    // signatures differ from a previous run, then calls init().
    // This is the same path hot_swap() uses — one code path for both.
    memset(&g_runner, 0, sizeof(g_runner));

    if (initial_load(&g_runner, m->so_path) != 0) {
        fprintf(stderr, "[task_runner:%d] initial_load failed\n", slot);
        framework_task_detach();
        return 1;
    }

    // ── Build descriptor ──────────────────────────────────────────
    // Manifest provides identity and scheduling.
    // .so provides compute, OD table, and block registry.
    // OD table pointers are valid after init() has run — init()
    // calls build_od() which resolves block pointers into od_entry_t.
    g_runner.desc = (task_descriptor_t){
        .id               = m->id,
        .name             = m->name,
        .period_us        = m->period_us,
        .cpu_affinity     = m->cpu_affinity,
        .sched_priority   = m->sched_priority,
        .task_slot        = slot,

        // compute_wrapper checks g_shutdown each cycle
        .wrapped_compute  = compute_wrapper
    };

    // ── Build context ─────────────────────────────────────────────
    framework_context_init(&g_runner.ctx, &g_runner.desc);

    // Wire block pointers into context so compute() can reach state
    g_runner.ctx.slot_ctx = g_runner.slot_ctx;

    // ── Run ───────────────────────────────────────────────────────
    // Blocks here until compute_wrapper returns TASK_SHUTDOWN_REQ.
    // Shutdown is triggered by SIGTERM → g_shutdown = 1.
    framework_task_runner(&g_runner.ctx);

    // ── Teardown ──────────────────────────────────────────────────
    printf("[task_runner:%d] '%s' exited after %llu cycles  "
           "overruns=%llu  max_exec=%uus\n",
           slot, m->name,
           (unsigned long long)g_runner.ctx.stats->cycle_count,
           (unsigned long long)g_runner.ctx.stats->overruns,
           g_runner.ctx.stats->max_exec_us);

    // Free all state blocks
    for (uint64_t i = 0; i < g_runner.allocated_slots; i++) {
        if (g_runner.slot_ctx.slots[i]) {
            free(g_runner.slot_ctx.slots[i]);
            g_runner.slot_ctx.slots[i] = NULL;
        }
    }
    free(g_runner.slot_ctx.slots);
    free(g_runner.reg);

    dlclose(g_runner.dl_handle);
    framework_task_detach();
    return 0;
}