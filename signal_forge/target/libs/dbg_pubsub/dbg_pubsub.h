/******************************************************************************
 * dbg_pubsub.h
 *
 * Public API for the debug publish/subscribe system.
 *
 * This header defines the publisher (target-side) and subscriber (client-side)
 * interfaces. Include this in your application code; the wire-format details
 * are in dbg_protocol.h.
 *
 * Usage (target side):
 *   1. dbg_pub_create()
 *   2. dbg_pub_register_field()  — once per observable variable
 *   3. In main loop:
 *        app_logic();
 *        dbg_pub_send_all(pub);       // end of cycle
 *        dbg_pub_poll_config(pub);    // process config messages
 *   4. dbg_pub_destroy()
 *
 * Usage (client side):
 *   1. dbg_sub_create()
 *   2. dbg_sub_request_field_list()  — optional discovery
 *   3. dbg_sub_subscribe()
 *   4. In receive loop:
 *        dbg_sub_poll(sub, callback, ctx);
 *   5. dbg_sub_unsubscribe()
 *   6. dbg_sub_destroy()
 *
 * (c) 2025 — Internal use only.
 *****************************************************************************/

#ifndef DBG_PUBSUB_H
#define DBG_PUBSUB_H

#include "dbg_protocol.h"
#include "dbg_util.h"       /* dbg_get_time_us, logging macros, etc. */

#include <stdio.h>          /* snprintf (used in inline helpers below) */
#include <stddef.h>         /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════════════════════
   CONFIGURATION
   ══════════════════════════════════════════════════════════════════════════ */

/** Maximum number of fields that can be registered with a publisher. */
#define DBG_PUB_MAX_FIELDS          512u

/** Maximum simultaneous subscriptions a publisher can serve. */
#define DBG_PUB_MAX_SUBSCRIPTIONS   DBG_MAX_SUBSCRIPTIONS
#define DBG_SUB_ID_AUTO             UINT16_C(0xFFFF)

/** Maximum fields per subscription. */
#define DBG_PUB_MAX_SUB_FIELDS      DBG_MAX_SUB_FIELDS

/** Receive buffer size for config socket. */
#define DBG_PUB_CONFIG_BUF_SIZE     4096u

/** Transmit buffer size (largest possible outbound message). */
#define DBG_PUB_TX_BUF_SIZE         4096u

/** Heartbeat timeout in microseconds.  If no heartbeat is received from a
 *  subscriber within this window, the subscription is torn down. */
#define DBG_PUB_HEARTBEAT_TIMEOUT_US  5000000u  /* 5 seconds */

/* ══════════════════════════════════════════════════════════════════════════
   OPAQUE TYPES
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct dbg_publisher   dbg_publisher_t;
typedef struct dbg_subscriber  dbg_subscriber_t;

/* ══════════════════════════════════════════════════════════════════════════
   PUBLISHER — CALLBACK TYPES
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief   Write-validation callback.
 *
 * Called before a remote write is applied.  Return DBG_OK to accept,
 * or any dbg_status_t error to reject.
 *
 * @param   field_id    Target field.
 * @param   type        Value type being written.
 * @param   value       Proposed value.
 * @param   user_ctx    Context pointer set via dbg_pub_set_validate_cb().
 * @return  DBG_OK to accept; negative dbg_status_t to reject.
 */
typedef dbg_status_t (*dbg_validate_cb_t)(uint64_t          field_id,
                                          dbg_value_type_t  type,
                                          const dbg_value_t *value,
                                          void              *user_ctx);

/**
 * @brief   Post-write notification callback.
 *
 * Called after a remote write has been applied successfully.
 *
 * @param   field_id    Written field.
 * @param   type        Value type.
 * @param   value       New value.
 * @param   user_ctx    Context pointer.
 */
typedef void (*dbg_write_notify_cb_t)(uint64_t          field_id,
                                      dbg_value_type_t  type,
                                      const dbg_value_t *value,
                                      void              *user_ctx);

/**
 * @brief   Subscription event callback.
 *
 * Notifies the application when a subscription is created or destroyed.
 *
 * @param   sub_id      Subscription handle.
 * @param   active      1 = created, 0 = destroyed.
 * @param   user_ctx    Context pointer.
 */
typedef void (*dbg_sub_event_cb_t)(uint16_t sub_id,
                                   int      active,
                                   void     *user_ctx);

/* ══════════════════════════════════════════════════════════════════════════
   PUBLISHER — CONFIGURATION STRUCTURE
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief   Publisher creation parameters.
 *
 * Fill in desired values and pass to dbg_pub_create().
 * Fields left at zero will use defaults.
 */
typedef struct {
    uint16_t  data_port;            /**< UDP port for cyclic frames.
                                         Default: DBG_DATA_PORT_DEFAULT    */
    uint16_t  config_port;          /**< UDP port for config messages.
                                         Default: DBG_CONFIG_PORT_DEFAULT  */
    const char *bind_addr;          /**< IP to bind, e.g. "0.0.0.0".
                                         Default: "0.0.0.0"               */
    uint32_t  heartbeat_timeout_us; /**< Subscription timeout.
                                         Default: DBG_PUB_HEARTBEAT_TIMEOUT_US */
} dbg_pub_config_t;

/** Convenience initialiser: all defaults. */
#define DBG_PUB_CONFIG_DEFAULT { 0, 0, NULL, 0 }

/* ══════════════════════════════════════════════════════════════════════════
   PUBLISHER — FIELD REGISTRATION
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief   Field registration descriptor.
 *
 * Passed to dbg_pub_register_field() or the batch-register variant.
 */
typedef struct {
    uint64_t          field_id;     /**< Unique field identifier.          */
    dbg_value_type_t  type;         /**< Value type.                       */
    dbg_access_t      access;       /**< Read-only or read-write.          */
    volatile void    *ptr;          /**< Pointer to the live variable.     */
    const char       *name;         /**< Human-readable name (≤31 chars).
                                         May be NULL.                      */
} dbg_field_def_t;

/* ══════════════════════════════════════════════════════════════════════════
   PUBLISHER — STATISTICS
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t  frames_sent;          /**< Total frames sent (all subs).     */
    uint32_t  config_msgs_received; /**< Total config messages processed.  */
    uint32_t  writes_applied;       /**< Successful remote writes.         */
    uint32_t  writes_rejected;      /**< Rejected remote writes.           */
    uint32_t  active_subscriptions; /**< Currently active subscriptions.   */
    uint32_t  registered_fields;    /**< Fields in the registry.           */
    uint64_t  last_send_us;         /**< Duration of last send_all (µs).   */
} dbg_pub_stats_t;

/* ══════════════════════════════════════════════════════════════════════════
   PUBLISHER API
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief   Create a publisher instance.
 *
 * Opens two UDP sockets (data and config) and initialises internal state.
 *
 * @param   config  Configuration parameters. Pass NULL for all defaults.
 * @return  Opaque publisher handle, or NULL on failure.
 */
dbg_publisher_t* dbg_pub_create(const dbg_pub_config_t *config);

/**
 * @brief   Destroy a publisher and release all resources.
 * @param   pub  Publisher handle (may be NULL — no-op).
 */
void dbg_pub_destroy(dbg_publisher_t *pub);

/**
 * @brief   Register a single observable field.
 *
 * @param   pub   Publisher handle.
 * @param   def   Field definition.  The pointer must remain valid for the
 *                lifetime of the publisher.
 * @return  DBG_OK on success.
 */
dbg_status_t dbg_pub_register_field(dbg_publisher_t     *pub,
                                    const dbg_field_def_t *def);

/**
 * @brief   Register multiple fields in one call.
 *
 * @param   pub    Publisher handle.
 * @param   defs   Array of field definitions.
 * @param   count  Number of entries.
 * @return  DBG_OK if all succeeded.  On partial failure, fields up to the
 *          failing index are registered and the error code is returned.
 */
dbg_status_t dbg_pub_register_fields(dbg_publisher_t       *pub,
                                     const dbg_field_def_t *defs,
                                     uint16_t               count);

/**
 * @brief   Unregister a field.
 *
 * Active subscriptions referencing this field will have the field removed
 * on the next cycle.
 *
 * @param   pub       Publisher handle.
 * @param   field_id  Field to remove.
 * @return  DBG_OK or DBG_ERR_UNKNOWN_FIELD.
 */
dbg_status_t dbg_pub_unregister_field(dbg_publisher_t *pub,
                                      uint64_t         field_id);

/**
 * @brief   Unregister a field.
 *
 * Active subscriptions will have all fields removed on the next cycle.
 *
 * @param   pub       Publisher handle.
 * @return  DBG_OK.
 */
dbg_status_t dbg_pub_unregister_all_fields(dbg_publisher_t *pub);

/**
 * @brief   Build and send data frames for all active subscriptions.
 *
 * Call this once at the end of every application cycle, after all
 * variables have been updated.
 *
 * @param   pub  Publisher handle.
 * @return  Number of frames sent (≥ 0), or negative dbg_status_t on error.
 */
int dbg_pub_send_all(dbg_publisher_t *pub);

/**
 * @brief   Poll the config socket for incoming messages.
 *
 * Processes subscribe, unsubscribe, write, field-list, and heartbeat
 * messages.  Non-blocking — returns immediately if no data is available.
 *
 * Call this once per cycle, after dbg_pub_send_all().
 *
 * @param   pub  Publisher handle.
 * @return  Number of messages processed (≥ 0), or negative on error.
 */
int dbg_pub_poll_config(dbg_publisher_t *pub);

/**
 * @brief   Set the write-validation callback.
 *
 * @param   pub       Publisher handle.
 * @param   cb        Callback function (NULL to disable).
 * @param   user_ctx  Opaque context forwarded to the callback.
 */
void dbg_pub_set_validate_cb(dbg_publisher_t  *pub,
                              dbg_validate_cb_t cb,
                              void             *user_ctx);

/**
 * @brief   Set the post-write notification callback.
 *
 * @param   pub       Publisher handle.
 * @param   cb        Callback function (NULL to disable).
 * @param   user_ctx  Opaque context forwarded to the callback.
 */
void dbg_pub_set_write_notify_cb(dbg_publisher_t      *pub,
                                  dbg_write_notify_cb_t cb,
                                  void                 *user_ctx);

/**
 * @brief   Set the subscription-event callback.
 *
 * @param   pub       Publisher handle.
 * @param   cb        Callback function (NULL to disable).
 * @param   user_ctx  Opaque context forwarded to the callback.
 */
void dbg_pub_set_sub_event_cb(dbg_publisher_t    *pub,
                               dbg_sub_event_cb_t  cb,
                               void               *user_ctx);

/**
 * @brief   Retrieve publisher statistics.
 *
 * @param   pub    Publisher handle.
 * @param   stats  Output structure.
 */
void dbg_pub_get_stats(const dbg_publisher_t *pub,
                       dbg_pub_stats_t       *stats);

/**
 * @brief   Force-close a subscription.
 *
 * @param   pub     Publisher handle.
 * @param   sub_id  Subscription to close.
 * @return  DBG_OK or DBG_ERR_UNKNOWN_SUB.
 */
dbg_status_t dbg_pub_close_subscription(dbg_publisher_t *pub,
                                        uint16_t         sub_id);

/**
 * @brief   Close all active subscriptions.
 * @param   pub  Publisher handle.
 */
void dbg_pub_close_all_subscriptions(dbg_publisher_t *pub);

/* ══════════════════════════════════════════════════════════════════════════
   PUBLISHER — CONVENIENCE MACROS
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief   Register a read-only field with minimal boilerplate.
 *
 * @param   pub_     Publisher handle.
 * @param   id_      Field ID (uint64_t).
 * @param   var_     Variable (lvalue).
 * @param   vt_      dbg_value_type_t.
 */
#define DBG_PUB_FIELD_RO(pub_, id_, var_, vt_)                            \
    do {                                                                   \
        dbg_field_def_t _def_ = {                                          \
            .field_id = (id_),                                             \
            .type     = (vt_),                                             \
            .access   = DBG_ACCESS_READ_ONLY,                              \
            .ptr      = &(var_),                                           \
            .name     = #var_                                              \
        };                                                                 \
        dbg_pub_register_field((pub_), &_def_);                            \
    } while (0)

/**
 * @brief   Register a read-write field with minimal boilerplate.
 */
#define DBG_PUB_FIELD_RW(pub_, id_, var_, vt_)                            \
    do {                                                                   \
        dbg_field_def_t _def_ = {                                          \
            .field_id = (id_),                                             \
            .type     = (vt_),                                             \
            .access   = DBG_ACCESS_READ_WRITE,                             \
            .ptr      = &(var_),                                           \
            .name     = #var_                                              \
        };                                                                 \
        dbg_pub_register_field((pub_), &_def_);                            \
    } while (0)

/* ══════════════════════════════════════════════════════════════════════════
   SUBSCRIBER — CALLBACK TYPES
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief   Frame-received callback.
 *
 * @param   sub_id         Subscription handle.
 * @param   sequence       Monotonic sequence number.
 * @param   timestamp_us   Source timestamp (µs).
 * @param   payload        Raw packed field values.
 * @param   payload_size   Size of payload in bytes.
 * @param   user_ctx       Context pointer.
 */
typedef void (*dbg_frame_cb_t)(uint16_t       sub_id,
                                uint32_t       sequence,
                                uint64_t       timestamp_us,
                                const uint8_t *payload,
                                uint16_t       payload_size,
                                void          *user_ctx);

/**
 * @brief   Field-list-received callback.
 *
 * @param   descriptors   Array of field descriptors.
 * @param   count         Number of descriptors in this batch.
 * @param   total         Total fields available on the server.
 * @param   user_ctx      Context pointer.
 */
typedef void (*dbg_field_list_cb_t)(const dbg_field_descriptor_t *descriptors,
                                     uint16_t                      count,
                                     uint16_t                      total,
                                     void                         *user_ctx);

/**
 * @brief   Disconnect / error callback.
 *
 * @param   reason     Status code describing the disconnect.
 * @param   user_ctx   Context pointer.
 */
typedef void (*dbg_disconnect_cb_t)(dbg_status_t reason,
                                     void        *user_ctx);

/* ══════════════════════════════════════════════════════════════════════════
   SUBSCRIBER — CONFIGURATION
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *host;               /**< Server IP or hostname.            */
    uint16_t    data_port;          /**< Server data port.
                                         Default: DBG_DATA_PORT_DEFAULT    */
    uint16_t    config_port;        /**< Server config port.
                                         Default: DBG_CONFIG_PORT_DEFAULT  */
    uint32_t    config_timeout_ms;  /**< Timeout for config replies (ms).
                                         Default: 1000                     */
    uint32_t    heartbeat_interval_ms; /**< Heartbeat send interval.
                                            Default: 1000                  */
} dbg_sub_config_t;

#define DBG_SUB_CONFIG_DEFAULT { NULL, 0, 0, 0, 0 }

/* ══════════════════════════════════════════════════════════════════════════
   SUBSCRIBER — SUBSCRIPTION LAYOUT
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief   Describes the layout of a single field within a received frame.
 *
 * Populated after a successful subscribe call by copying from the ack.
 * Clients use this to unpack frame payloads.
 */
typedef struct {
    uint64_t          field_id;
    dbg_value_type_t  type;
    uint8_t           size;         /**< Bytes in the frame payload.       */
    uint16_t          offset;       /**< Byte offset into payload.         */
} dbg_sub_field_layout_t;

/**
 * @brief   Full subscription layout returned after a successful subscribe.
 */
typedef struct {
    uint16_t                sub_id;
    uint16_t                field_count;
    uint16_t                frame_size;     /**< Total payload bytes.      */
    uint32_t                actual_interval_us;
    dbg_sub_field_layout_t  fields[DBG_MAX_SUB_FIELDS];
} dbg_sub_layout_t;

/* ══════════════════════════════════════════════════════════════════════════
   SUBSCRIBER — STATISTICS
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t  frames_received;
    uint32_t  frames_dropped;       /**< Detected via sequence gaps.       */
    uint32_t  bytes_received;
    uint64_t  last_frame_timestamp_us;
    uint32_t  last_sequence;
    float     rtt_ms;               /**< Last heartbeat round-trip (ms).   */
} dbg_sub_stats_t;

/* ══════════════════════════════════════════════════════════════════════════
   SUBSCRIBER API
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief   Create a subscriber instance.
 *
 * @param   config  Configuration. The 'host' field is required.
 * @return  Opaque subscriber handle, or NULL on failure.
 */
dbg_subscriber_t* dbg_sub_create(const dbg_sub_config_t *config);

/**
 * @brief   Destroy a subscriber and release all resources.
 * @param   sub  Subscriber handle (may be NULL — no-op).
 */
void dbg_sub_destroy(dbg_subscriber_t *sub);

/**
 * @brief   Subscribe to a set of fields.
 *
 * Sends a subscribe request and waits (blocking) for the ack.
 *
 * @param   sub          Subscriber handle.
 * @param   sub_id       Client-chosen subscription handle.
 * @param   field_ids    Array of field IDs to subscribe to.
 * @param   types        Array of expected value types.
 * @param   count        Number of fields.
 * @param   interval_us  Desired publish interval (µs).
 * @param   out_layout   (Optional) output layout descriptor.
 * @return  DBG_OK on success, negative on error.
 */
dbg_status_t dbg_sub_subscribe(dbg_subscriber_t        *sub,
                               uint16_t                 sub_id,
                               const uint64_t          *field_ids,
                               const dbg_value_type_t  *types,
                               uint16_t                 count,
                               uint32_t                 interval_us,
                               dbg_sub_layout_t        *out_layout);

/**
 * @brief   Unsubscribe.
 *
 * @param   sub     Subscriber handle.
 * @param   sub_id  Subscription to cancel.
 * @return  DBG_OK on success.
 */
dbg_status_t dbg_sub_unsubscribe(dbg_subscriber_t *sub,
                                 uint16_t          sub_id);

/**
 * @brief   Poll for incoming data frames.
 *
 * Non-blocking.  Invokes the frame callback for each received frame.
 *
 * @param   sub       Subscriber handle.
 * @param   cb        Frame callback.
 * @param   user_ctx  Context pointer forwarded to callback.
 * @return  Number of frames received (≥ 0), or negative on error.
 */
int dbg_sub_poll(dbg_subscriber_t *sub,
                 dbg_frame_cb_t    cb,
                 void             *user_ctx);
/**
 * @brief   Iterator for walking all values in a frame payload.
 *
 * Usage:
 *   dbg_frame_iter_t it;
 *   dbg_frame_iter_init(&it, payload, &layout);
 *   dbg_value_t val;
 *   while (dbg_frame_iter_next(&it, &val) == DBG_OK) {
 *       // it.current_index, it.current_field_id, it.current_type, val
 *   }
 */
typedef struct {
    const uint8_t          *payload;
    const dbg_sub_layout_t *layout;
    uint16_t                current_index;
    uint64_t                current_field_id;
    dbg_value_type_t        current_type;
} dbg_frame_iter_t;

/**
 * @brief   Holds a single received frame's metadata and a stable copy
 *          of its payload, plus a ready-initialised field iterator.
 *
 * The caller owns this struct and must ensure it outlives any use of
 * the embedded iterator.
 */
typedef struct {
    uint16_t          sub_id;
    uint32_t          sequence;
    uint64_t          timestamp_us;
    uint16_t          payload_size;
    uint8_t           payload[DBG_MAX_FRAME_PAYLOAD]; /* stable copy */
    dbg_frame_iter_t  iter;                           /* points into payload[] */
} dbg_frame_result_t;

/**
 * @brief   Variant of dbg_sub_poll that returns an iterator over the
 *          first valid frame received, rather than invoking a callback.
 *
 * Receives up to max_per_poll frames from the data socket, applies the
 * same validation and sequence-tracking logic as dbg_sub_poll, and on
 * the first accepted frame:
 *   - copies the payload into @p result->payload (stable storage),
 *   - initialises @p result->iter against that copy and @p layout,
 *   - populates all metadata fields.
 *
 * Remaining frames in the socket buffer are intentionally left for the
 * next call so that the caller can process one frame at a time.
 *
 * @param   sub     Subscriber handle.
 * @param   layout  Layout describing field offsets/types; must remain
 *                  valid for the lifetime of the returned iterator.
 * @param   result  Caller-allocated output struct.
 *
 * @return  1  if a frame was received and @p result is populated.
 *          0  if no frame was available.
 *         <0  on error (dbg_status_t cast to int).
 */
int dbg_sub_poll_iter(dbg_subscriber_t  *sub,
                      dbg_sub_layout_t  *layout,
                      dbg_frame_result_t *result);

/**
 * @brief   Send a write request to the server.
 *
 * Blocking — waits for the write ack.
 *
 * @param   sub        Subscriber handle.
 * @param   field_id   Target field.
 * @param   type       Value type.
 * @param   value      Value to write.
 * @return  DBG_OK on success, negative on error.
 */
dbg_status_t dbg_sub_write(dbg_subscriber_t *sub,
                           uint64_t          field_id,
                           dbg_value_type_t  type,
                           const dbg_value_t *value);

/**
 * @brief   Send a multi-field write request.
 *
 * Blocking — waits for the write ack.
 *
 * @param   sub         Subscriber handle.
 * @param   items       Array of write items.
 * @param   count       Number of items.
 * @param   out_status  (Optional) per-item status output array.
 * @return  DBG_OK if all writes succeeded, DBG_ERR_INTERNAL on
 *          partial failure (check out_status for details).
 */
dbg_status_t dbg_sub_write_multi(dbg_subscriber_t       *sub,
                                 const dbg_write_item_t *items,
                                 uint16_t                count,
                                 dbg_status_t           *out_status);

/**
 * @brief   Request the server's field list.
 *
 * Blocking — waits for the response.  For large registries the callback
 * may be invoked multiple times (paginated).
 *
 * @param   sub       Subscriber handle.
 * @param   cb        Callback invoked with each page of field descriptors.
 * @param   user_ctx  Context pointer.
 * @return  DBG_OK on success.
 */
dbg_status_t dbg_sub_request_field_list(dbg_subscriber_t   *sub,
                                        dbg_field_list_cb_t cb,
                                        void               *user_ctx);

/**
 * @brief   Send a heartbeat to the server.
 *
 * Normally called automatically by dbg_sub_poll() at the configured
 * interval.  Call manually if needed.
 *
 * @param   sub  Subscriber handle.
 * @return  DBG_OK on success.
 */
dbg_status_t dbg_sub_send_heartbeat(dbg_subscriber_t *sub);

/**
 * @brief   Set the disconnect / error callback.
 *
 * @param   sub       Subscriber handle.
 * @param   cb        Callback (NULL to disable).
 * @param   user_ctx  Context pointer.
 */
void dbg_sub_set_disconnect_cb(dbg_subscriber_t   *sub,
                                dbg_disconnect_cb_t cb,
                                void               *user_ctx);

/**
 * @brief   Retrieve subscriber statistics for a subscription.
 *
 * @param   sub     Subscriber handle.
 * @param   sub_id  Subscription to query.
 * @param   stats   Output statistics structure.
 * @return  DBG_OK or DBG_ERR_UNKNOWN_SUB.
 */
dbg_status_t dbg_sub_get_stats(const dbg_subscriber_t *sub,
                               uint16_t                sub_id,
                               dbg_sub_stats_t        *stats);

/* ══════════════════════════════════════════════════════════════════════════
   SUBSCRIBER — FRAME UNPACKING HELPERS
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief   Extract a typed value from a frame payload.
 *
 * @param   payload   Raw payload bytes.
 * @param   layout    Field layout descriptor (from subscribe ack).
 * @param   out       Output value union.
 * @return  DBG_OK on success.
 */
static inline dbg_status_t dbg_unpack_value(
    const uint8_t                *payload,
    const dbg_sub_field_layout_t *layout,
    dbg_value_t                  *out)
{
    if (!payload || !layout || !out) return DBG_ERR_INTERNAL;

    memset(out, 0, sizeof(*out));
    memcpy(out, payload + layout->offset, layout->size);
    return DBG_OK;
}

/**
 * @brief   Extract a float from a frame payload by field index.
 */
static inline dbg_status_t dbg_unpack_f32(
    const uint8_t          *payload,
    const dbg_sub_layout_t *sub_layout,
    uint16_t                field_index,
    float                  *out)
{
    if (field_index >= sub_layout->field_count) return DBG_ERR_OUT_OF_RANGE;

    const dbg_sub_field_layout_t *fl = &sub_layout->fields[field_index];
    if (fl->type != DBG_VT_F32) return DBG_ERR_TYPE_MISMATCH;

    memcpy(out, payload + fl->offset, sizeof(float));
    return DBG_OK;
}

/**
 * @brief   Extract a double from a frame payload by field index.
 */
static inline dbg_status_t dbg_unpack_f64(
    const uint8_t          *payload,
    const dbg_sub_layout_t *sub_layout,
    uint16_t                field_index,
    double                 *out)
{
    if (field_index >= sub_layout->field_count) return DBG_ERR_OUT_OF_RANGE;

    const dbg_sub_field_layout_t *fl = &sub_layout->fields[field_index];
    if (fl->type != DBG_VT_F64) return DBG_ERR_TYPE_MISMATCH;

    memcpy(out, payload + fl->offset, sizeof(double));
    return DBG_OK;
}

/**
 * @brief   Extract a uint32 from a frame payload by field index.
 */
static inline dbg_status_t dbg_unpack_u32(
    const uint8_t          *payload,
    const dbg_sub_layout_t *sub_layout,
    uint16_t                field_index,
    uint32_t               *out)
{
    if (field_index >= sub_layout->field_count) return DBG_ERR_OUT_OF_RANGE;

    const dbg_sub_field_layout_t *fl = &sub_layout->fields[field_index];
    if (fl->type != DBG_VT_U32) return DBG_ERR_TYPE_MISMATCH;

    memcpy(out, payload + fl->offset, sizeof(uint32_t));
    return DBG_OK;
}

/**
 * @brief   Extract an int32 from a frame payload by field index.
 */
static inline dbg_status_t dbg_unpack_i32(
    const uint8_t          *payload,
    const dbg_sub_layout_t *sub_layout,
    uint16_t                field_index,
    int32_t                *out)
{
    if (field_index >= sub_layout->field_count) return DBG_ERR_OUT_OF_RANGE;

    const dbg_sub_field_layout_t *fl = &sub_layout->fields[field_index];
    if (fl->type != DBG_VT_I32) return DBG_ERR_TYPE_MISMATCH;

    memcpy(out, payload + fl->offset, sizeof(int32_t));
    return DBG_OK;
}

/**
 * @brief   Extract a uint64 from a frame payload by field index.
 */
static inline dbg_status_t dbg_unpack_u64(
    const uint8_t          *payload,
    const dbg_sub_layout_t *sub_layout,
    uint16_t                field_index,
    uint64_t               *out)
{
    if (field_index >= sub_layout->field_count) return DBG_ERR_OUT_OF_RANGE;

    const dbg_sub_field_layout_t *fl = &sub_layout->fields[field_index];
    if (fl->type != DBG_VT_U64) return DBG_ERR_TYPE_MISMATCH;

    memcpy(out, payload + fl->offset, sizeof(uint64_t));
    return DBG_OK;
}

/**
 * @brief   Extract an int64 from a frame payload by field index.
 */
static inline dbg_status_t dbg_unpack_i64(
    const uint8_t          *payload,
    const dbg_sub_layout_t *sub_layout,
    uint16_t                field_index,
    int64_t                *out)
{
    if (field_index >= sub_layout->field_count) return DBG_ERR_OUT_OF_RANGE;

    const dbg_sub_field_layout_t *fl = &sub_layout->fields[field_index];
    if (fl->type != DBG_VT_I64) return DBG_ERR_TYPE_MISMATCH;

    memcpy(out, payload + fl->offset, sizeof(int64_t));
    return DBG_OK;
}

/**
 * @brief   Extract a uint16 from a frame payload by field index.
 */
static inline dbg_status_t dbg_unpack_u16(
    const uint8_t          *payload,
    const dbg_sub_layout_t *sub_layout,
    uint16_t                field_index,
    uint16_t               *out)
{
    if (field_index >= sub_layout->field_count) return DBG_ERR_OUT_OF_RANGE;

    const dbg_sub_field_layout_t *fl = &sub_layout->fields[field_index];
    if (fl->type != DBG_VT_U16) return DBG_ERR_TYPE_MISMATCH;

    memcpy(out, payload + fl->offset, sizeof(uint16_t));
    return DBG_OK;
}

/**
 * @brief   Extract an int16 from a frame payload by field index.
 */
static inline dbg_status_t dbg_unpack_i16(
    const uint8_t          *payload,
    const dbg_sub_layout_t *sub_layout,
    uint16_t                field_index,
    int16_t                *out)
{
    if (field_index >= sub_layout->field_count) return DBG_ERR_OUT_OF_RANGE;

    const dbg_sub_field_layout_t *fl = &sub_layout->fields[field_index];
    if (fl->type != DBG_VT_I16) return DBG_ERR_TYPE_MISMATCH;

    memcpy(out, payload + fl->offset, sizeof(int16_t));
    return DBG_OK;
}

/**
 * @brief   Extract a uint8 from a frame payload by field index.
 */
static inline dbg_status_t dbg_unpack_u8(
    const uint8_t          *payload,
    const dbg_sub_layout_t *sub_layout,
    uint16_t                field_index,
    uint8_t                *out)
{
    if (field_index >= sub_layout->field_count) return DBG_ERR_OUT_OF_RANGE;

    const dbg_sub_field_layout_t *fl = &sub_layout->fields[field_index];
    if (fl->type != DBG_VT_U8) return DBG_ERR_TYPE_MISMATCH;

    memcpy(out, payload + fl->offset, sizeof(uint8_t));
    return DBG_OK;
}

/**
 * @brief   Extract an int8 from a frame payload by field index.
 */
static inline dbg_status_t dbg_unpack_i8(
    const uint8_t          *payload,
    const dbg_sub_layout_t *sub_layout,
    uint16_t                field_index,
    int8_t                 *out)
{
    if (field_index >= sub_layout->field_count) return DBG_ERR_OUT_OF_RANGE;

    const dbg_sub_field_layout_t *fl = &sub_layout->fields[field_index];
    if (fl->type != DBG_VT_I8) return DBG_ERR_TYPE_MISMATCH;

    memcpy(out, payload + fl->offset, sizeof(int8_t));
    return DBG_OK;
}

/**
 * @brief   Extract a bool from a frame payload by field index.
 */
static inline dbg_status_t dbg_unpack_bool(
    const uint8_t          *payload,
    const dbg_sub_layout_t *sub_layout,
    uint16_t                field_index,
    uint8_t                *out)
{
    if (field_index >= sub_layout->field_count) return DBG_ERR_OUT_OF_RANGE;

    const dbg_sub_field_layout_t *fl = &sub_layout->fields[field_index];
    if (fl->type != DBG_VT_BOOL) return DBG_ERR_TYPE_MISMATCH;

    memcpy(out, payload + fl->offset, sizeof(uint8_t));
    return DBG_OK;
}

/**
 * @brief   Generic unpack: extract any value by field index into a
 *          dbg_value_t union.
 *
 * @param   payload       Raw payload bytes.
 * @param   sub_layout    Full subscription layout.
 * @param   field_index   Index of the field (0-based).
 * @param   out           Output value union.
 * @return  DBG_OK on success.
 */
static inline dbg_status_t dbg_unpack_by_index(
    const uint8_t          *payload,
    const dbg_sub_layout_t *sub_layout,
    uint16_t                field_index,
    dbg_value_t            *out)
{
    if (!payload || !sub_layout || !out) return DBG_ERR_INTERNAL;
    if (field_index >= sub_layout->field_count) return DBG_ERR_OUT_OF_RANGE;

    const dbg_sub_field_layout_t *fl = &sub_layout->fields[field_index];
    memset(out, 0, sizeof(*out));
    memcpy(out, payload + fl->offset, fl->size);
    return DBG_OK;
}

/**
 * @brief   Find a field index within a subscription layout by field_id.
 *
 * Linear search — intended for setup / non-critical path.
 *
 * @param   sub_layout   Subscription layout.
 * @param   field_id     Field to search for.
 * @return  Field index (0-based), or -1 if not found.
 */
static inline int dbg_layout_find_field(
    const dbg_sub_layout_t *sub_layout,
    uint64_t                field_id)
{
    if (!sub_layout) return -1;
    for (uint16_t i = 0; i < sub_layout->field_count; i++) {
        if (sub_layout->fields[i].field_id == field_id) {
            return (int)i;
        }
    }
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
   SUBSCRIBER — FRAME ITERATOR
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief   Initialise a frame iterator.
 */
static inline void dbg_frame_iter_init(
    dbg_frame_iter_t       *it,
    const uint8_t          *payload,
    const dbg_sub_layout_t *layout)
{
    it->payload          = payload;
    it->layout           = layout;
    it->current_index    = 0;
    it->current_field_id = 0;
    it->current_type     = (dbg_value_type_t)0;
}

/**
 * @brief   Advance the iterator and extract the next value.
 *
 * @param   it    Iterator state.
 * @param   out   Output value.
 * @return  DBG_OK if a value was extracted.
 *          DBG_ERR_OUT_OF_RANGE when iteration is complete.
 */
static inline dbg_status_t dbg_frame_iter_next(
    dbg_frame_iter_t *it,
    dbg_value_t      *out)
{
    if (it->current_index >= it->layout->field_count) {
        return DBG_ERR_OUT_OF_RANGE;
    }

    const dbg_sub_field_layout_t *fl =
        &it->layout->fields[it->current_index];

    it->current_field_id = fl->field_id;
    it->current_type     = fl->type;

    memset(out, 0, sizeof(*out));
    memcpy(out, it->payload + fl->offset, fl->size);

    it->current_index++;
    return DBG_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
   VALUE FORMATTING HELPER
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief   Format a dbg_value_t as a string for logging / display.
 *
 * @param   type    Value type.
 * @param   val     Value to format.
 * @param   buf     Output buffer.
 * @param   buf_sz  Buffer size.
 * @return  Number of characters written (excluding null terminator),
 *          or -1 on error.
 */
static inline int dbg_value_snprintf(
    dbg_value_type_t   type,
    const dbg_value_t *val,
    char              *buf,
    size_t             buf_sz)
{
    if (!val || !buf || buf_sz == 0) return -1;

    int n = 0;
    switch (type) {
    case DBG_VT_BOOL:
        n = snprintf(buf, buf_sz, "%s", val->b ? "true" : "false");
        break;
    case DBG_VT_U8:
        n = snprintf(buf, buf_sz, "%u", (unsigned)val->u8);
        break;
    case DBG_VT_I8:
        n = snprintf(buf, buf_sz, "%d", (int)val->i8);
        break;
    case DBG_VT_U16:
        n = snprintf(buf, buf_sz, "%u", (unsigned)val->u16);
        break;
    case DBG_VT_I16:
        n = snprintf(buf, buf_sz, "%d", (int)val->i16);
        break;
    case DBG_VT_U32:
        n = snprintf(buf, buf_sz, "%" PRIu32, val->u32);
        break;
    case DBG_VT_I32:
        n = snprintf(buf, buf_sz, "%" PRId32, val->i32);
        break;
    case DBG_VT_U64:
        n = snprintf(buf, buf_sz, "%" PRIu64, val->u64);
        break;
    case DBG_VT_I64:
        n = snprintf(buf, buf_sz, "%" PRId64, val->i64);
        break;
    case DBG_VT_F32:
        n = snprintf(buf, buf_sz, "%.6g", (double)val->f32);
        break;
    case DBG_VT_F64:
        n = snprintf(buf, buf_sz, "%.10g", val->f64);
        break;
    default:
        n = snprintf(buf, buf_sz, "?");
        break;
    }
    return n;
}

#ifdef __cplusplus
}
#endif

#endif /* DBG_PUBSUB_H */
