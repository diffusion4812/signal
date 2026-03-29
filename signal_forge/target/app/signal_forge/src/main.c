#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "sf_target.h"
#include "supervisor.h"
#include "signal_forge_target/framework.h"
#include "task_host_core/sf_protocol.h"
#include "sf_receiver.h"

int main(int argc, char *argv[]) {
    print_banner();

    sf_target_config_t config = {
        .port         = SF_TARGET_DEFAULT_PORT,
        .project_path = "projects/project.json",
        .task_dir     = "projects/tasks",
        .runner_path  = "../task_runner/signal_forge_task_runner",
        .autostart    = true,
    };

    if (parse_args(argc, argv, &config) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    sf_check_rt_environment();

    if (framework_shm_init() != 0) {
        fprintf(stderr, "[sf_target] fatal: shared memory init failed\n");
        return 1;
    }

    supervisor_config_t sv_cfg = {
        .runner_path          = config.runner_path,
        .task_dir             = config.task_dir,
        .watchdog_interval_ms = 10,
        .watchdog_timeout_ms  = 200,
        .spawn_stagger_ms     = 5,
        .ec_settle_ms         = 50,
    };
    supervisor_init(&sv_cfg);

    sf_receiver_config_t rx_cfg = {
        .port             = config.port,
        .task_dir         = config.task_dir,
        .on_swap          = supervisor_request_swap,
        .on_load_project  = supervisor_request_load,
        .on_shutdown      = supervisor_request_shutdown,
    };
    if (sf_receiver_start(&rx_cfg) != 0) {
        fprintf(stderr, "[sf_target] fatal: TCP receiver failed\n");
        framework_shm_destroy();
        return 1;
    }

    printf("[sf_target] listening on port %d\n", config.port);

    if (config.project_path) {
        if (supervisor_load_project(config.project_path) == 0
                && config.autostart) {
            supervisor_start_all();
        }
    } else {
        printf("[sf_target] waiting for project from signal_forge_host\n");
    }

    supervisor_run();

    printf("[sf_target] shutting down\n");
    supervisor_shutdown_all();
    sf_receiver_stop();
    framework_shm_destroy();
    supervisor_cleanup();

    printf("[sf_target] goodbye\n");
    return 0;
}