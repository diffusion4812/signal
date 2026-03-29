// framework_runner.c — RT task runner loop and context init

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sched.h>
#include <stdatomic.h>
#include "signal_forge_target/framework.h"

// ── Task state ────────────────────────────────────────────────────
typedef enum {
    TASK_STATE_IDLE,
    TASK_STATE_INIT,
    TASK_STATE_RUNNING,
    TASK_STATE_ERROR,
    TASK_STATE_SHUTDOWN,
} task_state_t;

// ── Utilities ─────────────────────────────────────────────────────

uint32_t framework_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000ULL
                    + (uint64_t)ts.tv_nsec / 1000ULL);
}

void timespec_add_us(struct timespec *ts, uint32_t us) {
    ts->tv_nsec += (long)us * 1000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec  += ts->tv_nsec / 1000000000L;
        ts->tv_nsec  = ts->tv_nsec % 1000000000L;
    }
}

uint32_t timespec_diff_us(const struct timespec *start,
                           const struct timespec *end) {
    int64_t diff = ((int64_t)end->tv_sec  - (int64_t)start->tv_sec)  * 1000000LL
                 + ((int64_t)end->tv_nsec - (int64_t)start->tv_nsec) / 1000LL;
    return diff > 0 ? (uint32_t)diff : 0;
}

// ── RT thread setup ───────────────────────────────────────────────

static int setup_rt(const task_descriptor_t *desc) {
    // CPU affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(desc->cpu_affinity, &cpuset);

    if (pthread_setaffinity_np(pthread_self(),
                                sizeof(cpuset), &cpuset) != 0) {
        fprintf(stderr, "[runner:%s] setaffinity cpu=%d failed: %s\n",
                desc->name, desc->cpu_affinity, strerror(errno));
        return -1;
    }

    // SCHED_FIFO priority
    struct sched_param sp = { .sched_priority = desc->sched_priority };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        fprintf(stderr, "[runner:%s] SCHED_FIFO prio=%d failed: %s "
                "(run as root or set CAP_SYS_NICE)\n",
                desc->name, desc->sched_priority, strerror(errno));
        return -1;
    }

    // Lock all current and future memory — no page faults during RT
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "[runner:%s] mlockall failed: %s\n",
                desc->name, strerror(errno));
        return -1;
    }

    return 0;
}

// ── Context initialisation ────────────────────────────────────────

void framework_context_init(task_context_t       *ctx,
                             const task_descriptor_t *desc) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->desc   = desc;
    ctx->state  = TASK_STATE_IDLE;

    // Wire up shared memory block for this slot
    framework_shared_t *shared = framework_get_shared();
    ctx->shared = &shared->task_blocks[desc->task_slot];
    ctx->stats  = &ctx->shared->stats;
}

// ── Main RT runner loop — blocks until shutdown ───────────────────

void framework_task_runner(task_context_t *ctx) {
    const task_descriptor_t *desc = ctx->desc;

    // ── RT setup ──────────────────────────────────────────────────
    if (setup_rt(desc) != 0) {
        fprintf(stderr, "[runner:%s] RT setup failed — aborting\n",
                desc->name);
        return;
    }

    printf("[runner:%s] RT configured: cpu=%d  prio=%d  period=%uus\n",
           desc->name, desc->cpu_affinity,
           desc->sched_priority, desc->period_us);

    // ── Init hook ─────────────────────────────────────────────────
    ctx->state = TASK_STATE_INIT;

    if (desc->init) {
        task_result_t rc = desc->init(&ctx->slot_ctx);
        if (rc != TASK_OK) {
            fprintf(stderr, "[runner:%s] init() failed (%d)\n",
                    desc->name, rc);
            ctx->state = TASK_STATE_ERROR;
            if (desc->on_error) desc->on_error(&ctx->slot_ctx, rc);
            return;
        }
    }

    ctx->state = TASK_STATE_RUNNING;
    printf("[runner:%s] entering RT loop\n", desc->name);

    // ── Absolute time base ────────────────────────────────────────
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    // ── RT loop ───────────────────────────────────────────────────
    while (ctx->state == TASK_STATE_RUNNING) {

        // ── 1. WAIT until next period boundary ───────────────────
        int wait_rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                      &next, NULL);
        if (wait_rc != 0 && wait_rc != EINTR) {
            fprintf(stderr, "[runner:%s] clock_nanosleep error: %s\n",
                    desc->name, strerror(wait_rc));
        }

        // Advance deadline — done immediately after wake to keep
        // timing independent of how long compute takes
        timespec_add_us(&next, desc->period_us);

        struct timespec t_start;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        // ── 3. COMPUTE: task author's code ────────────────────────
        task_result_t result = desc->wrapped_compute(ctx);

        // ── 7. WATCHDOG: kick the supervisor ──────────────────────
        atomic_fetch_add_explicit(&ctx->shared->watchdog_kick, 1,
                                   memory_order_release);
        ctx->stats->watchdog_kicks++;

        // ── 8. STATS ──────────────────────────────────────────────
        struct timespec t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_end);

        uint32_t exec_us = timespec_diff_us(&t_start, &t_end);

        ctx->stats->last_exec_us = exec_us;
        ctx->stats->cycle_count++;

        if (exec_us > ctx->stats->max_exec_us)
            ctx->stats->max_exec_us = exec_us;

        if (exec_us > desc->period_us) {
            ctx->stats->overruns++;
            fprintf(stderr, "[runner:%s] OVERRUN: exec=%uus period=%uus\n",
                    desc->name, exec_us, desc->period_us);
        }

        // ── 9. RESULT handling ────────────────────────────────────
        if (result == TASK_SHUTDOWN_REQ) {
            printf("[runner:%s] compute() requested shutdown\n", desc->name);
            ctx->state = TASK_STATE_SHUTDOWN;
        } else if (result == TASK_ERROR) {
            ctx->stats->overruns++;   // reuse field for error count
            if (desc->on_error) desc->on_error(&ctx->slot_ctx, result);
            // on_error may set ctx->state = TASK_STATE_SHUTDOWN
        }
    }

    // ── Shutdown hook ─────────────────────────────────────────────
    printf("[runner:%s] shutting down after %llu cycles "
           "(overruns=%llu  max_exec=%uus)\n",
           desc->name,
           (unsigned long long)ctx->stats->cycle_count,
           (unsigned long long)ctx->stats->overruns,
           ctx->stats->max_exec_us);

    if (desc->shutdown) desc->shutdown(&ctx->slot_ctx);
}