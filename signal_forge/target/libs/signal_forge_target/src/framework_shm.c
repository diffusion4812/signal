// framework_shm.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "signal_forge_target/framework.h"
#include "task_host_core/task_entry.h"

static framework_shared_t *g_shared = NULL;
static int                 g_shm_fd = -1;
static bool                g_is_owner = false;  // true in supervisor process

// ── Supervisor: create and initialise shared memory ───────────────

int framework_shm_init(void) {
    // Remove any stale region from a previous run
    shm_unlink(SHM_NAME);

    g_shm_fd = shm_open(SHM_NAME,
                         O_CREAT | O_RDWR | O_CLOEXEC,
                         S_IRUSR | S_IWUSR);
    if (g_shm_fd < 0) {
        fprintf(stderr, "[framework] shm_open failed: %s\n", strerror(errno));
        return -1;
    }

    if (ftruncate(g_shm_fd, sizeof(framework_shared_t)) != 0) {
        fprintf(stderr, "[framework] ftruncate failed: %s\n", strerror(errno));
        close(g_shm_fd);
        shm_unlink(SHM_NAME);
        return -1;
    }

    g_shared = mmap(NULL,
                    sizeof(framework_shared_t),
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    g_shm_fd, 0);

    if (g_shared == MAP_FAILED) {
        fprintf(stderr, "[framework] mmap failed: %s\n", strerror(errno));
        close(g_shm_fd);
        shm_unlink(SHM_NAME);
        g_shared = NULL;
        return -1;
    }

    // Lock into RAM — no page faults allowed once tasks start
    if (mlock(g_shared, sizeof(framework_shared_t)) != 0) {
        fprintf(stderr, "[framework] mlock failed: %s — "
                "run as root or set CAP_IPC_LOCK\n", strerror(errno));
        // Not fatal — warn and continue
    }

    // Zero-initialise and write magic
    memset(g_shared, 0, sizeof(framework_shared_t));
    g_shared->magic   = FRAMEWORK_SHM_MAGIC;
    g_shared->version = 1;
    atomic_init(&g_shared->task_count, 0);

    for (int t = 0; t < MAX_TASKS; t++) {

        // Per-task shared block
        task_shared_block_t *blk = &g_shared->task_blocks[t];
        atomic_init(&blk->watchdog_kick, 0);
        memset(&blk->stats, 0, sizeof(blk->stats));

        // SPSC rings — head and tail start at 0
        atomic_init(&blk->mailbox.head,    0);
        atomic_init(&blk->mailbox.tail,    0);

        atomic_init(&g_shared->swap_pending[t], false);
    }

    g_is_owner = true;
    printf("[framework] shared memory initialised: %zu bytes\n",
           sizeof(framework_shared_t));
    return 0;
}

// ── Task process: map existing shared memory ──────────────────────

int framework_task_attach(void) {
    g_shm_fd = shm_open(SHM_NAME,
                         O_RDWR | O_CLOEXEC,
                         S_IRUSR | S_IWUSR);
    if (g_shm_fd < 0) {
        fprintf(stderr, "[framework] task attach: shm_open failed: %s\n",
                strerror(errno));
        return -1;
    }

    g_shared = mmap(NULL,
                    sizeof(framework_shared_t),
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    g_shm_fd, 0);

    if (g_shared == MAP_FAILED) {
        fprintf(stderr, "[framework] task attach: mmap failed: %s\n",
                strerror(errno));
        close(g_shm_fd);
        g_shared = NULL;
        return -1;
    }

    // Validate magic
    if (g_shared->magic != FRAMEWORK_SHM_MAGIC) {
        fprintf(stderr, "[framework] task attach: bad magic 0x%08X\n",
                g_shared->magic);
        munmap(g_shared, sizeof(framework_shared_t));
        close(g_shm_fd);
        g_shared = NULL;
        return -1;
    }

    // Lock this process's mapping into RAM
    if (mlock(g_shared, sizeof(framework_shared_t)) != 0) {
        fprintf(stderr, "[framework] task mlock failed: %s\n",
                strerror(errno));
    }

    g_is_owner = false;
    return 0;
}

void framework_task_detach(void) {
    if (g_shared) {
        munmap(g_shared, sizeof(framework_shared_t));
        g_shared = NULL;
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
        g_shm_fd = -1;
    }
}

void framework_shm_destroy(void) {
    framework_task_detach();
    if (g_is_owner) {
        shm_unlink(SHM_NAME);
        printf("[framework] shared memory destroyed\n");
    }
}

// ── Spawn record management ───────────────────────────────────────

void framework_write_spawn_record(int slot,
                                   const task_manifest_entry_t *manifest) {
    if (!g_shared || slot < 0 || slot >= MAX_TASKS) return;

    task_spawn_record_t *rec = &g_shared->spawn_records[slot];
    rec->manifest = *manifest;
    rec->valid    = true;

    // Ensure task_count reflects the highest slot written
    int current = atomic_load(&g_shared->task_count);
    if (slot + 1 > current)
        atomic_store(&g_shared->task_count, slot + 1);
}

task_spawn_record_t *framework_get_spawn_record(int slot) {
    if (!g_shared || slot < 0 || slot >= MAX_TASKS) return NULL;
    return &g_shared->spawn_records[slot];
}

// ── Supervisor query helpers ──────────────────────────────────────

uint64_t framework_get_watchdog_kick(int slot) {
    if (!g_shared || slot < 0 || slot >= MAX_TASKS) return 0;
    return atomic_load_explicit(&g_shared->task_blocks[slot].watchdog_kick,
                                memory_order_acquire);
}

int framework_get_task_stats(int slot, framework_task_stats_t *out) {
    if (!g_shared || slot < 0 || slot >= MAX_TASKS || !out) return -1;
    // Snapshot the stats struct — no individual atomics here,
    // slight tear is acceptable for monitoring data
    *out = g_shared->task_blocks[slot].stats;
    return 0;
}

framework_shared_t *framework_get_shared(void) {
    return g_shared;
}