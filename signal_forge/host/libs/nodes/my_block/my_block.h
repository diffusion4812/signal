#include "task_host_core/interface.h"

typedef struct {
    STATIC REAL memory;
    INPUT  REAL input1_1;
    INPUT  REAL input1_2;
    INPUT  REAL input1_3;
    OUTPUT REAL output1_1;
} MyBlock1;

typedef struct {
    STATIC REAL memory;
    INPUT  REAL input2_1;
    INPUT  REAL input2_2;
    INPUT  REAL input2_3;
    OUTPUT REAL output2_1;
} MyBlock2;

typedef struct {
    /* STATIC state and parameters (configure at init or from host) */
    STATIC REAL integral;        /* integrator state */
    STATIC REAL prev_error;      /* previous error for derivative */
    STATIC REAL deriv_state;     /* filtered derivative state */

    INPUT REAL Kp;              /* proportional gain */
    INPUT REAL Ki;              /* integral gain (per second) */
    INPUT REAL Kd;              /* derivative gain */
    INPUT REAL tau;             /* derivative filter time constant (s) */
    INPUT REAL out_min;         /* output lower limit */
    INPUT REAL out_max;         /* output upper limit */

    /* Inputs */
    INPUT  REAL setpoint;        /* desired value */
    INPUT  REAL measurement;     /* measured value */
    INPUT  REAL dt;              /* time step in seconds */

    /* Output */
    OUTPUT REAL output;       /* controller output */
} PID;

void my_block1_entry(MyBlock1* ctx);
void my_block2_entry(MyBlock2* ctx);
void PID_entry(PID* ctx);