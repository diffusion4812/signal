#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SF_MAGIC                0x5346      // 'SF'
#define SF_PROTOCOL_VERSION     1
#define SF_MAX_CHUNK_SIZE       4096
#define SF_HEADER_CRC_POLY      0x1021

#pragma pack(push, 1)

// ── Commands ──────────────────────────────────────────────────────
typedef enum {
    // Project control
    SF_CMD_LOAD_PROJECT         = 0x0001,
    SF_CMD_PROJECT_ACK          = 0x0002,
    SF_CMD_PROJECT_NACK         = 0x0003,

    // .so transfer
    SF_CMD_TRANSFER_BEGIN       = 0x0010,
    SF_CMD_TRANSFER_CHUNK       = 0x0011,
    SF_CMD_TRANSFER_END         = 0x0012,
    SF_CMD_TRANSFER_ACK         = 0x0013,
    SF_CMD_TRANSFER_NACK        = 0x0014,

    // Task control
    SF_CMD_SWAP_TASK            = 0x0020,
    SF_CMD_SWAP_ACK             = 0x0021,
    SF_CMD_SWAP_NACK            = 0x0022,
    SF_CMD_STOP_TASK            = 0x0030,
    SF_CMD_START_TASK           = 0x0031,
    SF_CMD_RESTART_TASK         = 0x0032,
    SF_CMD_TASK_ACK             = 0x0033,
    SF_CMD_TASK_NACK            = 0x0034,
    SF_CMD_SHUTDOWN             = 0x003F,

    // OD access
    SF_CMD_OD_READ              = 0x0040,
    SF_CMD_OD_WRITE             = 0x0041,
    SF_CMD_OD_RESPONSE          = 0x0042,
    SF_CMD_OD_ERROR             = 0x0043,

    // Debug session
    SF_CMD_DEBUG_REQUEST        = 0x0050,
    SF_CMD_DEBUG_SESSION_INFO   = 0x0051,
    SF_CMD_DEBUG_SESSION_ERROR  = 0x0052,
    SF_CMD_DEBUG_CLOSE          = 0x0053,
    SF_CMD_DEBUG_CLOSED         = 0x0054,

    // Status / heartbeat
    SF_CMD_HEARTBEAT            = 0x00F0,
    SF_CMD_STATUS_REQUEST       = 0x00F1,
    SF_CMD_STATUS_RESPONSE      = 0x00F2,

} sf_command_t;

// ── Packet header — every packet starts with this ─────────────────
typedef struct {
    uint16_t     magic;             // SF_MAGIC
    uint8_t      version;           // SF_PROTOCOL_VERSION
    uint8_t      flags;             // reserved, set to 0
    uint16_t     command;           // sf_command_t
    uint32_t     sequence;          // incremented by sender
    uint32_t     payload_len;       // bytes following header
    uint16_t     header_crc;        // CRC-16 over preceding header bytes
} sf_header_t;

// ── Payload structs ───────────────────────────────────────────────

// SF_CMD_LOAD_PROJECT — payload is raw JSON bytes (no struct needed)
// SF_CMD_PROJECT_ACK  — no payload

typedef struct {
    uint8_t  reason;
    char     message[128];
} sf_nack_t;

// SF_CMD_TRANSFER_BEGIN
typedef struct {
    char     filename[128];         // e.g. "task1_v2.so"
    uint8_t  target_slot;           // slot to swap after transfer
    uint32_t total_size;            // total .so file size in bytes
    uint32_t crc32;                 // CRC-32 of complete file
} sf_transfer_begin_t;

// SF_CMD_TRANSFER_CHUNK
typedef struct {
    uint32_t offset;                // byte offset within file
    uint16_t length;                // bytes in this chunk
    uint8_t  data[SF_MAX_CHUNK_SIZE];
} sf_transfer_chunk_t;

// SF_CMD_TRANSFER_END — no payload beyond header
// SF_CMD_TRANSFER_ACK — no payload
// SF_CMD_TRANSFER_NACK — sf_nack_t

// SF_CMD_SWAP_TASK
typedef struct {
    uint8_t  slot;
    char     so_path[256];          // path on target after transfer
} sf_swap_request_t;

// SF_CMD_STOP_TASK / SF_CMD_START_TASK / SF_CMD_RESTART_TASK
typedef struct {
    uint8_t  slot;
} sf_task_control_t;

// SF_CMD_OD_READ
typedef struct {
    uint8_t  task_id;
    uint16_t index;
    uint8_t  subindex;
    uint32_t request_id;
} sf_od_read_t;

// SF_CMD_OD_WRITE
typedef struct {
    uint8_t  task_id;
    uint16_t index;
    uint8_t  subindex;
    uint32_t request_id;
    uint8_t  type;                  // od_type_t
    uint8_t  size;
    uint8_t  data[8];
} sf_od_write_t;

// SF_CMD_OD_RESPONSE
typedef struct {
    uint32_t request_id;
    uint8_t  task_id;
    uint16_t index;
    uint8_t  subindex;
    uint8_t  type;                  // od_type_t
    uint8_t  size;
    uint8_t  data[8];
} sf_od_response_t;

// SF_CMD_OD_ERROR
typedef struct {
    uint32_t request_id;
    uint8_t  error_code;
    char     message[64];
} sf_od_error_t;

// SF_CMD_DEBUG_REQUEST
typedef struct {
    uint8_t  task_id;
    uint32_t sample_rate_us;        // 0 = task's natural rate
    uint16_t filter_count;          // 0 = all published entries
    uint16_t od_filters[32];        // OD indices to stream
} sf_debug_request_t;

// SF_CMD_DEBUG_SESSION_INFO
typedef struct {
    uint32_t session_id;
    uint8_t  task_id;
    uint16_t udp_port;
    uint32_t actual_rate_us;
} sf_debug_session_info_t;

// SF_CMD_DEBUG_SESSION_ERROR
typedef struct {
    uint8_t  reason;
    char     message[64];
} sf_debug_session_error_t;

// SF_CMD_DEBUG_CLOSE
typedef struct {
    uint32_t session_id;
} sf_debug_close_t;

// SF_CMD_DEBUG_CLOSED
typedef struct {
    uint32_t session_id;
} sf_debug_closed_t;

// SF_CMD_STATUS_RESPONSE — payload is supervisor_status_t directly
// (defined in project.h)

// ── Transfer NACK reason codes ────────────────────────────────────
#define SF_NACK_BAD_CRC         0x01
#define SF_NACK_IO_ERROR        0x02
#define SF_NACK_BAD_SLOT        0x03
#define SF_NACK_NOT_IN_TRANSFER 0x04
#define SF_NACK_ALREADY_ACTIVE  0x05
#define SF_NACK_TASK_NOT_FOUND  0x06
#define SF_NACK_SIZE_MISMATCH   0x07

// ── Debug session error codes ─────────────────────────────────────
#define SF_DEBUG_ERR_NO_PORTS       0x01
#define SF_DEBUG_ERR_TASK_NOT_FOUND 0x02
#define SF_DEBUG_ERR_ALREADY_OPEN   0x03
#define SF_DEBUG_ERR_LIB_FAILED     0x04

#pragma pack(pop)