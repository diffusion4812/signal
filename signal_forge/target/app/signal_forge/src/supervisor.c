// supervisor.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <stdatomic.h>

#include "supervisor.h"
#include "task_host_core/project.h"

// ── Internal state ────────────────────────────────────────────────

typedef struct {
    // Pending signals from SIGCHLD — written by signal handler,
    // read by supervisor loop.  Atomic to avoid async-signal races.
    struct {
        _Alignas(64) atomic_int  pid;      // pid that exited, or 0
        _Alignas(64) atomic_int  status;   // waitpid status
    } child_exits[MAX_TASKS_PER_PROJECT];
    atomic_int child_exit_head;
    atomic_int child_exit_tail;
} supervisor_signal_state_t;

typedef struct {
    supervisor_config_t         config;
    project_manifest_t          project;
    task_process_t              tasks   [MAX_TASKS_PER_PROJECT];
    size_t                      task_count;
    supervisor_state_t          state;

    // Requests from TCP receiver thread — atomic flags
    atomic_bool                 shutdown_requested;
    atomic_bool                 load_requested;
    char                        load_requested_path[MAX_PATH_LEN];

    supervisor_signal_state_t   sig_state;
    pthread_mutex_t             lock;       // protects task[] mutations
} supervisor_t;

static supervisor_t g_sv = {0};

// ── Helpers ───────────────────────────────────────────────────────

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void sleep_ms(uint32_t ms) {
    struct timespec ts = {
        .tv_sec  =  ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

static task_process_t *find_task_by_pid(pid_t pid) {
    for (size_t i = 0; i < g_sv.task_count; i++) {
        if (g_sv.tasks[i].pid == pid) return &g_sv.tasks[i];
    }
    return NULL;
}

static task_process_t *find_task_by_id(uint8_t id) {
    for (size_t i = 0; i < g_sv.task_count; i++) {
        if (g_sv.tasks[i].manifest.id == id) return &g_sv.tasks[i];
    }
    return NULL;
}

static task_process_t *find_task_by_slot(int slot) {
    if (slot < 0 || slot >= (int)g_sv.task_count) return NULL;
    return &g_sv.tasks[slot];
}

// ── Spawn a single task process ───────────────────────────────────

static int spawn_task(task_process_t *tp) {
    char slot_str[16];
    snprintf(slot_str, sizeof(slot_str), "%d", tp->slot);

    clock_gettime(CLOCK_MONOTONIC, &tp->last_start_time);
    tp->state = PROC_STATE_SPAWNING;

    pid_t pid = fork();

    if (pid < 0) {
        fprintf(stderr, "[supervisor] fork() failed for '%s': %s\n",
                tp->manifest.name, strerror(errno));
        tp->state = PROC_STATE_CRASHED;
        return -1;
    }

    if (pid == 0) {
        // ── Child process ─────────────────────────────────────────
        // All file descriptors created without O_CLOEXEC must be
        // explicitly closed here before exec.
        // Shared memory fds are mapped — the mapping survives exec
        // but the fd is closed (opened with O_CLOEXEC in shm_open).

        execl(g_sv.config.runner_path,
              "task_runner",
              slot_str,
              NULL);

        // Only reached on failure
        fprintf(stderr, "[task_runner] execl failed: %s\n", strerror(errno));
        _exit(1);
    }

    // ── Parent ────────────────────────────────────────────────────
    tp->pid          = pid;
    tp->state        = PROC_STATE_RUNNING;
    tp->last_kick    = framework_get_watchdog_kick(tp->slot);
    tp->missed_kicks = 0;

    printf("[supervisor] spawned '%s'  pid=%-6d  slot=%d  "
           "cpu=%d  prio=%d  period=%uus\n",
           tp->manifest.name, pid, tp->slot,
           tp->manifest.cpu_affinity,
           tp->manifest.sched_priority,
           tp->manifest.period_us);

    return 0;
}

// ── Stop a single task process ────────────────────────────────────

static void stop_task(task_process_t *tp, bool wait_for_exit) {
    if (tp->pid <= 0 || tp->state == PROC_STATE_STOPPED
                     || tp->state == PROC_STATE_IDLE) return;

    printf("[supervisor] stopping '%s' (pid=%d)\n",
           tp->manifest.name, tp->pid);

    tp->state = PROC_STATE_STOPPING;

    // Send SIGTERM — task_runner handles this gracefully
    if (kill(tp->pid, SIGTERM) != 0 && errno != ESRCH) {
        fprintf(stderr, "[supervisor] kill(%d, SIGTERM) failed: %s\n",
                tp->pid, strerror(errno));
    }

    if (wait_for_exit) {
        // Blocking wait with timeout — if task doesn't stop in 2s, SIGKILL
        uint64_t deadline = now_ms() + 2000;
        while (now_ms() < deadline) {
            int status;
            pid_t result = waitpid(tp->pid, &status, WNOHANG);
            if (result == tp->pid) goto done;
            sleep_ms(10);
        }
        fprintf(stderr, "[supervisor] '%s' did not stop — sending SIGKILL\n",
                tp->manifest.name);
        kill(tp->pid, SIGKILL);
        waitpid(tp->pid, NULL, 0);
    }

done:
    tp->state = PROC_STATE_STOPPED;
    tp->pid   = 0;
}

// ── Handle a task crash / unexpected exit ─────────────────────────

static void handle_task_exit(task_process_t *tp, int status) {
    clock_gettime(CLOCK_MONOTONIC, &tp->last_crash_time);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("[supervisor] '%s' exited cleanly\n", tp->manifest.name);
        tp->state = PROC_STATE_STOPPED;
        tp->pid   = 0;
        return;
    }

    if (WIFSIGNALED(status)) {
        fprintf(stderr, "[supervisor] '%s' (pid=%d) killed by signal %d (%s)\n",
                tp->manifest.name, tp->pid,
                WTERMSIG(status), strsignal(WTERMSIG(status)));
    } else {
        fprintf(stderr, "[supervisor] '%s' (pid=%d) exited with code %d\n",
                tp->manifest.name, tp->pid, WEXITSTATUS(status));
    }

    tp->state = PROC_STATE_CRASHED;
    tp->pid   = 0;

    // ── Restart policy ────────────────────────────────────────────
    switch (tp->restart_policy) {
        case RESTART_NEVER:
            printf("[supervisor] '%s' will not be restarted (policy=NEVER)\n",
                   tp->manifest.name);
            break;

        case RESTART_LIMITED:
            if (tp->restart_count >= tp->max_restarts) {
                fprintf(stderr, "[supervisor] '%s' exceeded max restarts (%u) — "
                        "not restarting\n",
                        tp->manifest.name, tp->max_restarts);
                break;
            }
            // fall through to restart

        case RESTART_ALWAYS: {
            // Back-off: if it crashed less than 500ms after starting,
            // wait 1 second before restarting to avoid rapid restart loops
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long uptime_ms = (now.tv_sec  - tp->last_start_time.tv_sec)  * 1000
                           + (now.tv_nsec - tp->last_start_time.tv_nsec) / 1000000;

            if (uptime_ms < 500) {
                fprintf(stderr, "[supervisor] '%s' crashed quickly (%ldms) — "
                        "back-off 1s before restart\n",
                        tp->manifest.name, uptime_ms);
                sleep_ms(1000);
            }

            tp->restart_count++;
            printf("[supervisor] restarting '%s' (attempt %u)\n",
                   tp->manifest.name, tp->restart_count);
            spawn_task(tp);
            break;
        }
    }
}

// ── SIGCHLD handler — minimal async-signal-safe work ─────────────
// Only enqueue; supervisor loop processes on next iteration.

static void sigchld_handler(int sig) {
    (void)sig;
    pid_t  pid;
    int    status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int tail = atomic_fetch_add(&g_sv.sig_state.child_exit_tail, 1)
                   % MAX_TASKS_PER_PROJECT;
        atomic_store(&g_sv.sig_state.child_exits[tail].pid,    (int)pid);
        atomic_store(&g_sv.sig_state.child_exits[tail].status, status);
    }
}

static void sigterm_handler(int sig) {
    (void)sig;
    atomic_store(&g_sv.shutdown_requested, true);
}

// ── Watchdog check — called from supervisor loop ──────────────────

static void watchdog_check_all(void) {
    for (size_t i = 0; i < g_sv.task_count; i++) {
        task_process_t *tp = &g_sv.tasks[i];

        if (tp->state != PROC_STATE_RUNNING) continue;
        if (tp->manifest.watchdog_timeout_ms == 0) continue;

        uint64_t kick = framework_get_watchdog_kick((int)i);

        if (kick != tp->last_kick) {
            // Task is alive — reset counter
            tp->last_kick    = kick;
            tp->missed_kicks = 0;
            return;
        }

        // No new kick — accumulate missed time
        tp->missed_kicks += g_sv.config.watchdog_interval_ms;

        if (tp->missed_kicks >= tp->manifest.watchdog_timeout_ms) {
            fprintf(stderr, "[supervisor] WATCHDOG: '%s' (pid=%d) "
                    "silent for %ums — killing\n",
                    tp->manifest.name, tp->pid,
                    tp->manifest.watchdog_timeout_ms);

            //debug_session_close_all_for_task(tp->manifest.id);
            kill(tp->pid, SIGKILL);
            // handle_task_exit will be triggered by SIGCHLD
        }
    }
}

// ── Process pending child exits ───────────────────────────────────

static void process_child_exits(void) {
    for (;;) {
        int head = atomic_load(&g_sv.sig_state.child_exit_head);
        int tail = atomic_load(&g_sv.sig_state.child_exit_tail);
        if (head == tail) break;

        int   slot   = head % MAX_TASKS_PER_PROJECT;
        pid_t pid    = (pid_t)atomic_load(&g_sv.sig_state.child_exits[slot].pid);
        int   status = atomic_load(&g_sv.sig_state.child_exits[slot].status);

        atomic_fetch_add(&g_sv.sig_state.child_exit_head, 1);

        if (pid <= 0) continue;

        pthread_mutex_lock(&g_sv.lock);
        task_process_t *tp = find_task_by_pid(pid);
        if (tp) {
            handle_task_exit(tp, status);
        } else {
            // Unknown child — log and ignore
            fprintf(stderr, "[supervisor] unknown child pid=%d exited "
                    "(status=%d)\n", pid, status);
        }
        pthread_mutex_unlock(&g_sv.lock);
    }
}

// ── Hot-swap processing ───────────────────────────────────────────

static void process_swap_requests(void) {
    for (size_t i = 0; i < g_sv.task_count; i++) {
        task_process_t *tp = &g_sv.tasks[i];
        if (tp->state != PROC_STATE_SWAP_PENDING) continue;

        strncpy(tp->manifest.so_path, tp->pending_so_path,
                MAX_PATH_LEN - 1);
        framework_write_spawn_record((int)i, &tp->manifest);

        if (tp->pid > 0) {
            if (!tp->manifest.allow_hot_swap) {
                // ── Safe default: reject swap on running task ──────
                fprintf(stderr,
                    "[supervisor] swap rejected: '%s' is running. "
                    "Stop the task first.\n",
                    tp->manifest.name);
                tp->state = PROC_STATE_RUNNING;
                tp->pending_so_path[0] = '\0';
                continue;
            }

            // ── Hot swap explicitly enabled for this task ──────────
            // Only appropriate for non-critical tasks like debug,
            // logging, or soft-RT monitoring tasks
            printf("[supervisor] hot swap: '%s' pid=%d → %s\n",
                   tp->manifest.name, tp->pid, tp->pending_so_path);
            kill(tp->pid, SIGUSR1);

        } else {
            // ── Initial load or post-stop load: always allowed ─────
            printf("[supervisor] loading: '%s' → %s\n",
                   tp->manifest.name, tp->pending_so_path);
            spawn_task(tp);
        }

        tp->pending_so_path[0] = '\0';
        tp->swap_on_next_stop  = false;
        tp->state = (tp->pid > 0) ? PROC_STATE_RUNNING
                                   : PROC_STATE_SPAWNING;
    }
}

// ── Load project request from TCP thread ─────────────────────────

static void process_load_request(void) {
    if (!atomic_load(&g_sv.load_requested)) return;

    char path[MAX_PATH_LEN];
    strncpy(path, g_sv.load_requested_path, MAX_PATH_LEN - 1);
    atomic_store(&g_sv.load_requested, false);

    printf("[supervisor] load project request: %s\n", path);

    // Stop all running tasks first
    supervisor_shutdown_all();

    // Load new project
    if (supervisor_load_project(path) == 0) {
        supervisor_start_all();
    }
}

// ─────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────

int supervisor_init(const supervisor_config_t *cfg) {
    memset(&g_sv, 0, sizeof(g_sv));
    g_sv.config = *cfg;
    g_sv.state  = SUPERVISOR_STOPPED;

    pthread_mutex_init(&g_sv.lock, NULL);
    atomic_init(&g_sv.shutdown_requested, false);
    atomic_init(&g_sv.load_requested,     false);
    atomic_init(&g_sv.sig_state.child_exit_head, 0);
    atomic_init(&g_sv.sig_state.child_exit_tail, 0);

    // Register signal handlers
    struct sigaction sa_chld = {
        .sa_handler = sigchld_handler,
        .sa_flags   = SA_RESTART | SA_NOCLDSTOP,
    };
    sigemptyset(&sa_chld.sa_mask);
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term = { .sa_handler = sigterm_handler };
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    return 0;
}

int supervisor_load_project(const char *path) {
    project_manifest_t manifest;

    if (project_load(path, &manifest) != 0) {
        fprintf(stderr, "[supervisor] failed to load project '%s'\n", path);
        return -1;
    }

    project_print(&manifest);

    pthread_mutex_lock(&g_sv.lock);

    g_sv.project    = manifest;
    g_sv.task_count = manifest.task_count;

    // Initialise task process records from manifest
    for (size_t i = 0; i < manifest.task_count; i++) {
        task_process_t *tp = &g_sv.tasks[i];
        memset(tp, 0, sizeof(*tp));

        tp->slot             = (int)i;
        tp->manifest         = manifest.tasks[i];
        tp->state            = PROC_STATE_IDLE;
        tp->pid              = 0;
        tp->restart_policy   = (restart_policy_t)manifest.tasks[i].restart_policy;
        tp->max_restarts     = manifest.tasks[i].max_restarts;
        tp->watchdog_timeout_ms = manifest.tasks[i].watchdog_timeout_ms;

        // Write spawn record into shared memory so task_runner can read it
        framework_write_spawn_record((int)i, &manifest.tasks[i]);
    }

    pthread_mutex_unlock(&g_sv.lock);

    printf("[supervisor] project '%s' loaded (%zu tasks)\n",
           manifest.name, manifest.task_count);

    return 0;
}

int supervisor_start_all(void) {
    if (g_sv.task_count == 0) {
        fprintf(stderr, "[supervisor] no project loaded\n");
        return -1;
    }

    g_sv.state = SUPERVISOR_STARTING;
    printf("[supervisor] starting all tasks for project '%s'\n",
           g_sv.project.name);

    pthread_mutex_lock(&g_sv.lock);

    // ── Pass 1: EtherCAT first ────────────────────────────────────
    for (size_t i = 0; i < g_sv.task_count; i++) {
        task_process_t *tp = &g_sv.tasks[i];
        if (!tp->manifest.enabled)     continue;
        if (!tp->manifest.is_ethercat) continue;

        if (spawn_task(tp) != 0) {
            fprintf(stderr, "[supervisor] fatal: EtherCAT task failed to spawn\n");
            pthread_mutex_unlock(&g_sv.lock);
            g_sv.state = SUPERVISOR_STOPPED;
            return -1;
        }
    }

    pthread_mutex_unlock(&g_sv.lock);

    // Wait for EtherCAT to initialise and begin cycling before
    // any application task tries to take its first snapshot.
    if (g_sv.config.ec_settle_ms > 0) {
        printf("[supervisor] waiting %ums for EtherCAT bus to settle\n",
               g_sv.config.ec_settle_ms);
        sleep_ms(g_sv.config.ec_settle_ms);
    }

    pthread_mutex_lock(&g_sv.lock);

    // ── Pass 2: Application tasks — highest priority first ────────
    // Sort a local index array by sched_priority descending so higher
    // priority tasks are alive before lower priority ones try to run.
    int order[MAX_TASKS_PER_PROJECT];
    int order_count = 0;

    for (size_t i = 0; i < g_sv.task_count; i++) {
        task_process_t *tp = &g_sv.tasks[i];
        if (!tp->manifest.enabled)    continue;
        if (tp->manifest.is_ethercat) continue;
        order[order_count++] = (int)i;
    }

    // Insertion sort — small array, clarity over speed
    for (int a = 1; a < order_count; a++) {
        int key = order[a];
        int b   = a - 1;
        while (b >= 0 && g_sv.tasks[order[b]].manifest.sched_priority < g_sv.tasks[key].manifest.sched_priority) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = key;
    }

    for (int a = 0; a < order_count; a++) {
        task_process_t *tp = &g_sv.tasks[order[a]];

        if (spawn_task(tp) != 0) {
            fprintf(stderr, "[supervisor] failed to spawn '%s' — continuing\n",
                    tp->manifest.name);
        }

        if (g_sv.config.spawn_stagger_ms > 0) {
            pthread_mutex_unlock(&g_sv.lock);
            sleep_ms(g_sv.config.spawn_stagger_ms);
            pthread_mutex_lock(&g_sv.lock);
        }
    }

    pthread_mutex_unlock(&g_sv.lock);

    g_sv.state = SUPERVISOR_RUNNING;
    printf("[supervisor] all tasks started\n");

    return 0;
}

// ── Main supervisor loop — blocks until shutdown ──────────────────

void supervisor_run(void) {
    printf("[supervisor] entering supervisor loop\n");

    while (!atomic_load(&g_sv.shutdown_requested)) {
        sleep_ms(g_sv.config.watchdog_interval_ms);

        // Process exits queued by SIGCHLD handler
        process_child_exits();

        // Watchdog
        watchdog_check_all();

        // Hot-swap requests from TCP receiver
        process_swap_requests();

        // Project load request from TCP receiver
        process_load_request();
    }

    printf("[supervisor] shutdown requested — exiting loop\n");
}

void supervisor_shutdown_all(void) {
    if (g_sv.state == SUPERVISOR_STOPPED) return;

    g_sv.state = SUPERVISOR_STOPPING;
    printf("[supervisor] shutting down all tasks\n");

    pthread_mutex_lock(&g_sv.lock);

    // Stop in reverse priority order — lowest priority first
    int order[MAX_TASKS_PER_PROJECT];
    int order_count = 0;

    for (size_t i = 0; i < g_sv.task_count; i++) {
        if (g_sv.tasks[i].state == PROC_STATE_RUNNING ||
            g_sv.tasks[i].state == PROC_STATE_SPAWNING) {
            order[order_count++] = (int)i;
        }
    }

    // Sort ascending by priority — stop lowest priority first
    for (int a = 1; a < order_count; a++) {
        int key = order[a];
        int b   = a - 1;
        while (b >= 0 &&
               g_sv.tasks[order[b]].manifest.sched_priority >
               g_sv.tasks[key].manifest.sched_priority) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = key;
    }

    for (int a = 0; a < order_count; a++) {
        task_process_t *tp = &g_sv.tasks[order[a]];
        if (tp->manifest.is_ethercat) continue;  // EtherCAT stopped last
        stop_task(tp, true);
    }

    // EtherCAT last — keeps bus valid until all application tasks are down
    for (size_t i = 0; i < g_sv.task_count; i++) {
        if (g_sv.tasks[i].manifest.is_ethercat) {
            stop_task(&g_sv.tasks[i], true);
        }
    }

    pthread_mutex_unlock(&g_sv.lock);

    g_sv.state = SUPERVISOR_STOPPED;
    printf("[supervisor] all tasks stopped\n");
}

// ── Individual task control ───────────────────────────────────────

int supervisor_start_task(int slot) {
    task_process_t *tp = find_task_by_slot(slot);
    if (!tp) return -1;

    pthread_mutex_lock(&g_sv.lock);
    int rc = spawn_task(tp);
    pthread_mutex_unlock(&g_sv.lock);
    return rc;
}

int supervisor_stop_task(int slot) {
    task_process_t *tp = find_task_by_slot(slot);
    if (!tp) return -1;

    pthread_mutex_lock(&g_sv.lock);
    stop_task(tp, false);   // non-blocking — SIGCHLD will confirm exit
    pthread_mutex_unlock(&g_sv.lock);
    return 0;
}

int supervisor_restart_task(int slot) {
    task_process_t *tp = find_task_by_slot(slot);
    if (!tp) return -1;

    pthread_mutex_lock(&g_sv.lock);
    stop_task(tp, true);
    int rc = spawn_task(tp);
    pthread_mutex_unlock(&g_sv.lock);
    return rc;
}

// ── Async requests from TCP receiver thread ───────────────────────

void supervisor_request_swap(int slot, const char *new_so_path) {
    task_process_t *tp = find_task_by_slot(slot);
    if (!tp) {
        fprintf(stderr, "[supervisor] swap requested for invalid slot %d\n",
                slot);
        return;
    }

    pthread_mutex_lock(&g_sv.lock);
    strncpy(tp->pending_so_path, new_so_path, MAX_PATH_LEN - 1);
    tp->swap_on_next_stop = true;
    tp->state             = PROC_STATE_SWAP_PENDING;
    pthread_mutex_unlock(&g_sv.lock);

    printf("[supervisor] swap queued: slot=%d → %s\n", slot, new_so_path);
}

void supervisor_request_load(const char *project_path) {
    strncpy(g_sv.load_requested_path, project_path, MAX_PATH_LEN - 1);
    atomic_store(&g_sv.load_requested, true);
}

void supervisor_request_shutdown(void) {
    atomic_store(&g_sv.shutdown_requested, true);
}

void supervisor_notify_child_exit(pid_t pid, int status) {
    (void)pid; (void)status;
    // Enqueuing is handled directly in sigchld_handler via atomics.
    // This function exists for external callers that detect exits by
    // other means (e.g. a polling thread).
}

// ── Query API ─────────────────────────────────────────────────────

bool supervisor_task_is_running(uint8_t id) {
    task_process_t *tp = find_task_by_id(id);
    return tp && tp->state == PROC_STATE_RUNNING;
}

uint32_t supervisor_task_period(uint8_t id) {
    task_process_t *tp = find_task_by_id(id);
    return tp ? tp->manifest.period_us : 0;
}

int supervisor_slot_for_id(uint8_t id) {
    task_process_t *tp = find_task_by_id(id);
    return tp ? tp->slot : -1;
}

void supervisor_get_status(supervisor_status_t *out) {
    memset(out, 0, sizeof(*out));

    strncpy(out->project_name, g_sv.project.name, MAX_NAME_LEN - 1);
    out->supervisor_state = (uint8_t)g_sv.state;
    out->task_count       = g_sv.task_count;

    for (size_t i = 0; i < g_sv.task_count; i++) {
        task_process_t *tp  = &g_sv.tasks[i];
        task_status_t  *ts  = &out->tasks[i];
        //framework_task_stats_t stats;

        strncpy(ts->name, tp->manifest.name, MAX_NAME_LEN - 1);
        ts->id            = tp->manifest.id;
        ts->proc_state    = (uint8_t)tp->state;
        ts->pid           = tp->pid;
        ts->restart_count = tp->restart_count;

        // Pull live stats from shared memory
        //if (framework_get_task_stats(tp->slot, &stats) == 0) {
        //    ts->cycle_count   = stats.cycle_count;
        //    ts->last_exec_us  = stats.last_exec_us;
        //    ts->max_exec_us   = stats.max_exec_us;
        //    ts->overruns      = stats.overruns;
        //    ts->watchdog_kicks = stats.watchdog_kicks;
        //    ts->debug_drops   = stats.debug_drops;
        //}
    }
}

void supervisor_cleanup(void) {
    pthread_mutex_destroy(&g_sv.lock);
}