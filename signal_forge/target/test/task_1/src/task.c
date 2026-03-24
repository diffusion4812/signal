#include <stdio.h>
#include <math.h>
#include "task_host_core/interface.h"
#include "task_host_core/manifest.h"

#include "task.h"

typedef struct {
    BlockHeader header;
    double integral;
    int tick_count;
} PID_Internal;

typedef struct {
    BlockHeader header;
    double in;
    double out;
} Floor_Internal;

typedef struct {
    BlockHeader header;
    PID_Internal pid;
    Floor_Internal floor;
} Combined_Internal;

const TaskManifest TASK_METADATA = {
    .num_slots = 3,
    .slots = (SlotInternalDef[]) {
        { ID_MYPIDBLOCK,      0xABC123, sizeof(PID_Internal) },
        { ID_MYFLOORBLOCK,    0xDEF456, sizeof(Floor_Internal) },
        { ID_MYCOMBINEDBLOCK, 0x789012, sizeof(Combined_Internal) }
    }
};

void PID(PID_Internal *pid) {
    pid->integral += 0.5;
    pid->tick_count++;
}

void task_entry(TaskContext *ctx) {
    PID_Internal *pid_internal = (PID_Internal*)ctx->slots[ID_MYPIDBLOCK];
    Floor_Internal *floor_internal = (Floor_Internal*)ctx->slots[ID_MYFLOORBLOCK];

    Combined_Internal *combined_internal = (Combined_Internal*)ctx->slots[ID_MYCOMBINEDBLOCK];

    PID(pid_internal);

    floor_internal->in = pid_internal->integral;
    floor_internal->out = floor(floor_internal->in);

    combined_internal->floor.out = floor_internal->out + 0.2;

    printf("[Task V1] Tick: %d | Integral: %.1f | Floor: %.1f | Combined: %.1f\n", 
           pid_internal->tick_count, pid_internal->integral, floor_internal->out, combined_internal->floor.out);
}