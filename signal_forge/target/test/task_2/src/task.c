#include <stdio.h>
#include <math.h>
#include "task_host_core/interface.h"
#include "task_host_core/manifest.h"

#include "task.h"

typedef struct {
    BlockHeader header;
    double integral;
    int tick_count;
} PID_Internal_V1;

typedef struct {
    BlockHeader header;
    int tick_count;
    double integral;
} PID_Internal_V2;

typedef struct {
    BlockHeader header;
    double in;
    double out;
} Floor_Internal;

typedef struct {
    BlockHeader header;
    PID_Internal_V1 pid;
    Floor_Internal floor;
} Combined_Internal_V1;

typedef struct {
    BlockHeader header;
    PID_Internal_V2 pid;
    Floor_Internal floor;
} Combined_Internal_V2;

const TaskManifest TASK_METADATA = {
    .num_slots = 3,
    .slots = (SlotInternalDef[]) {
        { ID_MYPIDBLOCK,      0xABC124, sizeof(PID_Internal_V2) },
        { ID_MYFLOORBLOCK,    0xDEF456, sizeof(Floor_Internal) },
        { ID_MYCOMBINEDBLOCK, 0x789014, sizeof(Combined_Internal_V2) }
    }
};

int migrate_state(uint32_t old_sig, void* old_data, void* new_data) {
    if (old_sig == 0xABC123) { // The signature of V1
        PID_Internal_V1 *v1 = (PID_Internal_V1*)old_data;
        PID_Internal_V2 *v2 = (PID_Internal_V2*)new_data;
        
        // Copy over the fields that still exist
        v2->integral = v1->integral;
        v2->tick_count = v1->tick_count;
        return 1;
    }

    if (old_sig == 0x789012) {
        Combined_Internal_V1 *old_c = (Combined_Internal_V1*)old_data;
        Combined_Internal_V2 *new_c = (Combined_Internal_V2*)new_data;
        
        // Migrate the nested PID data
        new_c->pid.integral = old_c->pid.integral;
        new_c->pid.tick_count = old_c->pid.tick_count;
        
        // Copy the floor data (which didn't change)
        new_c->floor = old_c->floor;
        return 1;
    }
    
    return 0;
}

void task_entry(TaskContext *ctx) {
    PID_Internal_V2 *pid_internal = (PID_Internal_V2*)ctx->slots[ID_MYPIDBLOCK];
    Floor_Internal *floor_internal = (Floor_Internal*)ctx->slots[ID_MYFLOORBLOCK];

    Combined_Internal_V1 *combined_internal = (Combined_Internal_V1*)ctx->slots[ID_MYCOMBINEDBLOCK];

    pid_internal->tick_count++;
    pid_internal->integral += 1.0;

    floor_internal->in = pid_internal->integral;
    floor_internal->out = floor(floor_internal->in);

    combined_internal->floor.out = floor_internal->out + 0.4;

    printf("[Task V2] Tick: %d | Integral: %.1f | Floor: %.1f | Combined: %.1f\n", 
           pid_internal->tick_count, pid_internal->integral, floor_internal->out, combined_internal->floor.out);
}