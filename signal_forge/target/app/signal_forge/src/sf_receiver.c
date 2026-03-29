#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "sf_receiver.h"
#include "task_host_core/sf_protocol.h"
#include "supervisor.h"
#include "signal_forge_target/framework.h"

// ── Internal state ────────────────────────────────────────────────

// Transfer state machine
typedef enum {
    TRANSFER_IDLE,
    TRANSFER_ACTIVE,
} transfer_state_t;

typedef struct {
    transfer_state_t  state;
    char              filename[128];
    char              dest_path[512];
    int               fd;               // open file descriptor during transfer
    uint32_t          total_size;
    uint32_t          expected_crc;
    uint32_t          bytes_received;
    uint8_t           target_slot;
} transfer_ctx_t;

// OD poll state — one pending request at a time per connection
typedef struct {
    bool              pending;
    uint32_t          request_id;
    uint8_t           task_id;
    int               task_slot;
} od_poll_ctx_t;

// Receiver state
typedef struct {
    sf_receiver_config_t  config;

    // Networking
    int                   listen_fd;
    int                   conn_fd;          // -1 if no client connected
    pthread_mutex_t       send_lock;        // serialise concurrent sends

    // Threads
    pthread_t             accept_thread;
    pthread_t             session_thread;
    atomic_bool           running;
    atomic_bool           session_active;

    // Sub-state machines
    transfer_ctx_t        transfer;
    od_poll_ctx_t         od_poll;

    // Sequence counter for outbound packets
    uint32_t              send_sequence;

} sf_receiver_t;

static sf_receiver_t g_rx = {0};

// ─────────────────────────────────────────────────────────────────
// CRC utilities
// ─────────────────────────────────────────────────────────────────

static uint16_t crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u)
                ? (uint16_t)((crc << 1) ^ SF_HEADER_CRC_POLY)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    // CRC-32/ISO-HDLC
    static const uint32_t table[256] = {
        0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA,
        0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
        0x0EDB8832, 0x79DCB8A4, 0xE0D5E91B, 0x97D2D988,
        0x09B64C2B, 0x7EB17CBF, 0xE7B82D09, 0x90BF1FBD,
        // ... initialise full table at startup rather than embedding
        // Using this simplified runtime version instead:
    };
    (void)table;

    // Runtime CRC-32 — avoid embedding 1KB table
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return ~crc;
}

// ─────────────────────────────────────────────────────────────────
// Network helpers
// ─────────────────────────────────────────────────────────────────

// Blocking read — reads exactly 'len' bytes or returns -1
static int recv_exact(int fd, void *buf, size_t len) {
    size_t   remaining = len;
    uint8_t *ptr       = buf;

    while (remaining > 0) {
        ssize_t n = recv(fd, ptr, remaining, 0);
        if (n <= 0) {
            if (n == 0)
                fprintf(stderr, "[sf_receiver] connection closed by host\n");
            else
                fprintf(stderr, "[sf_receiver] recv error: %s\n",
                        strerror(errno));
            return -1;
        }
        ptr       += n;
        remaining -= (size_t)n;
    }
    return 0;
}

// Blocking write — writes exactly 'len' bytes or returns -1
static int send_exact(int fd, const void *buf, size_t len) {
    size_t         remaining = len;
    const uint8_t *ptr       = buf;

    while (remaining > 0) {
        ssize_t n = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (n <= 0) {
            fprintf(stderr, "[sf_receiver] send error: %s\n",
                    strerror(errno));
            return -1;
        }
        ptr       += n;
        remaining -= (size_t)n;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────
// Packet send (thread-safe)
// ─────────────────────────────────────────────────────────────────

bool sf_send_packet(uint16_t command,
                    const void *payload, uint32_t payload_len) {
    int fd = g_rx.conn_fd;
    if (fd < 0) return false;

    sf_header_t hdr = {
        .magic       = SF_MAGIC,
        .version     = SF_PROTOCOL_VERSION,
        .flags       = 0,
        .command     = command,
        .payload_len = payload_len,
    };

    pthread_mutex_lock(&g_rx.send_lock);
    hdr.sequence  = g_rx.send_sequence++;
    hdr.header_crc = 0;
    hdr.header_crc = crc16((const uint8_t *)&hdr, sizeof(hdr));

    int rc = 0;
    rc |= send_exact(fd, &hdr, sizeof(hdr));
    if (payload && payload_len > 0)
        rc |= send_exact(fd, payload, payload_len);

    pthread_mutex_unlock(&g_rx.send_lock);
    return rc == 0;
}

static bool send_ack(uint16_t command) {
    return sf_send_packet(command, NULL, 0);
}

static bool send_nack(uint16_t command, uint8_t reason,
                      const char *message) {
    sf_nack_t nack = { .reason = reason };
    snprintf(nack.message, sizeof(nack.message), "%s",
             message ? message : "");
    return sf_send_packet(command, &nack, sizeof(nack));
}

// ─────────────────────────────────────────────────────────────────
// Header validation
// ─────────────────────────────────────────────────────────────────

static bool header_valid(const sf_header_t *hdr) {
    if (hdr->magic != SF_MAGIC) {
        fprintf(stderr, "[sf_receiver] bad magic: 0x%04X\n", hdr->magic);
        return false;
    }
    if (hdr->version != SF_PROTOCOL_VERSION) {
        fprintf(stderr, "[sf_receiver] version mismatch: got %u want %u\n",
                hdr->version, SF_PROTOCOL_VERSION);
        return false;
    }

    // Validate header CRC — save and zero the field before checking
    uint16_t received_crc  = hdr->header_crc;
    sf_header_t hdr_copy   = *hdr;
    hdr_copy.header_crc    = 0;
    uint16_t computed_crc  = crc16((const uint8_t *)&hdr_copy,
                                    sizeof(hdr_copy));

    if (received_crc != computed_crc) {
        fprintf(stderr, "[sf_receiver] header CRC mismatch: "
                "got 0x%04X want 0x%04X\n",
                received_crc, computed_crc);
        return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────
// Command handlers
// ─────────────────────────────────────────────────────────────────

// ── SF_CMD_LOAD_PROJECT ───────────────────────────────────────────
// Payload is raw JSON text. Write to a temp file then ask supervisor
// to load it.

static void handle_load_project(const uint8_t *payload, uint32_t len) {
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/_incoming_project.json",
             g_rx.config.task_dir);

    // Ensure task_dir exists
    mkdir(g_rx.config.task_dir, 0755);

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        fprintf(stderr, "[sf_receiver] cannot write project file: %s\n",
                strerror(errno));
        send_nack(SF_CMD_PROJECT_NACK, SF_NACK_IO_ERROR,
                  "cannot write project file");
        return;
    }

    fwrite(payload, 1, len, f);
    fclose(f);

    printf("[sf_receiver] project received (%u bytes) → %s\n",
           len, tmp_path);

    // Validate before handing off
    project_manifest_t manifest;
    if (project_load(tmp_path, &manifest) != 0) {
        send_nack(SF_CMD_PROJECT_NACK, SF_NACK_IO_ERROR,
                  "project JSON failed validation");
        unlink(tmp_path);
        return;
    }

    send_ack(SF_CMD_PROJECT_ACK);

    // Ask supervisor to load — happens on next supervisor loop iteration
    if (g_rx.config.on_load_project)
        g_rx.config.on_load_project(tmp_path);
}

// ── SF_CMD_TRANSFER_BEGIN ─────────────────────────────────────────

static void handle_transfer_begin(const uint8_t *payload, uint32_t len) {
    if (len < sizeof(sf_transfer_begin_t)) {
        send_nack(SF_CMD_TRANSFER_NACK, SF_NACK_IO_ERROR,
                  "payload too small");
        return;
    }

    sf_transfer_begin_t req;
    memcpy(&req, payload, sizeof(req));

    if (g_rx.transfer.state == TRANSFER_ACTIVE) {
        fprintf(stderr, "[sf_receiver] transfer already active — "
                "closing previous\n");
        if (g_rx.transfer.fd >= 0) {
            close(g_rx.transfer.fd);
            g_rx.transfer.fd = -1;
        }
    }

    // Sanitise filename — strip any path components
    const char *basename = strrchr(req.filename, '/');
    basename = basename ? basename + 1 : req.filename;

    if (strlen(basename) == 0 || strlen(basename) > 127) {
        send_nack(SF_CMD_TRANSFER_NACK, SF_NACK_IO_ERROR,
                  "invalid filename");
        return;
    }

    // Ensure destination directory exists
    mkdir(g_rx.config.task_dir, 0755);

    snprintf(g_rx.transfer.dest_path, sizeof(g_rx.transfer.dest_path),
             "%s/%s", g_rx.config.task_dir, basename);

    g_rx.transfer.fd = open(g_rx.transfer.dest_path,
                             O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                             S_IRWXU | S_IRGRP | S_IXGRP);

    if (g_rx.transfer.fd < 0) {
        fprintf(stderr, "[sf_receiver] cannot create '%s': %s\n",
                g_rx.transfer.dest_path, strerror(errno));
        send_nack(SF_CMD_TRANSFER_NACK, SF_NACK_IO_ERROR,
                  "cannot create destination file");
        return;
    }

    strncpy(g_rx.transfer.filename, basename,
            sizeof(g_rx.transfer.filename) - 1);
    g_rx.transfer.total_size     = req.total_size;
    g_rx.transfer.expected_crc   = req.crc32;
    g_rx.transfer.bytes_received = 0;
    g_rx.transfer.target_slot    = req.target_slot;
    g_rx.transfer.state          = TRANSFER_ACTIVE;

    printf("[sf_receiver] transfer begin: '%s'  size=%u  slot=%u\n",
           g_rx.transfer.filename,
           g_rx.transfer.total_size,
           g_rx.transfer.target_slot);

    send_ack(SF_CMD_TRANSFER_ACK);
}

// ── SF_CMD_TRANSFER_CHUNK ─────────────────────────────────────────

static void handle_transfer_chunk(const uint8_t *payload, uint32_t len) {
    if (g_rx.transfer.state != TRANSFER_ACTIVE) {
        send_nack(SF_CMD_TRANSFER_NACK, SF_NACK_NOT_IN_TRANSFER,
                  "no active transfer");
        return;
    }

    if (len < offsetof(sf_transfer_chunk_t, data)) {
        send_nack(SF_CMD_TRANSFER_NACK, SF_NACK_IO_ERROR,
                  "chunk payload too small");
        return;
    }

    sf_transfer_chunk_t chunk;
    // Chunk header fields only — data follows directly in payload
    memcpy(&chunk, payload, offsetof(sf_transfer_chunk_t, data));

    uint16_t    data_len  = chunk.length;
    const uint8_t *data   = payload + offsetof(sf_transfer_chunk_t, data);

    if (len < offsetof(sf_transfer_chunk_t, data) + data_len) {
        send_nack(SF_CMD_TRANSFER_NACK, SF_NACK_IO_ERROR,
                  "chunk data truncated");
        return;
    }

    // Write to file at the declared offset
    if (lseek(g_rx.transfer.fd, (off_t)chunk.offset, SEEK_SET) < 0) {
        send_nack(SF_CMD_TRANSFER_NACK, SF_NACK_IO_ERROR, "seek failed");
        return;
    }

    ssize_t written = write(g_rx.transfer.fd, data, data_len);
    if (written != (ssize_t)data_len) {
        fprintf(stderr, "[sf_receiver] write failed: %s\n",
                strerror(errno));
        send_nack(SF_CMD_TRANSFER_NACK, SF_NACK_IO_ERROR,
                  "write failed");
        return;
    }

    g_rx.transfer.bytes_received += data_len;
    send_ack(SF_CMD_TRANSFER_ACK);
}

// ── SF_CMD_TRANSFER_END ───────────────────────────────────────────

static void handle_transfer_end(void) {
    if (g_rx.transfer.state != TRANSFER_ACTIVE) {
        send_nack(SF_CMD_TRANSFER_NACK, SF_NACK_NOT_IN_TRANSFER,
                  "no active transfer");
        return;
    }

    // Flush and close file
    fsync(g_rx.transfer.fd);
    close(g_rx.transfer.fd);
    g_rx.transfer.fd = -1;

    // Size check
    if (g_rx.transfer.bytes_received != g_rx.transfer.total_size) {
        fprintf(stderr, "[sf_receiver] size mismatch: got %u expected %u\n",
                g_rx.transfer.bytes_received, g_rx.transfer.total_size);
        send_nack(SF_CMD_TRANSFER_NACK, SF_NACK_SIZE_MISMATCH,
                  "file size mismatch");
        unlink(g_rx.transfer.dest_path);
        g_rx.transfer.state = TRANSFER_IDLE;
        return;
    }

    // CRC-32 verify — re-read the file
    FILE *f = fopen(g_rx.transfer.dest_path, "rb");
    if (!f) {
        send_nack(SF_CMD_TRANSFER_NACK, SF_NACK_IO_ERROR,
                  "cannot reopen for CRC check");
        g_rx.transfer.state = TRANSFER_IDLE;
        return;
    }

    uint32_t crc    = 0;
    uint8_t  buf[4096];
    size_t   n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        crc = crc32_update(crc, buf, n);
    fclose(f);

    if (crc != g_rx.transfer.expected_crc) {
        fprintf(stderr, "[sf_receiver] CRC mismatch: got 0x%08X "
                "expected 0x%08X\n", crc, g_rx.transfer.expected_crc);
        send_nack(SF_CMD_TRANSFER_NACK, SF_NACK_BAD_CRC, "CRC mismatch");
        unlink(g_rx.transfer.dest_path);
        g_rx.transfer.state = TRANSFER_IDLE;
        return;
    }

    printf("[sf_receiver] transfer complete: '%s'  %u bytes  "
           "CRC OK  slot=%u\n",
           g_rx.transfer.filename,
           g_rx.transfer.bytes_received,
           g_rx.transfer.target_slot);

    send_ack(SF_CMD_TRANSFER_ACK);

    // Signal supervisor to swap this slot to the new .so
    if (g_rx.config.on_swap) {
        g_rx.config.on_swap((int)g_rx.transfer.target_slot,
                             g_rx.transfer.dest_path);
    }

    g_rx.transfer.state = TRANSFER_IDLE;
}

// ── SF_CMD_SWAP_TASK ──────────────────────────────────────────────

static void handle_swap_task(const uint8_t *payload, uint32_t len) {
    if (len < sizeof(sf_swap_request_t)) {
        send_nack(SF_CMD_SWAP_NACK, SF_NACK_IO_ERROR, "payload too small");
        return;
    }

    sf_swap_request_t req;
    memcpy(&req, payload, sizeof(req));

    printf("[sf_receiver] swap task: slot=%u  so=%s\n",
           req.slot, req.so_path);

    if (g_rx.config.on_swap)
        g_rx.config.on_swap((int)req.slot, req.so_path);

    send_ack(SF_CMD_SWAP_ACK);
}

// ── SF_CMD_STOP_TASK / SF_CMD_START_TASK / SF_CMD_RESTART_TASK ────

static void handle_task_control(uint16_t command,
                                 const uint8_t *payload, uint32_t len) {
    if (len < sizeof(sf_task_control_t)) {
        send_nack(SF_CMD_TASK_NACK, SF_NACK_IO_ERROR, "payload too small");
        return;
    }

    sf_task_control_t ctrl;
    memcpy(&ctrl, payload, sizeof(ctrl));

    int rc = 0;
    switch (command) {
        case SF_CMD_STOP_TASK:
            printf("[sf_receiver] stop task slot=%u\n", ctrl.slot);
            rc = supervisor_stop_task((int)ctrl.slot);
            break;
        case SF_CMD_START_TASK:
            printf("[sf_receiver] start task slot=%u\n", ctrl.slot);
            rc = supervisor_start_task((int)ctrl.slot);
            break;
        case SF_CMD_RESTART_TASK:
            printf("[sf_receiver] restart task slot=%u\n", ctrl.slot);
            rc = supervisor_restart_task((int)ctrl.slot);
            break;
        default:
            break;
    }

    if (rc == 0)
        send_ack(SF_CMD_TASK_ACK);
    else
        send_nack(SF_CMD_TASK_NACK, SF_NACK_BAD_SLOT, "task control failed");
}

// ── SF_CMD_OD_READ ────────────────────────────────────────────────
// OD requests are forwarded to the task's OD request ring.
// The response is picked up by a poll loop and sent back
// asynchronously on the same connection.

static void handle_od_read(const uint8_t *payload, uint32_t len) {
    //if (len < sizeof(sf_od_read_t)) {
    //    sf_od_error_t err = { .error_code = SF_NACK_IO_ERROR };
    //    snprintf(err.message, sizeof(err.message), "payload too small");
    //    sf_send_packet(SF_CMD_OD_ERROR, &err, sizeof(err));
    //    return;
    //}
//
    //sf_od_read_t req;
    //memcpy(&req, payload, sizeof(req));
//
    //int slot = supervisor_slot_for_id(req.task_id);
    //if (slot < 0) {
    //    sf_od_error_t err = { .error_code = SF_NACK_TASK_NOT_FOUND };
    //    snprintf(err.message, sizeof(err.message),
    //             "task 0x%02x not found", req.task_id);
    //    sf_send_packet(SF_CMD_OD_ERROR, &err, sizeof(err));
    //    return;
    //}
//
    //framework_shared_t *shared = framework_get_shared();
    //od_request_t od_req = {
    //    .index      = req.index,
    //    .subindex   = req.subindex,
    //    .task_id    = req.task_id,
    //    .is_write   = false,
    //    .request_id = req.request_id,
    //};
//
    //if (!od_req_ring_push(&shared->task_blocks[slot].od_req, &od_req)) {
    //    sf_od_error_t err = { .error_code = SF_NACK_IO_ERROR };
    //    snprintf(err.message, sizeof(err.message), "OD request queue full");
    //    sf_send_packet(SF_CMD_OD_ERROR, &err, sizeof(err));
    //    return;
    //}
//
    //// Response is collected by od_poll_loop and sent back asynchronously
    //g_rx.od_poll.pending    = true;
    //g_rx.od_poll.request_id = req.request_id;
    //g_rx.od_poll.task_id    = req.task_id;
    //g_rx.od_poll.task_slot  = slot;
}

// ── SF_CMD_OD_WRITE ───────────────────────────────────────────────

static void handle_od_write(const uint8_t *payload, uint32_t len) {
    //if (len < sizeof(sf_od_write_t)) {
    //    sf_od_error_t err = { .error_code = SF_NACK_IO_ERROR };
    //    snprintf(err.message, sizeof(err.message), "payload too small");
    //    sf_send_packet(SF_CMD_OD_ERROR, &err, sizeof(err));
    //    return;
    //}
//
    //sf_od_write_t req;
    //memcpy(&req, payload, sizeof(req));
//
    //int slot = supervisor_slot_for_id(req.task_id);
    //if (slot < 0) {
    //    sf_od_error_t err = { .error_code = SF_NACK_TASK_NOT_FOUND };
    //    snprintf(err.message, sizeof(err.message),
    //             "task 0x%02x not found", req.task_id);
    //    sf_send_packet(SF_CMD_OD_ERROR, &err, sizeof(err));
    //    return;
    //}
//
    //framework_shared_t *shared = framework_get_shared();
    //od_request_t od_req = {
    //    .index      = req.index,
    //    .subindex   = req.subindex,
    //    .task_id    = req.task_id,
    //    .is_write   = true,
    //    .request_id = req.request_id,
    //};
    //memcpy(od_req.data, req.data, sizeof(od_req.data));
//
    //if (!od_req_ring_push(&shared->task_blocks[slot].od_req, &od_req)) {
    //    sf_od_error_t err = { .error_code = SF_NACK_IO_ERROR };
    //    snprintf(err.message, sizeof(err.message), "OD request queue full");
    //    sf_send_packet(SF_CMD_OD_ERROR, &err, sizeof(err));
    //    return;
    //}
//
    //g_rx.od_poll.pending    = true;
    //g_rx.od_poll.request_id = req.request_id;
    //g_rx.od_poll.task_id    = req.task_id;
    //g_rx.od_poll.task_slot  = slot;
}

// ── SF_CMD_DEBUG_REQUEST ──────────────────────────────────────────

static void handle_debug_request(const uint8_t *payload, uint32_t len) {
    //if (len < sizeof(sf_debug_request_t)) {
    //    sf_debug_session_error_t err = {
    //        .reason = SF_DEBUG_ERR_TASK_NOT_FOUND
    //    };
    //    snprintf(err.message, sizeof(err.message), "payload too small");
    //    sf_send_packet(SF_CMD_DEBUG_SESSION_ERROR, &err, sizeof(err));
    //    return;
    //}
//
    //sf_debug_request_t req;
    //memcpy(&req, payload, sizeof(req));
//
    //sf_debug_session_info_t  info = {0};
    //sf_debug_session_error_t err  = {0};
//
    //if (debug_session_open(&req, &info, &err) == 0) {
    //    sf_send_packet(SF_CMD_DEBUG_SESSION_INFO, &info, sizeof(info));
    //} else {
    //    sf_send_packet(SF_CMD_DEBUG_SESSION_ERROR, &err, sizeof(err));
    //}
}

// ── SF_CMD_DEBUG_CLOSE ────────────────────────────────────────────

static void handle_debug_close(const uint8_t *payload, uint32_t len) {
    //if (len < sizeof(sf_debug_close_t)) return;
//
    //sf_debug_close_t req;
    //memcpy(&req, payload, sizeof(req));
//
    //debug_session_close(req.session_id);
//
    //sf_debug_closed_t closed = { .session_id = req.session_id };
    //sf_send_packet(SF_CMD_DEBUG_CLOSED, &closed, sizeof(closed));
}

// ── SF_CMD_STATUS_REQUEST ─────────────────────────────────────────

static void handle_status_request(void) {
    supervisor_status_t status;
    supervisor_get_status(&status);
    sf_send_packet(SF_CMD_STATUS_RESPONSE, &status, sizeof(status));
}

// ── SF_CMD_HEARTBEAT ──────────────────────────────────────────────

static void handle_heartbeat(void) {
    // Echo back — keeps the connection alive and confirms target is up
    send_ack(SF_CMD_HEARTBEAT);
}

// ── SF_CMD_SHUTDOWN ───────────────────────────────────────────────

static void handle_shutdown(void) {
    printf("[sf_receiver] shutdown command received from host\n");
    if (g_rx.config.on_shutdown)
        g_rx.config.on_shutdown();
}

// ─────────────────────────────────────────────────────────────────
// OD response poll — runs between packet reads
// ─────────────────────────────────────────────────────────────────

static void poll_od_responses(void) {
    //if (!g_rx.od_poll.pending) return;
//
    //framework_shared_t *shared = framework_get_shared();
    //int slot = g_rx.od_poll.task_slot;
//
    //od_response_t resp;
    //if (!od_resp_ring_pop(&shared->task_blocks[slot].od_resp, &resp))
    //    return;     // not ready yet — will check again next iteration
//
    //if (resp.request_id != g_rx.od_poll.request_id)
    //    return;     // stale response from a previous request
//
    //g_rx.od_poll.pending = false;
//
    //if (resp.success) {
    //    sf_od_response_t out = {
    //        .request_id = resp.request_id,
    //        .task_id    = g_rx.od_poll.task_id,
    //        .type       = (uint8_t)resp.type,
    //        .size       = (uint8_t)resp.size,
    //    };
    //    memcpy(out.data, resp.data, sizeof(out.data));
    //    sf_send_packet(SF_CMD_OD_RESPONSE, &out, sizeof(out));
    //} else {
    //    sf_od_error_t err = { .error_code = resp.error_code };
    //    snprintf(err.message, sizeof(err.message),
    //             "OD access failed (code %u)", resp.error_code);
    //    sf_send_packet(SF_CMD_OD_ERROR, &err, sizeof(err));
    //}
}

// ─────────────────────────────────────────────────────────────────
// Main command dispatch
// ─────────────────────────────────────────────────────────────────

static void dispatch(const sf_header_t *hdr,
                     const uint8_t *payload) {
    switch (hdr->command) {
        case SF_CMD_LOAD_PROJECT:
            handle_load_project(payload, hdr->payload_len);
            break;
        case SF_CMD_TRANSFER_BEGIN:
            handle_transfer_begin(payload, hdr->payload_len);
            break;
        case SF_CMD_TRANSFER_CHUNK:
            handle_transfer_chunk(payload, hdr->payload_len);
            break;
        case SF_CMD_TRANSFER_END:
            handle_transfer_end();
            break;
        case SF_CMD_SWAP_TASK:
            handle_swap_task(payload, hdr->payload_len);
            break;
        case SF_CMD_STOP_TASK:
        case SF_CMD_START_TASK:
        case SF_CMD_RESTART_TASK:
            handle_task_control(hdr->command, payload, hdr->payload_len);
            break;
        case SF_CMD_OD_READ:
            handle_od_read(payload, hdr->payload_len);
            break;
        case SF_CMD_OD_WRITE:
            handle_od_write(payload, hdr->payload_len);
            break;
        case SF_CMD_DEBUG_REQUEST:
            handle_debug_request(payload, hdr->payload_len);
            break;
        case SF_CMD_DEBUG_CLOSE:
            handle_debug_close(payload, hdr->payload_len);
            break;
        case SF_CMD_STATUS_REQUEST:
            handle_status_request();
            break;
        case SF_CMD_HEARTBEAT:
            handle_heartbeat();
            break;
        case SF_CMD_SHUTDOWN:
            handle_shutdown();
            break;
        default:
            fprintf(stderr, "[sf_receiver] unknown command: 0x%04X\n",
                    hdr->command);
            break;
    }
}

// ─────────────────────────────────────────────────────────────────
// Session thread — handles a single connected host
// ─────────────────────────────────────────────────────────────────

static void cleanup_session(void) {
    // Close any in-progress transfer
    if (g_rx.transfer.state == TRANSFER_ACTIVE) {
        if (g_rx.transfer.fd >= 0) {
            close(g_rx.transfer.fd);
            g_rx.transfer.fd = -1;
        }
        // Remove partial file
        if (strlen(g_rx.transfer.dest_path) > 0)
            unlink(g_rx.transfer.dest_path);
        g_rx.transfer.state = TRANSFER_IDLE;
    }

    // Cancel any pending OD request
    g_rx.od_poll.pending = false;

    // Close all debug sessions — host has disconnected
    //debug_session_close_all();

    // Close connection socket
    if (g_rx.conn_fd >= 0) {
        close(g_rx.conn_fd);
        g_rx.conn_fd = -1;
    }

    atomic_store(&g_rx.session_active, false);
    printf("[sf_receiver] session ended\n");
}

// Payload buffer — allocated once, reused for every packet.
// Maximum sensible payload: project JSON (64KB) or .so chunk (4KB).
// Use 128KB to be safe.
#define MAX_PAYLOAD_BUF  (128 * 1024)

static void *session_thread_fn(void *arg) {
    (void)arg;

    uint8_t *payload_buf = malloc(MAX_PAYLOAD_BUF);
    if (!payload_buf) {
        fprintf(stderr, "[sf_receiver] OOM allocating payload buffer\n");
        atomic_store(&g_rx.session_active, false);
        return NULL;
    }

    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    getpeername(g_rx.conn_fd, (struct sockaddr *)&peer, &peer_len);
    printf("[sf_receiver] host connected: %s:%u\n",
           inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

    // Set TCP keepalive so we detect silent disconnects
    int opt = 1;
    setsockopt(g_rx.conn_fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    opt = 5;  // start keepalive after 5s idle
    setsockopt(g_rx.conn_fd, IPPROTO_TCP, TCP_KEEPIDLE, &opt, sizeof(opt));
    opt = 1;  // interval between probes
    setsockopt(g_rx.conn_fd, IPPROTO_TCP, TCP_KEEPINTVL, &opt, sizeof(opt));
    opt = 3;  // drop after 3 missed probes
    setsockopt(g_rx.conn_fd, IPPROTO_TCP, TCP_KEEPCNT, &opt, sizeof(opt));

    // Disable Nagle — we control framing ourselves
    opt = 1;
    setsockopt(g_rx.conn_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    while (atomic_load(&g_rx.running)) {
        // ── Poll for OD responses before blocking on recv ─────────
        poll_od_responses();

        // ── Read header ───────────────────────────────────────────
        sf_header_t hdr;
        if (recv_exact(g_rx.conn_fd, &hdr, sizeof(hdr)) != 0)
            break;

        if (!header_valid(&hdr))
            break;

        // ── Read payload ──────────────────────────────────────────
        if (hdr.payload_len > MAX_PAYLOAD_BUF) {
            fprintf(stderr, "[sf_receiver] payload too large: %u bytes\n",
                    hdr.payload_len);
            break;
        }

        if (hdr.payload_len > 0) {
            if (recv_exact(g_rx.conn_fd, payload_buf,
                           hdr.payload_len) != 0)
                break;
        }

        // ── Dispatch ──────────────────────────────────────────────
        dispatch(&hdr, payload_buf);
    }

    free(payload_buf);
    cleanup_session();
    return NULL;
}

// ─────────────────────────────────────────────────────────────────
// Accept thread — waits for host connections
// ─────────────────────────────────────────────────────────────────

static void *accept_thread_fn(void *arg) {
    (void)arg;

    while (atomic_load(&g_rx.running)) {
        // Block until a host connects
        struct sockaddr_in client_addr;
        socklen_t          client_len = sizeof(client_addr);

        int conn = accept(g_rx.listen_fd,
                          (struct sockaddr *)&client_addr,
                          &client_len);

        if (conn < 0) {
            if (!atomic_load(&g_rx.running)) break;   // shutting down
            if (errno == EINTR)              continue; // signal — retry
            fprintf(stderr, "[sf_receiver] accept error: %s\n",
                    strerror(errno));
            continue;
        }

        // Only one host connection at a time
        if (atomic_load(&g_rx.session_active)) {
            fprintf(stderr, "[sf_receiver] rejecting second connection — "
                    "already have a host\n");
            close(conn);
            continue;
        }

        g_rx.conn_fd = conn;
        atomic_store(&g_rx.session_active, true);

        // Spawn session thread for this connection
        pthread_t tid;
        if (pthread_create(&tid, NULL, session_thread_fn, NULL) != 0) {
            fprintf(stderr, "[sf_receiver] pthread_create failed: %s\n",
                    strerror(errno));
            cleanup_session();
            continue;
        }
        pthread_detach(tid);
    }

    return NULL;
}

// ─────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────

int sf_receiver_start(const sf_receiver_config_t *cfg) {
    memset(&g_rx, 0, sizeof(g_rx));
    g_rx.config  = *cfg;
    g_rx.conn_fd = -1;
    g_rx.transfer.fd = -1;

    atomic_init(&g_rx.running,        true);
    atomic_init(&g_rx.session_active, false);
    pthread_mutex_init(&g_rx.send_lock, NULL);

    // ── Create listen socket ──────────────────────────────────────
    g_rx.listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_rx.listen_fd < 0) {
        fprintf(stderr, "[sf_receiver] socket failed: %s\n",
                strerror(errno));
        return -1;
    }

    // Allow rapid restart without TIME_WAIT delay
    int opt = 1;
    setsockopt(g_rx.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(cfg->port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(g_rx.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[sf_receiver] bind port %u failed: %s\n",
                cfg->port, strerror(errno));
        close(g_rx.listen_fd);
        return -1;
    }

    if (listen(g_rx.listen_fd, 1) != 0) {
        fprintf(stderr, "[sf_receiver] listen failed: %s\n",
                strerror(errno));
        close(g_rx.listen_fd);
        return -1;
    }

    // ── Start accept thread ───────────────────────────────────────
    if (pthread_create(&g_rx.accept_thread, NULL,
                        accept_thread_fn, NULL) != 0) {
        fprintf(stderr, "[sf_receiver] accept thread create failed: %s\n",
                strerror(errno));
        close(g_rx.listen_fd);
        return -1;
    }

    return 0;
}

void sf_receiver_stop(void) {
    atomic_store(&g_rx.running, false);

    // Unblock the accept thread by closing the listen socket
    if (g_rx.listen_fd >= 0) {
        shutdown(g_rx.listen_fd, SHUT_RDWR);
        close(g_rx.listen_fd);
        g_rx.listen_fd = -1;
    }

    // Unblock the session thread
    if (g_rx.conn_fd >= 0) {
        shutdown(g_rx.conn_fd, SHUT_RDWR);
    }

    pthread_join(g_rx.accept_thread, NULL);
    pthread_mutex_destroy(&g_rx.send_lock);

    printf("[sf_receiver] stopped\n");
}