#include "fg.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

void fg_recv_func(fg_recv_internal* ctx) {
    if (!ctx->sock) {
        ctx->sock = socket(AF_INET, SOCK_DGRAM, 0);
        printf("fg_recv: created socket %d\n", ctx->sock);
        if (ctx->sock < 0) {
            ctx->sock = 0;
            return;
        }

        fcntl(ctx->sock, F_SETFL, O_NONBLOCK);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(5501);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (bind(ctx->sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            printf("fg_recv: bind failed: %s\n", strerror(errno));
            close(ctx->sock);
            ctx->sock = 0;
            return;
        }
    }
    else {
        float buf;
        float latest;
        int got_data = 0;

        while (1) {
            ssize_t n = recvfrom(ctx->sock, &buf, sizeof(buf), 0, NULL, NULL);
            if (n == (ssize_t)sizeof(float)) {
                latest = buf;
                got_data = 1;
            }
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                printf("fg_recv: error %d (%s)\n", errno, strerror(errno));
                break;
            }
            else {
                break;
            }
        }

        if (got_data) {
            ctx->rpm = buf;
        }
    }
}

void fg_send_func(fg_send_internal* ctx) {
    if (!ctx->sock) {
        ctx->sock = socket(AF_INET, SOCK_DGRAM, 0);
        printf("fg_send: created socket %d\n", ctx->sock);
        if (ctx->sock < 0) {
            ctx->sock = 0;
            return;
        }

        memset(&ctx->dest, 0, sizeof(ctx->dest));
        ctx->dest.sin_family = AF_INET;
        ctx->dest.sin_port   = htons(5500);
        inet_pton(AF_INET, "127.0.0.1", &ctx->dest.sin_addr);
    }
    else {
        float value = ctx->throttle;
        ssize_t n = sendto(ctx->sock, &value, sizeof(value), 0,
                           (struct sockaddr*)&ctx->dest, sizeof(ctx->dest));
        if (n < 0) {
            printf("fg_send: error %d (%s)\n", errno, strerror(errno));
        }
    }
}