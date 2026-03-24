#include "task_host_core/interface.h"

#include <arpa/inet.h>

typedef struct {
    int sock;
    OUTPUT float rpm;
} fg_recv_internal;

void fg_recv_func(fg_recv_internal* ctx);

typedef struct {
    int sock;
    struct sockaddr_in dest;
    INPUT float throttle;
} fg_send_internal;

void fg_send_func(fg_send_internal* ctx);