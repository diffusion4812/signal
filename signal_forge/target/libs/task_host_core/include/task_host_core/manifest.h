#ifndef MANIFEST_H
#define MANIFEST_H

#include <stddef.h>
#include <stdint.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * FieldType — IEC 61131-3 elementary datatype codes
 *
 *   Compact enum carried in every FieldInfo so the host / HMI / runtime can
 *   interpret the raw bytes without relying on C sizeof alone.
 *
 *   Naming convention:  FIELD_TYPE_<IEC name>
 *   The numeric values are arbitrary but fixed — do NOT reorder.
 * ───────────────────────────────────────────────────────────────────────────── */
typedef enum {
    /* ── Bit types ────────────────────────────────────────────────────────── */
    FIELD_TYPE_BOOL         =  0,   /* IEC BOOL          — 1 bit (stored as 1 byte) */

    /* ── Integer types (unsigned) ─────────────────────────────────────────── */
    FIELD_TYPE_BYTE         =  1,   /* IEC BYTE          —  8-bit bit-string         */
    FIELD_TYPE_WORD         =  2,   /* IEC WORD          — 16-bit bit-string         */
    FIELD_TYPE_DWORD        =  3,   /* IEC DWORD         — 32-bit bit-string         */
    FIELD_TYPE_LWORD        =  4,   /* IEC LWORD         — 64-bit bit-string         */

    FIELD_TYPE_USINT        =  5,   /* IEC USINT         —  8-bit unsigned           */
    FIELD_TYPE_UINT         =  6,   /* IEC UINT          — 16-bit unsigned           */
    FIELD_TYPE_UDINT        =  7,   /* IEC UDINT         — 32-bit unsigned           */
    FIELD_TYPE_ULINT        =  8,   /* IEC ULINT         — 64-bit unsigned           */

    /* ── Integer types (signed) ───────────────────────────────────────────── */
    FIELD_TYPE_SINT         =  9,   /* IEC SINT          —  8-bit signed             */
    FIELD_TYPE_INT          = 10,   /* IEC INT           — 16-bit signed             */
    FIELD_TYPE_DINT         = 11,   /* IEC DINT          — 32-bit signed             */
    FIELD_TYPE_LINT         = 12,   /* IEC LINT          — 64-bit signed             */

    /* ── Floating-point types ─────────────────────────────────────────────── */
    FIELD_TYPE_REAL         = 13,   /* IEC REAL          — 32-bit IEEE 754           */
    FIELD_TYPE_LREAL        = 14,   /* IEC LREAL         — 64-bit IEEE 754           */

    /* ── Time types ───────────────────────────────────────────────────────── */
    FIELD_TYPE_TIME         = 15,   /* IEC TIME          — duration (impl-defined)   */
    FIELD_TYPE_LTIME        = 16,   /* IEC LTIME         — duration, 64-bit ns       */

    /* ── Date / time-of-day types ─────────────────────────────────────────── */
    FIELD_TYPE_DATE         = 17,   /* IEC DATE                                      */
    FIELD_TYPE_LDATE        = 18,   /* IEC LDATE                                     */
    FIELD_TYPE_TIME_OF_DAY  = 19,   /* IEC TIME_OF_DAY   (TOD)                       */
    FIELD_TYPE_LTIME_OF_DAY = 20,   /* IEC LTIME_OF_DAY  (LTOD)                      */
    FIELD_TYPE_DATE_AND_TIME = 21,  /* IEC DATE_AND_TIME (DT)                        */
    FIELD_TYPE_LDATE_AND_TIME = 22, /* IEC LDATE_AND_TIME (LDT)                      */

    /* ── String types ─────────────────────────────────────────────────────── */
    FIELD_TYPE_STRING       = 23,   /* IEC STRING        — single-byte char string   */
    FIELD_TYPE_WSTRING      = 24,   /* IEC WSTRING       — wide-char string          */

    /* ── Sentinel ─────────────────────────────────────────────────────────── */
    FIELD_TYPE_COUNT               /* Total number of type codes (not a valid type) */
} field_type_t;

/* ─────────────────────────────────────────────────────────────────────────────
 * Field category — distinguishes pin fields from static (internal state) fields
 * ───────────────────────────────────────────────────────────────────────────── */
#define FIELD_CAT_PIN    (0u << 2)   /* I/O pin: participates in graph wiring  */
#define FIELD_CAT_STATIC (1u << 2)   /* Internal state: not wired, host-accessible */

/* ─────────────────────────────────────────────────────────────────────────────
 * Pin direction (stored in bits [1:0] of flags when FIELD_CAT_PIN)
 * ───────────────────────────────────────────────────────────────────────────── */
#define FIELD_DIR_INPUT  0u
#define FIELD_DIR_OUTPUT 1u

/* ─────────────────────────────────────────────────────────────────────────────
 * Static host-access permission (stored in bits [1:0] of flags when FIELD_CAT_STATIC)
 *   RD  — host may read  (e.g. probe captured value)
 *   WR  — host may write (e.g. inject force_enable)
 *   RW  — host may read and write
 * ───────────────────────────────────────────────────────────────────────────── */
#define FIELD_ACCESS_NONE 0u
#define FIELD_ACCESS_RD   1u
#define FIELD_ACCESS_WR   2u
#define FIELD_ACCESS_RW   3u

/* Convenience masks to extract category or direction/access from flags */
#define FIELD_CAT_MASK    (1u << 2)
#define FIELD_ATTR_MASK   (3u)

/* ─────────────────────────────────────────────────────────────────────────────
 * FieldInfo — metadata for one field (pin or static) inside a block struct
 * ───────────────────────────────────────────────────────────────────────────── */
typedef struct {
    uint64_t     field_id;   /* Stable project/GUI ID                              */
    const char*  instance;   /* Instance name of the block in the graph            */
    const char*  name;       /* Symbol name of the field in the C struct           */
    uint32_t     flags;      /* FIELD_CAT_* | FIELD_DIR_* or FIELD_ACCESS_*        */
    field_type_t type;       /* IEC 61131-3 datatype of the field                  */
    uint64_t     size;       /* sizeof the field in bytes                          */
    uint64_t     offset;     /* offsetof the field in the block struct             */
} FieldInfo;

/* ─────────────────────────────────────────────────────────────────────────────
 * FIELD_ENTRY  — use for INPUT / OUTPUT pins
 *
 *   struct_t  : the block's C struct type
 *   id        : stable field_id integer
 *   dir       : FIELD_DIR_INPUT or FIELD_DIR_OUTPUT
 *   iec_type  : FIELD_TYPE_* constant (IEC datatype)
 *   field     : unquoted field name in struct_t
 * ───────────────────────────────────────────────────────────────────────────── */
#define FIELD_ENTRY(struct_t, id, dir, iec_type, instance, field) \
    {                                                             \
        (uint64_t)(id),                                           \
        #instance,                                                \
        #field,                                                   \
        (uint32_t)(FIELD_CAT_PIN | (dir)),                        \
        (field_type_t)(iec_type),                                 \
        (uint64_t)sizeof(((struct_t*)0)->field),                  \
        (uint64_t)offsetof(struct_t, field)                       \
    }

/* ─────────────────────────────────────────────────────────────────────────────
 * STATIC_ENTRY  — use for internal-state / host-accessible static fields
 *
 *   struct_t  : the block's C struct type
 *   id        : stable field_id integer
 *   access    : FIELD_ACCESS_NONE | _RD | _WR | _RW
 *   iec_type  : FIELD_TYPE_* constant (IEC datatype)
 *   field     : unquoted field name in struct_t
 * ───────────────────────────────────────────────────────────────────────────── */
#define STATIC_ENTRY(struct_t, id, access, iec_type, instance, field) \
    {                                                                 \
        (uint64_t)(id),                                               \
        #instance,                                                    \
        #field,                                                       \
        (uint32_t)(FIELD_CAT_STATIC | (access)),                      \
        (field_type_t)(iec_type),                                     \
        (uint64_t)sizeof(((struct_t*)0)->field),                      \
        (uint64_t)offsetof(struct_t, field)                           \
    }

/* ─────────────────────────────────────────────────────────────────────────────
 * BlockRegistryEntry — one entry per block instance in the generated task
 * ───────────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t         block_id;       /* Stable project/GUI ID                    */
    const char*      block_name;     /* Block type name (e.g. "sf_pid")          */
    const char*      instance_name;  /* Instance name   (e.g. "speed_pid")       */
    uint64_t         block_sig;      /* Layout fingerprint — must match manifest */
    uint64_t         block_size;     /* sizeof the block struct                  */
    uint64_t         field_count;    /* Total entries in the fields array        */
    const FieldInfo* fields;         /* Pins + statics in declaration order      */
} block_reg_entry_t;

#endif /* MANIFEST_H */