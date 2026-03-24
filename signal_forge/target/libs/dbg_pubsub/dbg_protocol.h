/******************************************************************************
 * dbg_protocol.h
 *
 * Wire-format definitions for the debug publish/subscribe protocol.
 *
 * Protocol version: 3
 *
 * Message flow:
 *   CONFIG PHASE  (request/reply on config port)
 *     - SUBSCRIBE / SUBSCRIBE_ACK
 *     - UNSUBSCRIBE / UNSUBSCRIBE_ACK
 *     - WRITE / WRITE_ACK
 *     - FIELD_LIST_REQUEST / FIELD_LIST_RESPONSE
 *
 *   STREAMING PHASE (fire-and-forget on data port)
 *     - FRAME
 *
 * Byte order: Little-endian throughout.
 * All structures are packed (no padding).
 *
 * (c) 2025 — Internal use only.
 *****************************************************************************/

#ifndef DBG_PROTOCOL_H
#define DBG_PROTOCOL_H

#include <stdint.h>
#include <string.h>

#pragma pack(push, 1)

/* ══════════════════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════════════════ */

#define DBG_PROTOCOL_VERSION       3u
#define DBG_MAX_SUB_FIELDS         128u
#define DBG_MAX_SUBSCRIPTIONS      8u
#define DBG_MAX_WRITE_ITEMS        64u
#define DBG_MAX_FRAME_PAYLOAD      1400u   /* stays below 1472 UDP MTU    */
#define DBG_DATA_PORT_DEFAULT      9500u
#define DBG_CONFIG_PORT_DEFAULT    9501u
#define DBG_MAGIC                  0x44425Eu /* "DB^" — quick validation  */

/* ══════════════════════════════════════════════════════════════════════════
   MESSAGE TYPES
   ══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    /* Configuration (request/reply) */
    DBG_MSG_SUBSCRIBE           = 0x10,
    DBG_MSG_SUBSCRIBE_ACK       = 0x11,
    DBG_MSG_UNSUBSCRIBE         = 0x12,
    DBG_MSG_UNSUBSCRIBE_ACK     = 0x13,

    /* On-demand writes */
    DBG_MSG_WRITE               = 0x20,
    DBG_MSG_WRITE_ACK           = 0x21,

    /* Cyclic data */
    DBG_MSG_FRAME               = 0x30,

    /* Discovery */
    DBG_MSG_FIELD_LIST_REQUEST  = 0x40,
    DBG_MSG_FIELD_LIST_RESPONSE = 0x41,

    /* Housekeeping */
    DBG_MSG_HEARTBEAT           = 0x50,
    DBG_MSG_HEARTBEAT_ACK       = 0x51,
} dbg_msg_type_t;

/* ══════════════════════════════════════════════════════════════════════════
   VALUE TYPES
   ══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    DBG_VT_BOOL = 1,
    DBG_VT_U8   = 2,
    DBG_VT_I8   = 3,
    DBG_VT_U16  = 4,
    DBG_VT_I16  = 5,
    DBG_VT_U32  = 6,
    DBG_VT_I32  = 7,
    DBG_VT_U64  = 8,
    DBG_VT_I64  = 9,
    DBG_VT_F32  = 10,
    DBG_VT_F64  = 11,
} dbg_value_type_t;

/* ══════════════════════════════════════════════════════════════════════════
   STATUS CODES
   ══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    DBG_OK                      =    0,
    DBG_ERR_UNKNOWN_FIELD       =   -1,
    DBG_ERR_ACCESS_DENIED       =   -2,
    DBG_ERR_TYPE_MISMATCH       =   -3,
    DBG_ERR_OUT_OF_RANGE        =   -4,
    DBG_ERR_TOO_MANY_FIELDS     =   -5,
    DBG_ERR_TOO_MANY_SUBS       =   -6,
    DBG_ERR_PAYLOAD_OVERFLOW    =   -7,
    DBG_ERR_UNKNOWN_SUB         =   -8,
    DBG_ERR_BAD_VERSION         =   -9,
    DBG_ERR_BAD_MSG_TYPE        =  -10,
    DBG_ERR_BAD_MAGIC           =  -11,
    DBG_ERR_MALFORMED           =  -12,
    DBG_ERR_INTERNAL            = -128,
} dbg_status_t;

/* ══════════════════════════════════════════════════════════════════════════
   ACCESS FLAGS
   ══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    DBG_ACCESS_READ_ONLY  = 0x00,
    DBG_ACCESS_READ_WRITE = 0x01,
} dbg_access_t;

/* ══════════════════════════════════════════════════════════════════════════
   VALUE UNION
   ══════════════════════════════════════════════════════════════════════════ */

typedef union {
    uint8_t  b;
    uint8_t  u8;
    int8_t   i8;
    uint16_t u16;
    int16_t  i16;
    uint32_t u32;
    int32_t  i32;
    uint64_t u64;
    int64_t  i64;
    float    f32;
    double   f64;
} dbg_value_t;

/* ══════════════════════════════════════════════════════════════════════════
   COMMON HEADER  (first 12 bytes of every message)
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t  magic;          /* DBG_MAGIC — fast rejection of junk     */
    uint16_t  version;        /* DBG_PROTOCOL_VERSION                    */
    uint16_t  msg_type;       /* dbg_msg_type_t                          */
    uint32_t  tx_id;          /* transaction id, echoed in replies       */
} dbg_header_t;

/* ══════════════════════════════════════════════════════════════════════════
   SUBSCRIBE  (client → server)
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t          field_id;
    uint8_t           value_type;   /* dbg_value_type_t */
    uint8_t           _pad[3];
} dbg_sub_field_t;

typedef struct {
    dbg_header_t      header;       /* msg_type = DBG_MSG_SUBSCRIBE      */
    uint16_t          sub_id;       /* client-chosen handle              */
    uint16_t          data_port;    /* Subscriber's listening data port  */
    uint16_t          field_count;  /* 1 .. DBG_MAX_SUB_FIELDS           */
    uint32_t          interval_us;  /* desired publish interval          */
    /* Followed by field_count × dbg_sub_field_t */
    dbg_sub_field_t   fields[];
} dbg_subscribe_request_t;

/* ══════════════════════════════════════════════════════════════════════════
   SUBSCRIBE ACK  (server → client)
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t  field_index;    /* ordinal in the subscription              */
    uint16_t  offset;         /* byte offset into frame payload           */
    uint8_t   value_type;     /* confirmed dbg_value_type_t               */
    uint8_t   size;           /* bytes occupied in frame (1,2,4,8)        */
    int16_t   status;         /* per-field status (DBG_OK or error)       */
} dbg_sub_field_ack_t;

typedef struct {
    dbg_header_t          header;     /* msg_type = DBG_MSG_SUBSCRIBE_ACK */
    uint16_t              sub_id;     /* echo back                         */
    int16_t               status;     /* overall status                    */
    uint16_t              field_count;
    uint16_t              frame_size; /* total payload bytes per frame     */
    uint32_t              actual_interval_us;
    /* Followed by field_count × dbg_sub_field_ack_t */
    dbg_sub_field_ack_t   fields[];
} dbg_subscribe_ack_t;

/* ══════════════════════════════════════════════════════════════════════════
   UNSUBSCRIBE  (client → server)
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    dbg_header_t  header;      /* msg_type = DBG_MSG_UNSUBSCRIBE         */
    uint16_t      sub_id;
    uint16_t      _pad;
} dbg_unsubscribe_request_t;

typedef struct {
    dbg_header_t  header;      /* msg_type = DBG_MSG_UNSUBSCRIBE_ACK     */
    uint16_t      sub_id;
    int16_t       status;
} dbg_unsubscribe_ack_t;

/* ══════════════════════════════════════════════════════════════════════════
   FRAME  (server → client, cyclic, fire-and-forget)
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    dbg_header_t  header;       /* msg_type = DBG_MSG_FRAME              */
    uint16_t      sub_id;
    uint16_t      frame_size;   /* bytes in payload[]                    */
    uint32_t      sequence;     /* monotonic per subscription            */
    uint64_t      timestamp_us; /* source time, µs since boot/epoch     */
    /* Followed by frame_size bytes of tightly packed field values,
       laid out exactly as described in the subscribe ack. */
    uint8_t       payload[];
} dbg_frame_t;

/* ══════════════════════════════════════════════════════════════════════════
   WRITE  (client → server, on-demand)
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t      field_id;
    uint8_t       value_type;   /* dbg_value_type_t */
    uint8_t       _pad[3];
    dbg_value_t   value;
} dbg_write_item_t;

typedef struct {
    dbg_header_t      header;       /* msg_type = DBG_MSG_WRITE          */
    uint16_t          item_count;   /* 1 .. DBG_MAX_WRITE_ITEMS          */
    uint16_t          _pad;
    /* Followed by item_count × dbg_write_item_t */
    dbg_write_item_t  items[];
} dbg_write_request_t;

/* ── Write ack ──────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t  field_id;
    int16_t   status;           /* per-item result                       */
    uint16_t  _pad;
} dbg_write_result_t;

typedef struct {
    dbg_header_t       header;     /* msg_type = DBG_MSG_WRITE_ACK       */
    uint16_t           item_count;
    int16_t            status;     /* aggregate status                    */
    /* Followed by item_count × dbg_write_result_t */
    dbg_write_result_t items[];
} dbg_write_ack_t;

/* ══════════════════════════════════════════════════════════════════════════
   FIELD LIST DISCOVERY  (client → server → client)
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    dbg_header_t  header;      /* msg_type = DBG_MSG_FIELD_LIST_REQUEST  */
    uint16_t      offset;      /* pagination: start index                */
    uint16_t      max_count;   /* max fields to return (0 = all)         */
} dbg_field_list_request_t;

#define DBG_FIELD_NAME_MAX  32u

typedef struct {
    uint64_t  field_id;
    uint8_t   value_type;       /* dbg_value_type_t                      */
    uint8_t   access;           /* dbg_access_t                          */
    uint16_t  _pad;
    char      name[DBG_FIELD_NAME_MAX]; /* null-terminated, truncated    */
} dbg_field_descriptor_t;

typedef struct {
    dbg_header_t            header;  /* msg_type = DBG_MSG_FIELD_LIST_RESPONSE */
    uint16_t                total_fields;  /* total in registry           */
    uint16_t                field_count;   /* returned in this message    */
    uint16_t                offset;        /* echo starting index         */
    int16_t                 status;
    /* Followed by field_count × dbg_field_descriptor_t */
    dbg_field_descriptor_t  fields[];
} dbg_field_list_response_t;

/* ══════════════════════════════════════════════════════════════════════════
   HEARTBEAT  (bidirectional keepalive)
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    dbg_header_t  header;       /* msg_type = DBG_MSG_HEARTBEAT          */
    uint64_t      timestamp_us;
    uint16_t      active_subs;  /* server: number of active subscriptions*/
    uint16_t      _pad;
} dbg_heartbeat_t;

typedef struct {
    dbg_header_t  header;       /* msg_type = DBG_MSG_HEARTBEAT_ACK      */
    uint64_t      timestamp_us; /* echo for RTT measurement              */
} dbg_heartbeat_ack_t;

#pragma pack(pop)

/* ══════════════════════════════════════════════════════════════════════════
   UTILITY FUNCTIONS
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief   Return the byte size for a given value type.
 * @param   vt   Value type enum.
 * @return  Size in bytes (1, 2, 4, or 8). Returns 0 on unknown type.
 */
static inline uint8_t dbg_value_type_size(dbg_value_type_t vt)
{
    switch (vt) {
    case DBG_VT_BOOL: return 1u;
    case DBG_VT_U8:   return 1u;
    case DBG_VT_I8:   return 1u;
    case DBG_VT_U16:  return 2u;
    case DBG_VT_I16:  return 2u;
    case DBG_VT_U32:  return 4u;
    case DBG_VT_I32:  return 4u;
    case DBG_VT_U64:  return 8u;
    case DBG_VT_I64:  return 8u;
    case DBG_VT_F32:  return 4u;
    case DBG_VT_F64:  return 8u;
    default:          return 0u;
    }
}

/**
 * @brief   Compute the total wire size of a subscribe request.
 * @param   n   Number of fields.
 * @return  Size in bytes.
 */
static inline uint32_t dbg_subscribe_request_size(uint16_t n)
{
    return (uint32_t)(sizeof(dbg_subscribe_request_t)
                      + (uint32_t)n * sizeof(dbg_sub_field_t));
}

/**
 * @brief   Compute the total wire size of a subscribe ack.
 * @param   n   Number of fields.
 * @return  Size in bytes.
 */
static inline uint32_t dbg_subscribe_ack_size(uint16_t n)
{
    return (uint32_t)(sizeof(dbg_subscribe_ack_t)
                      + (uint32_t)n * sizeof(dbg_sub_field_ack_t));
}

/**
 * @brief   Compute the total wire size of a data frame.
 * @param   payload_size   Payload bytes.
 * @return  Size in bytes.
 */
static inline uint32_t dbg_frame_wire_size(uint16_t payload_size)
{
    return (uint32_t)(sizeof(dbg_frame_t) + payload_size);
}

/**
 * @brief   Compute the total wire size of a write request.
 * @param   n   Number of write items.
 * @return  Size in bytes.
 */
static inline uint32_t dbg_write_request_size(uint16_t n)
{
    return (uint32_t)(sizeof(dbg_write_request_t)
                      + (uint32_t)n * sizeof(dbg_write_item_t));
}

/**
 * @brief   Compute the total wire size of a write ack.
 * @param   n   Number of write result items.
 * @return  Size in bytes.
 */
static inline uint32_t dbg_write_ack_size(uint16_t n)
{
    return (uint32_t)(sizeof(dbg_write_ack_t)
                      + (uint32_t)n * sizeof(dbg_write_result_t));
}

/**
 * @brief   Compute the total wire size of a field list response.
 * @param   n   Number of field descriptors.
 * @return  Size in bytes.
 */
static inline uint32_t dbg_field_list_response_size(uint16_t n)
{
    return (uint32_t)(sizeof(dbg_field_list_response_t)
                      + (uint32_t)n * sizeof(dbg_field_descriptor_t));
}

/**
 * @brief   Compute the frame payload size for a set of fields.
 * @param   types        Array of value types.
 * @param   count        Number of fields.
 * @param   out_offsets  (Optional) output array of per-field byte offsets.
 * @return  Total payload size in bytes.  0 if any type is invalid.
 */
static inline uint16_t dbg_compute_frame_layout(
    const dbg_value_type_t *types,
    uint16_t count,
    uint16_t *out_offsets)
{
    uint16_t offset = 0u;
    for (uint16_t i = 0u; i < count; i++) {
        uint8_t sz = dbg_value_type_size(types[i]);
        if (sz == 0u) return 0u;
        if (out_offsets) {
            out_offsets[i] = offset;
        }
        offset += sz;
    }
    return offset;
}

/**
 * @brief   Initialise a message header with standard fields.
 * @param   hdr       Pointer to the header to fill.
 * @param   msg_type  Message type.
 * @param   tx_id     Transaction ID.
 */
static inline void dbg_header_init(dbg_header_t *hdr,
                                   dbg_msg_type_t msg_type,
                                   uint32_t tx_id)
{
    hdr->magic    = DBG_MAGIC;
    hdr->version  = DBG_PROTOCOL_VERSION;
    hdr->msg_type = (uint16_t)msg_type;
    hdr->tx_id    = tx_id;
}

/**
 * @brief   Validate a received message header.
 * @param   hdr       Pointer to the received header.
 * @return  DBG_OK on success, or a specific error code.
 */
static inline dbg_status_t dbg_header_validate(const dbg_header_t *hdr)
{
    if (hdr->magic != DBG_MAGIC)              return DBG_ERR_BAD_MAGIC;
    if (hdr->version != DBG_PROTOCOL_VERSION) return DBG_ERR_BAD_VERSION;
    return DBG_OK;
}

/**
 * @brief   Return a human-readable string for a status code.
 * @param   s   Status code.
 * @return  Constant string.
 */
static inline const char* dbg_status_str(dbg_status_t s)
{
    switch (s) {
    case DBG_OK:                   return "OK";
    case DBG_ERR_UNKNOWN_FIELD:    return "Unknown field";
    case DBG_ERR_ACCESS_DENIED:    return "Access denied";
    case DBG_ERR_TYPE_MISMATCH:    return "Type mismatch";
    case DBG_ERR_OUT_OF_RANGE:     return "Out of range";
    case DBG_ERR_TOO_MANY_FIELDS:  return "Too many fields";
    case DBG_ERR_TOO_MANY_SUBS:    return "Too many subscriptions";
    case DBG_ERR_PAYLOAD_OVERFLOW: return "Payload overflow";
    case DBG_ERR_UNKNOWN_SUB:      return "Unknown subscription";
    case DBG_ERR_BAD_VERSION:      return "Bad protocol version";
    case DBG_ERR_BAD_MSG_TYPE:     return "Bad message type";
    case DBG_ERR_BAD_MAGIC:        return "Bad magic number";
    case DBG_ERR_MALFORMED:        return "Malformed message";
    case DBG_ERR_INTERNAL:         return "Internal error";
    default:                       return "Unknown error";
    }
}

/**
 * @brief   Return a human-readable string for a value type.
 * @param   vt  Value type.
 * @return  Constant string.
 */
static inline const char* dbg_value_type_str(dbg_value_type_t vt)
{
    switch (vt) {
    case DBG_VT_BOOL: return "bool";
    case DBG_VT_U8:   return "u8";
    case DBG_VT_I8:   return "i8";
    case DBG_VT_U16:  return "u16";
    case DBG_VT_I16:  return "i16";
    case DBG_VT_U32:  return "u32";
    case DBG_VT_I32:  return "i32";
    case DBG_VT_U64:  return "u64";
    case DBG_VT_I64:  return "i64";
    case DBG_VT_F32:  return "f32";
    case DBG_VT_F64:  return "f64";
    default:          return "unknown";
    }
}

/**
 * @brief   Parse a human-readable string into a value type.
 * @param   str  Null-terminated type string (e.g. "f64", "u32").
 * @return  Corresponding value type, or DBG_VT_UNKNOWN if unrecognized.
 */
static inline dbg_value_type_t dbg_value_type_from_str(const char* str)
{
    if (!str)
        return DBG_VT_F32;

    if (strcmp(str, "bool") == 0) return DBG_VT_BOOL;
    if (strcmp(str, "u8")   == 0) return DBG_VT_U8;
    if (strcmp(str, "i8")   == 0) return DBG_VT_I8;
    if (strcmp(str, "u16")  == 0) return DBG_VT_U16;
    if (strcmp(str, "i16")  == 0) return DBG_VT_I16;
    if (strcmp(str, "u32")  == 0) return DBG_VT_U32;
    if (strcmp(str, "i32")  == 0) return DBG_VT_I32;
    if (strcmp(str, "u64")  == 0) return DBG_VT_U64;
    if (strcmp(str, "i64")  == 0) return DBG_VT_I64;
    if (strcmp(str, "f32")  == 0) return DBG_VT_F32;
    if (strcmp(str, "f64")  == 0) return DBG_VT_F64;

    return DBG_VT_F32;
}

#endif /* DBG_PROTOCOL_H */