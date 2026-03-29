// mailbox.c — telegram send/receive helpers for task_context

#include <string.h>
#include <stdio.h>
#include "framework.h"

// ── CRC-16/CCITT ──────────────────────────────────────────────────
static uint16_t crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)(crc << 1) ^ 0x1021
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

static void telegram_seal(telegram_t *tg) {
    tg->crc = crc16((const uint8_t *)tg,
                    offsetof(telegram_t, crc));
}

static bool telegram_valid(const telegram_t *tg) {
    return tg->crc == crc16((const uint8_t *)tg,
                             offsetof(telegram_t, crc));
}

// ── task_recv: pop next telegram from this task's mailbox ─────────
bool task_recv(task_context_t *ctx, telegram_t *out) {
    return mailbox_ring_pop(&ctx->shared->mailbox, out);
}

// ── task_send: push telegram to dest task's mailbox ───────────────
bool task_send(task_context_t *ctx, task_id_t dest,
               uint16_t tid, const void *payload, uint16_t len) {

    if (len > MAX_PAYLOAD_SIZE) {
        fprintf(stderr, "[framework] task_send: payload too large (%u > %d)\n",
                len, MAX_PAYLOAD_SIZE);
        return false;
    }

    framework_shared_t *shared = framework_get_shared();
    int task_count = atomic_load(&shared->task_count);

    telegram_t tg = {
        .version     = 1,
        .source_id   = ctx->desc->id,
        .dest_id     = dest,
        .telegram_id = tid,
        .sequence    = ctx->_sequence++,
        .timestamp_us = framework_now_us(),
        .payload_len = len,
    };

    if (payload && len > 0)
        memcpy(tg.payload, payload, len);

    telegram_seal(&tg);

    if (dest == TASK_ID_BROADCAST) {
        bool any = false;
        for (int slot = 0; slot < task_count; slot++) {
            if (shared->spawn_records[slot].manifest.id == ctx->desc->id)
                continue;   // don't send to self
            if (!mailbox_ring_push(&shared->task_blocks[slot].mailbox, &tg)) {
                shared->task_blocks[slot].stats.mailbox_drops++;
            } else {
                any = true;
            }
        }
        return any;
    }

    // Unicast — find dest slot
    for (int slot = 0; slot < task_count; slot++) {
        if (shared->spawn_records[slot].manifest.id == dest) {
            if (!mailbox_ring_push(&shared->task_blocks[slot].mailbox, &tg)) {
                shared->task_blocks[slot].stats.mailbox_drops++;
                return false;
            }
            return true;
        }
    }

    fprintf(stderr, "[framework] task_send: dest 0x%02x not found\n", dest);
    return false;
}

// ── task_reply: respond to a received telegram ────────────────────
bool task_reply(task_context_t *ctx, const telegram_t *req,
                uint16_t tid, const void *payload, uint16_t len) {
    return task_send(ctx, req->source_id, tid, payload, len);
}

// ── Gateway push: TCP receiver injects telegram into task mailbox ─
bool framework_mailbox_push(task_id_t dest_id,
                             const telegram_t *tg) {
    framework_shared_t *shared = framework_get_shared();
    int task_count = atomic_load(&shared->task_count);

    if (dest_id == TASK_ID_BROADCAST) {
        bool any = false;
        for (int slot = 0; slot < task_count; slot++) {
            if (!mailbox_ring_push(&shared->task_blocks[slot].mailbox, tg)) {
                shared->task_blocks[slot].stats.mailbox_drops++;
            } else {
                any = true;
            }
        }
        return any;
    }

    for (int slot = 0; slot < task_count; slot++) {
        if (shared->spawn_records[slot].manifest.id == dest_id) {
            if (!mailbox_ring_push(&shared->task_blocks[slot].mailbox, tg)) {
                shared->task_blocks[slot].stats.mailbox_drops++;
                return false;
            }
            return true;
        }
    }

    return false;
}