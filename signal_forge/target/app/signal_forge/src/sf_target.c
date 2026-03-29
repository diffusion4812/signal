#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/utsname.h>
#include "sf_target.h"

// ── Banner ────────────────────────────────────────────────────────

void print_banner(void) {
    printf("\n");
    printf("  ███████╗██╗ ██████╗ ███╗   ██╗ █████╗ ██╗\n");
    printf("  ██╔════╝██║██╔════╝ ████╗  ██║██╔══██╗██║\n");
    printf("  ███████╗██║██║  ███╗██╔██╗ ██║███████║██║\n");
    printf("  ╚════██║██║██║   ██║██║╚██╗██║██╔══██║██║\n");
    printf("  ███████║██║╚██████╔╝██║ ╚████║██║  ██║███████╗\n");
    printf("  ╚══════╝╚═╝ ╚═════╝ ╚═╝  ╚═══╝╚═╝  ╚═╝╚══════╝\n");
    printf("\n");
    printf("  ███████╗ ██████╗ ██████╗  ██████╗ ███████╗\n");
    printf("  ██╔════╝██╔═══██╗██╔══██╗██╔════╝ ██╔════╝\n");
    printf("  █████╗  ██║   ██║██████╔╝██║  ███╗█████╗\n");
    printf("  ██╔══╝  ██║   ██║██╔══██╗██║   ██║██╔══╝\n");
    printf("  ██║     ╚██████╔╝██║  ██║╚██████╔╝███████╗\n");
    printf("  ╚═╝      ╚═════╝ ╚═╝  ╚═╝ ╚═════╝ ╚══════╝ TARGET\n");
    printf("\n");
    printf("  version %d.%d\n\n",
           SF_TARGET_VERSION_MAJOR,
           SF_TARGET_VERSION_MINOR);
}

// ── Argument parsing ──────────────────────────────────────────────

void print_usage(const char *prog) {
    printf("usage: %s [options]\n\n", prog);
    printf("  -p <port>      listen port for signal_forge_host "
           "(default: %d)\n", SF_TARGET_DEFAULT_PORT);
    printf("  -j <project>   project.json to load on startup\n");
    printf("  -t <task_dir>  directory for received .so files "
           "(default: projects/tasks)\n");
    printf("  -r <runner>    path to task_runner binary "
           "(default: ./task_runner)\n");
    printf("  -a             autostart tasks after project load\n");
    printf("  -h             show this help\n\n");
}

int parse_args(int argc, char *argv[], sf_target_config_t *cfg) {
    int opt;
    while ((opt = getopt(argc, argv, "p:j:t:r:ah")) != -1) {
        switch (opt) {
            case 'p':
                cfg->port = (uint16_t)atoi(optarg);
                if (cfg->port == 0) {
                    fprintf(stderr, "invalid port: %s\n", optarg);
                    return -1;
                }
                break;
            case 'j':
                cfg->project_path = optarg;
                break;
            case 't':
                cfg->task_dir = optarg;
                break;
            case 'r':
                cfg->runner_path = optarg;
                break;
            case 'a':
                cfg->autostart = true;
                break;
            case 'h':
                return -1;
            default:
                fprintf(stderr, "unknown option: -%c\n", opt);
                return -1;
        }
    }
    return 0;
}

// ── RT environment checks ─────────────────────────────────────────

int sf_check_rt_environment(void) {
    int issues = 0;

    // ── Kernel PREEMPT_RT ─────────────────────────────────────────
    struct utsname u;
    uname(&u);
    if (!strstr(u.release, "rt") && !strstr(u.release, "RT")) {
        fprintf(stderr, "[sf_target] WARNING: kernel '%s' does not "
                "appear to be PREEMPT_RT\n", u.release);
        issues++;
    } else {
        printf("[sf_target] kernel: %s  OK\n", u.release);
    }

    // ── Isolated CPUs ─────────────────────────────────────────────
    FILE *f = fopen("/sys/devices/system/cpu/isolated", "r");
    if (f) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), f)) {
            buf[strcspn(buf, "\n")] = '\0';
            if (strlen(buf) == 0) {
                fprintf(stderr, "[sf_target] WARNING: no isolated CPUs "
                        "(isolcpus not set in kernel cmdline)\n");
                issues++;
            } else {
                printf("[sf_target] isolated CPUs: %s  OK\n", buf);
            }
        }
        fclose(f);
    } else {
        fprintf(stderr, "[sf_target] WARNING: cannot read isolated CPU "
                "list\n");
        issues++;
    }

    // ── RT scheduling capability ──────────────────────────────────
    struct sched_param sp = { .sched_priority = 1 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        fprintf(stderr, "[sf_target] WARNING: cannot set SCHED_FIFO — "
                "run as root or grant CAP_SYS_NICE\n");
        issues++;
    } else {
        // Restore normal scheduling immediately
        sp.sched_priority = 0;
        sched_setscheduler(0, SCHED_OTHER, &sp);
        printf("[sf_target] SCHED_FIFO capability: OK\n");
    }

    // ── /dev/shm writable ─────────────────────────────────────────
    if (access("/dev/shm", W_OK) != 0) {
        fprintf(stderr, "[sf_target] WARNING: /dev/shm not writable — "
                "shared memory will fail\n");
        issues++;
    } else {
        printf("[sf_target] /dev/shm writable: OK\n");
    }

    if (issues > 0) {
        fprintf(stderr, "[sf_target] %d environment issue(s) — "
                "real-time performance may be degraded\n\n", issues);
    } else {
        printf("[sf_target] RT environment OK\n\n");
    }

    // Warn but never fatal — allows development on non-RT machines
    return issues > 0 ? -1 : 0;
}