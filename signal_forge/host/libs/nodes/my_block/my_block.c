#include "my_block.h"

#include <stdio.h>

void my_block1_entry(MyBlock1* ctx) {
    float error = 1000.0 - ctx->input1_1;
    ctx->memory += 0.000000001 * error;
    ctx->output1_1 =  0.0000001 * error + ctx->memory;
    printf("my_block1: error=%.1f mem=%.6f out=%.6f\n", error, ctx->memory, ctx->output1_1);
}


void my_block2_entry(MyBlock2* ctx) {
    float error = 1000.0 - ctx->input2_1;
    ctx->memory += 0.000001 * error;
    ctx->output2_1 =  0.002 * error + ctx->memory;
    printf("my_block2: error=%.1f mem=%.6f out=%.6f\n", error, ctx->memory, ctx->output2_1);
}

void PID_entry(PID* ctx) {
    /* Read inputs */
    REAL sp = ctx->setpoint;
    REAL pv = ctx->measurement;
    REAL dt = ctx->dt;

    /* Safety on dt */
    if (dt <= 0.0f) {
        dt = 1e-6f;
    }

    /* Error */
    REAL error = sp - pv;

    /* Derivative (raw) */
    REAL raw_d = (error - ctx->prev_error) / dt;

    /* First-order filter for derivative: alpha = tau / (tau + dt)
       If tau <= 0, use raw derivative (no filtering) */
    REAL tau = ctx->tau;
    if (tau <= 0.0f) {
        ctx->deriv_state = raw_d;
    } else {
        REAL alpha = tau / (tau + dt);
        ctx->deriv_state = alpha * ctx->deriv_state + (1.0f - alpha) * raw_d;
    }

    /* Tentative integrator update (anti-windup by conditional commit) */
    REAL tentative_integral = ctx->integral + ctx->Ki * error * dt;

    /* Compute unclamped output using tentative integral and filtered derivative */
    REAL unclamped = ctx->Kp * error + tentative_integral + ctx->Kd * ctx->deriv_state;

    /* Clamp output */
    REAL out = unclamped;
    if (out > ctx->out_max) out = ctx->out_max;
    if (out < ctx->out_min) out = ctx->out_min;

    /* Commit integrator only if unclamped output is not saturated */
    if (out == unclamped) {
        ctx->integral = tentative_integral;
    }

    /* Save previous error */
    ctx->prev_error = error;

    /* Set output */
    ctx->output = out;

    /* Optional debug print (consistent with your examples) */
    printf("pid_block: err=%.6f Kp=%.6f Ki=%.6f Kd=%.6f int=%.6f der=%.6f out=%.6f\n",
           error, ctx->Kp, ctx->Ki, ctx->Kd, ctx->integral, ctx->deriv_state, ctx->output);
}


