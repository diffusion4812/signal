#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>

#pragma pack(push, 1)

/* ── Limits ─────────────────────────────────────────────── */
#define DBG_PROTOCOL_VERSION   2
#define DBG_MAX_BATCH_ITEMS    64

/* ── Enumerations (unchanged) ───────────────────────────── */
typedef enum {
    DBG_OP_READ  = 0,
    DBG_OP_WRITE = 1,
} dbg_op_t;

typedef enum {
    DBG_VT_BOOL = 1,
    DBG_VT_U32  = 2,
    DBG_VT_I32  = 3,
    DBG_VT_U64  = 4,
    DBG_VT_I64  = 5,
    DBG_VT_F32  = 6,
    DBG_VT_F64  = 7,
} dbg_value_type_t;

typedef enum {
    DBG_OK                 =    0,
    DBG_ERR_UNKNOWN_FIELD  =   -1,
    DBG_ERR_ACCESS_DENIED  =   -2,
    DBG_ERR_TYPE_MISMATCH  =   -3,
    DBG_ERR_OUT_OF_RANGE   =   -4,
    DBG_ERR_BATCH_TOO_LARGE =  -5,   /* NEW */
    DBG_ERR_PARTIAL_FAILURE =  -6,   /* NEW: batch-level hint */
    DBG_ERR_INTERNAL       = -128,
} dbg_status_t;

/* ── Batch flags ────────────────────────────────────────── */
typedef enum {
    DBG_BATCH_FLAG_NONE     = 0x00,
    DBG_BATCH_FLAG_ATOMIC   = 0x01,  /* All-or-nothing: roll back on any error */
} dbg_batch_flags_t;

/* ── Per-item structures (used inside batch arrays) ─────── */

typedef union {
    uint64_t u64;
    int64_t  i64;
    double   f64;
    float    f32;
    uint8_t  b;
} dbg_value_t;

typedef struct {
    uint64_t     field_id;
    uint8_t      op;          /* dbg_op_t: READ or WRITE           */
    uint8_t      value_type;  /* dbg_value_type_t                   */
    uint16_t     value_len;   /* 0 for READ; 1..8 for WRITE        */
    uint32_t     _rsvd;       /* alignment / future use             */
    dbg_value_t  value;       /* payload for WRITE; ignored on READ */
} DebugRequestItem;

typedef struct {
    uint64_t     field_id;    /* echo back                          */
    int16_t      status;      /* per-item dbg_status_t              */
    uint8_t      value_type;  /* dbg_value_type_t                   */
    uint8_t      _rsvd1;
    uint32_t     _rsvd2;
    dbg_value_t  value;       /* READ: current value; WRITE: echoed */
} DebugReplyItem;

/* ── Batch request ──────────────────────────────────────── */
typedef struct {
    uint32_t           tx_id;
    uint16_t           version;       /* DBG_PROTOCOL_VERSION (2)  */
    uint8_t            flags;         /* dbg_batch_flags_t         */
    uint8_t            _rsvd;
    uint16_t           item_count;    /* 1 .. DBG_MAX_BATCH_ITEMS  */
    uint16_t           _pad;
    /*
     * Followed by item_count × DebugRequestItem in contiguous memory.
     * For convenience we declare a flexible array member.
     */
    DebugRequestItem   items[];
} DebugBatchRequest;

/* ── Batch reply ────────────────────────────────────────── */
typedef struct {
    uint32_t           tx_id;         /* echo back                 */
    uint16_t           version;
    int16_t            status;        /* batch-level status        */
    uint16_t           item_count;    /* same as request           */
    uint16_t           _pad;
    DebugReplyItem     items[];
} DebugBatchReply;

/* ── Legacy single-field wrappers (version 1 compat) ────── */

typedef struct {
    uint32_t     tx_id;
    uint16_t     version;     /* 1 */
    uint16_t     op;
    uint64_t     field_id;
    uint8_t      value_type;
    uint8_t      flags;
    uint16_t     value_len;
    dbg_value_t  value;
} DebugRequest;              /* unchanged layout */

typedef struct {
    uint32_t     tx_id;
    int16_t      status;
    uint16_t     version;
    uint64_t     field_id;
    uint8_t      value_type;
    uint8_t      _rsvd1;
    uint16_t     _rsvd2;
    dbg_value_t  value;
} DebugReply;                /* unchanged layout */

#pragma pack(pop)

/* ── Helper: compute wire sizes ─────────────────────────── */
static inline uint32_t dbg_batch_request_size(uint16_t n) {
    return (uint32_t)(sizeof(DebugBatchRequest)
                      + (uint32_t)n * sizeof(DebugRequestItem));
}

static inline uint32_t dbg_batch_reply_size(uint16_t n) {
    return (uint32_t)(sizeof(DebugBatchReply)
                      + (uint32_t)n * sizeof(DebugReplyItem));
}

#endif /* DEBUG_H */