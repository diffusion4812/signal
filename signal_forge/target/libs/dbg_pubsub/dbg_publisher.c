/******************************************************************************
 * dbg_publisher.c
 *
 * Publisher (target-side) implementation for the debug pub/sub system.
 *
 * All socket I/O is routed through dbg_socket.h, which provides a
 * platform-independent abstraction over POSIX and Winsock2.  This file
 * contains no platform-specific socket code of its own.
 *
 * (c) 2025 — Internal use only.
 *****************************************************************************/

#include "dbg_pubsub.h"
#include "dbg_protocol.h"
#include "dbg_socket.h"
#include "dbg_util.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* ══════════════════════════════════════════════════════════════════════════
   INTERNAL TYPES
   ══════════════════════════════════════════════════════════════════════════ */

/** Registry entry for a single observable field. */
typedef struct {
    uint64_t          field_id;
    dbg_value_type_t  type;
    dbg_access_t      access;
    uint8_t           size;           /* dbg_value_type_size(type) */
    volatile void    *ptr;
    char              name[DBG_FIELD_NAME_MAX];
    int               active;         /* 1 = registered, 0 = slot free */
} pub_field_entry_t;

/** Per-field info within a subscription. */
typedef struct {
    volatile void *ptr;               /* pointer to app variable */
    uint16_t       offset;            /* byte offset in frame payload */
    uint8_t        size;              /* bytes to copy */
    uint8_t        value_type;
    uint64_t       field_id;
} sub_field_binding_t;

/** A single active subscription. */
typedef struct {
    int                   active;
    uint16_t              sub_id;
    uint16_t              field_count;
    uint16_t              frame_size;       /* total payload bytes */
    uint32_t              interval_us;
    uint32_t              sequence;
    uint64_t              last_heartbeat_us;
    dbg_addr_t            client_config_addr;
    dbg_addr_t            client_data_addr;
    sub_field_binding_t   fields[DBG_PUB_MAX_SUB_FIELDS];

    /* Pre-built frame buffer: header + max payload.
       Avoids allocation on the hot path. */
    uint8_t               frame_buf[sizeof(dbg_frame_t) + DBG_MAX_FRAME_PAYLOAD];
} pub_subscription_t;

/** Main publisher structure. */
struct dbg_publisher {
    /* Sockets */
    dbg_socket_t            data_sock;
    dbg_socket_t            config_sock;
    uint16_t                data_port;
    uint16_t                config_port;

    /* Field registry */
    pub_field_entry_t       fields[DBG_PUB_MAX_FIELDS];
    uint32_t                field_count;

    /* Active subscriptions */
    pub_subscription_t      subs[DBG_PUB_MAX_SUBSCRIPTIONS];

    /* Callbacks */
    dbg_validate_cb_t       validate_cb;
    void                   *validate_ctx;
    dbg_write_notify_cb_t   write_notify_cb;
    void                   *write_notify_ctx;
    dbg_sub_event_cb_t      sub_event_cb;
    void                   *sub_event_ctx;

    /* Configuration */
    uint32_t                heartbeat_timeout_us;

    /* Statistics */
    dbg_pub_stats_t         stats;

    /* Transaction counter for server-initiated messages */
    uint32_t                next_tx_id;

    /* Config receive buffer */
    uint8_t                 rx_buf[DBG_PUB_CONFIG_BUF_SIZE];

    /* Transmit buffer for config replies */
    uint8_t                 tx_buf[DBG_PUB_TX_BUF_SIZE];
};

/* ══════════════════════════════════════════════════════════════════════════
   FORWARD DECLARATIONS (INTERNAL)
   ══════════════════════════════════════════════════════════════════════════ */

static pub_field_entry_t*  pub_find_field(dbg_publisher_t *pub, uint64_t field_id);
static pub_subscription_t* pub_find_sub(dbg_publisher_t *pub, uint16_t sub_id);
static pub_subscription_t* pub_alloc_sub(dbg_publisher_t *pub);

static void pub_handle_subscribe(dbg_publisher_t *pub,
                                 const uint8_t *data, uint32_t len,
                                 const dbg_addr_t *from);
static void pub_handle_unsubscribe(dbg_publisher_t *pub,
                                   const uint8_t *data, uint32_t len,
                                   const dbg_addr_t *from);
static void pub_handle_write(dbg_publisher_t *pub,
                             const uint8_t *data, uint32_t len,
                             const dbg_addr_t *from);
static void pub_handle_field_list(dbg_publisher_t *pub,
                                  const uint8_t *data, uint32_t len,
                                  const dbg_addr_t *from);
static void pub_handle_heartbeat(dbg_publisher_t *pub,
                                 const uint8_t *data, uint32_t len,
                                 const dbg_addr_t *from);

static void pub_send_config(dbg_publisher_t *pub,
                            const void *data, uint32_t len,
                            const dbg_addr_t *to);
static void pub_check_heartbeat_timeouts(dbg_publisher_t *pub);
static void pub_teardown_sub(dbg_publisher_t *pub, pub_subscription_t *sub);

/* ══════════════════════════════════════════════════════════════════════════
   LIFECYCLE
   ══════════════════════════════════════════════════════════════════════════ */

dbg_publisher_t* dbg_pub_create(const dbg_pub_config_t *config)
{
    dbg_publisher_t *pub = (dbg_publisher_t *)calloc(1, sizeof(*pub));
    if (!pub) return NULL;

    /* Apply defaults */
    const char *bind_addr = "0.0.0.0";
    pub->data_port            = DBG_DATA_PORT_DEFAULT;
    pub->config_port          = DBG_CONFIG_PORT_DEFAULT;
    pub->heartbeat_timeout_us = DBG_PUB_HEARTBEAT_TIMEOUT_US;

    if (config) {
        if (config->data_port)            pub->data_port            = config->data_port;
        if (config->config_port)          pub->config_port          = config->config_port;
        if (config->bind_addr)            bind_addr                 = config->bind_addr;
        if (config->heartbeat_timeout_us) pub->heartbeat_timeout_us = config->heartbeat_timeout_us;
    }

    /* ── Data socket (send-only; bound for a consistent source port) ─────── */
    if (dbg_socket_open(&pub->data_sock) != DBG_OK) {
        DBG_ERROR("[pub] failed to open data socket");
        free(pub);
        return NULL;
    }
    dbg_socket_set_send_bufsize(&pub->data_sock, 262144);  /* 256 KB burst buffer */
    if (dbg_socket_bind(&pub->data_sock, bind_addr, pub->data_port) != DBG_OK) {
        DBG_ERROR("[pub] failed to bind data socket to port %u", pub->data_port);
        dbg_socket_close(&pub->data_sock);
        free(pub);
        return NULL;
    }

    /* ── Config socket (receives requests, sends replies) ────────────────── */
    if (dbg_socket_open(&pub->config_sock) != DBG_OK) {
        DBG_ERROR("[pub] failed to open config socket");
        dbg_socket_close(&pub->data_sock);
        free(pub);
        return NULL;
    }
    dbg_socket_set_recv_bufsize(&pub->config_sock, 262144);
    if (dbg_socket_bind(&pub->config_sock, bind_addr, pub->config_port) != DBG_OK) {
        DBG_ERROR("[pub] failed to bind config socket to port %u", pub->config_port);
        dbg_socket_close(&pub->data_sock);
        dbg_socket_close(&pub->config_sock);
        free(pub);
        return NULL;
    }

    /* Config socket must be non-blocking so poll_config() never stalls */
    if (dbg_socket_set_nonblocking(&pub->config_sock, 1) != DBG_OK) {
        DBG_ERROR("[pub] failed to set config socket non-blocking");
        dbg_socket_close(&pub->data_sock);
        dbg_socket_close(&pub->config_sock);
        free(pub);
        return NULL;
    }

    pub->next_tx_id = 1;
    DBG_INFO("[pub] created on data_port=%u config_port=%u", pub->data_port, pub->config_port);
    return pub;
}

void dbg_pub_destroy(dbg_publisher_t *pub)
{
    if (!pub) return;

    dbg_pub_close_all_subscriptions(pub);

    dbg_socket_close(&pub->data_sock);
    dbg_socket_close(&pub->config_sock);

    free(pub);
}

/* ══════════════════════════════════════════════════════════════════════════
   FIELD REGISTRATION
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_pub_register_field(dbg_publisher_t       *pub,
                                    const dbg_field_def_t *def)
{
    if (!pub || !def || !def->ptr) return DBG_ERR_INTERNAL;

    uint8_t sz = dbg_value_type_size(def->type);
    if (sz == 0) return DBG_ERR_TYPE_MISMATCH;

    /* Reject duplicates */
    if (pub_find_field(pub, def->field_id)) {
        return DBG_ERR_INTERNAL;
    }

    /* Find a free slot */
    pub_field_entry_t *slot = NULL;
    for (uint32_t i = 0; i < DBG_PUB_MAX_FIELDS; i++) {
        if (!pub->fields[i].active) {
            slot = &pub->fields[i];
            break;
        }
    }
    if (!slot) return DBG_ERR_TOO_MANY_FIELDS;

    slot->field_id = def->field_id;
    slot->type     = def->type;
    slot->access   = def->access;
    slot->size     = sz;
    slot->ptr      = def->ptr;
    slot->active   = 1;

    memset(slot->name, 0, sizeof(slot->name));
    if (def->name) {
        strncpy(slot->name, def->name, DBG_FIELD_NAME_MAX - 1);
    }

    pub->field_count++;
    pub->stats.registered_fields = pub->field_count;
    return DBG_OK;
}

dbg_status_t dbg_pub_register_fields(dbg_publisher_t       *pub,
                                     const dbg_field_def_t *defs,
                                     uint16_t               count)
{
    if (!pub || !defs) return DBG_ERR_INTERNAL;

    for (uint16_t i = 0; i < count; i++) {
        dbg_status_t rc = dbg_pub_register_field(pub, &defs[i]);
        if (rc != DBG_OK) return rc;
    }
    return DBG_OK;
}

dbg_status_t dbg_pub_unregister_field(dbg_publisher_t *pub,
                                      uint64_t         field_id)
{
    if (!pub) return DBG_ERR_INTERNAL;

    pub_field_entry_t *f = pub_find_field(pub, field_id);
    if (!f) return DBG_ERR_UNKNOWN_FIELD;

    f->active = 0;
    pub->field_count--;
    pub->stats.registered_fields = pub->field_count;
    return DBG_OK;
}

dbg_status_t dbg_pub_unregister_all_fields(dbg_publisher_t *pub)
{
    if (!pub) return DBG_ERR_INTERNAL;
    
    for (uint32_t i = 0; i < DBG_PUB_MAX_FIELDS; i++) {
        pub->fields[i].active = 0;
        pub->fields[i].field_id = 0;
    }

    pub->field_count = 0;
    pub->stats.registered_fields = pub->field_count;
    return DBG_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
   CYCLIC FRAME SEND  (HOT PATH)
   ══════════════════════════════════════════════════════════════════════════ */

int dbg_pub_send_all(dbg_publisher_t *pub)
{
    if (!pub) return (int)DBG_ERR_INTERNAL;

    uint64_t t_start = dbg_get_time_us();
    int frames_sent = 0;

    for (uint32_t s = 0; s < DBG_PUB_MAX_SUBSCRIPTIONS; s++) {
        pub_subscription_t *sub = &pub->subs[s];
        if (!sub->active) continue;

        /* Build frame header directly in the pre-allocated buffer */
        dbg_frame_t *frame = (dbg_frame_t *)sub->frame_buf;
        dbg_header_init(&frame->header, DBG_MSG_FRAME, 0);
        frame->sub_id       = sub->sub_id;
        frame->frame_size   = sub->frame_size;
        frame->sequence     = sub->sequence++;
        frame->timestamp_us = t_start;

        /* Pack values into payload — tight loop, no branches */
        uint8_t *payload = frame->payload;
        for (uint16_t i = 0; i < sub->field_count; i++) {
            const sub_field_binding_t *b = &sub->fields[i];
            memcpy(payload + b->offset, (const void *)b->ptr, b->size);
        }

        /* Single send per subscription */
        uint32_t wire_sz = dbg_frame_wire_size(sub->frame_size);
        if (dbg_socket_sendto(&pub->data_sock,
                              sub->frame_buf,
                              wire_sz,
                              &sub->client_data_addr) == DBG_OK) {
            frames_sent++;
        }
    }

    uint64_t t_end = dbg_get_time_us();
    pub->stats.frames_sent  += (uint32_t)frames_sent;
    pub->stats.last_send_us  = t_end - t_start;

    return frames_sent;
}

/* ══════════════════════════════════════════════════════════════════════════
   CONFIG POLL
   ══════════════════════════════════════════════════════════════════════════ */

int dbg_pub_poll_config(dbg_publisher_t *pub)
{
    if (!pub) return (int)DBG_ERR_INTERNAL;

    int messages_processed = 0;
    const int max_per_poll = 16;  /* bound work per call */

    for (int m = 0; m < max_per_poll; m++) {
        dbg_addr_t from;
        int n = dbg_socket_recvfrom(&pub->config_sock,
                                    pub->rx_buf,
                                    sizeof(pub->rx_buf),
                                    &from);

        if (n <= 0) break;  /* 0 = empty, -1 = error; either way stop */

        if ((uint32_t)n < sizeof(dbg_header_t)) continue;

        const dbg_header_t *hdr = (const dbg_header_t *)pub->rx_buf;
        if (dbg_header_validate(hdr) != DBG_OK) continue;

        switch ((dbg_msg_type_t)hdr->msg_type) {
        case DBG_MSG_SUBSCRIBE:
            pub_handle_subscribe(pub, pub->rx_buf, (uint32_t)n, &from);
            break;
        case DBG_MSG_UNSUBSCRIBE:
            pub_handle_unsubscribe(pub, pub->rx_buf, (uint32_t)n, &from);
            break;
        case DBG_MSG_WRITE:
            pub_handle_write(pub, pub->rx_buf, (uint32_t)n, &from);
            break;
        case DBG_MSG_FIELD_LIST_REQUEST:
            pub_handle_field_list(pub, pub->rx_buf, (uint32_t)n, &from);
            break;
        case DBG_MSG_HEARTBEAT:
            pub_handle_heartbeat(pub, pub->rx_buf, (uint32_t)n, &from);
            break;
        default:
            break;  /* silently ignore unknown message types */
        }

        messages_processed++;
        pub->stats.config_msgs_received++;
    }

    pub_check_heartbeat_timeouts(pub);

    return messages_processed;
}

/* ══════════════════════════════════════════════════════════════════════════
   SUBSCRIBE HANDLER
   ══════════════════════════════════════════════════════════════════════════ */

static void pub_handle_subscribe(dbg_publisher_t  *pub,
                                 const uint8_t    *data,
                                 uint32_t          len,
                                 const dbg_addr_t *from)
{
    if (len < sizeof(dbg_subscribe_request_t)) return;

    const dbg_subscribe_request_t *req =
        (const dbg_subscribe_request_t *)data;

    uint16_t field_count = req->field_count;

    if (len < dbg_subscribe_request_size(field_count)) return;

    /* Validate field count */
    if (field_count == 0 || field_count > DBG_PUB_MAX_SUB_FIELDS) {
        dbg_subscribe_ack_t ack;
        memset(&ack, 0, sizeof(ack));
        dbg_header_init(&ack.header, DBG_MSG_SUBSCRIBE_ACK, req->header.tx_id);
        ack.sub_id  = req->sub_id;
        ack.status  = (int16_t)DBG_ERR_TOO_MANY_FIELDS;
        pub_send_config(pub, &ack, sizeof(ack), from);
        return;
    }

    /* Tear down any existing subscription with the same ID */
    pub_subscription_t *existing = pub_find_sub(pub, req->sub_id);
    if (existing) {
        pub_teardown_sub(pub, existing);
    }

    /* Allocate a slot */
    pub_subscription_t *sub = pub_alloc_sub(pub);
    if (!sub) {
        dbg_subscribe_ack_t ack;
        memset(&ack, 0, sizeof(ack));
        dbg_header_init(&ack.header, DBG_MSG_SUBSCRIBE_ACK, req->header.tx_id);
        ack.sub_id = req->sub_id;
        ack.status = (int16_t)DBG_ERR_TOO_MANY_SUBS;
        pub_send_config(pub, &ack, sizeof(ack), from);
        return;
    }

    /* Build ack and subscription simultaneously */
    uint32_t ack_size = dbg_subscribe_ack_size(field_count);
    if (ack_size > sizeof(pub->tx_buf)) return;  /* shouldn't happen */

    dbg_subscribe_ack_t *ack = (dbg_subscribe_ack_t *)pub->tx_buf;
    memset(ack, 0, ack_size);
    dbg_header_init(&ack->header, DBG_MSG_SUBSCRIBE_ACK, req->header.tx_id);
    ack->sub_id      = req->sub_id;
    ack->field_count = field_count;

    sub->sub_id            = req->sub_id;
    sub->interval_us       = req->interval_us;
    sub->sequence          = 0;
    sub->last_heartbeat_us = dbg_get_time_us();
    sub->client_config_addr = *from;

    /* Data address: same host as config source, different port */
    sub->client_data_addr      = *from;
    sub->client_data_addr.port = req->data_port;

    uint16_t offset        = 0;
    uint16_t valid_count   = 0;
    int16_t  overall_status = (int16_t)DBG_OK;

    for (uint16_t i = 0; i < field_count; i++) {
        const dbg_sub_field_t *sf = &req->fields[i];
        dbg_sub_field_ack_t   *fa = &ack->fields[i];

        fa->field_index = i;
        fa->value_type  = sf->value_type;

        /* Look up in registry */
        pub_field_entry_t *f = pub_find_field(pub, sf->field_id);
        if (!f) {
            fa->status = (int16_t)DBG_ERR_UNKNOWN_FIELD;
            fa->offset = 0;
            fa->size   = 0;
            overall_status = (int16_t)DBG_ERR_UNKNOWN_FIELD;
            continue;
        }

        /* Type check */
        if ((dbg_value_type_t)sf->value_type != f->type) {
            fa->status = (int16_t)DBG_ERR_TYPE_MISMATCH;
            fa->offset = 0;
            fa->size   = 0;
            overall_status = (int16_t)DBG_ERR_TYPE_MISMATCH;
            continue;
        }

        /* Payload overflow check */
        if ((uint32_t)offset + f->size > DBG_MAX_FRAME_PAYLOAD) {
            fa->status = (int16_t)DBG_ERR_PAYLOAD_OVERFLOW;
            fa->offset = 0;
            fa->size   = 0;
            overall_status = (int16_t)DBG_ERR_PAYLOAD_OVERFLOW;
            continue;
        }

        /* Success — bind this field */
        fa->status = (int16_t)DBG_OK;
        fa->offset = offset;
        fa->size   = f->size;

        sub->fields[valid_count].ptr        = f->ptr;
        sub->fields[valid_count].offset     = offset;
        sub->fields[valid_count].size       = f->size;
        sub->fields[valid_count].value_type = (uint8_t)f->type;
        sub->fields[valid_count].field_id   = f->field_id;

        offset += f->size;
        valid_count++;
    }

    sub->field_count = valid_count;
    sub->frame_size  = offset;

    ack->status             = overall_status;
    ack->frame_size         = offset;
    ack->actual_interval_us = req->interval_us;

    /* Activate only if at least one field succeeded */
    if (valid_count > 0) {
        sub->active = 1;
        pub->stats.active_subscriptions++;
        if (pub->sub_event_cb) {
            pub->sub_event_cb(sub->sub_id, 1, pub->sub_event_ctx);
        }
    }

    pub_send_config(pub, ack, ack_size, from);
}

/* ══════════════════════════════════════════════════════════════════════════
   UNSUBSCRIBE HANDLER
   ══════════════════════════════════════════════════════════════════════════ */

static void pub_handle_unsubscribe(dbg_publisher_t  *pub,
                                   const uint8_t    *data,
                                   uint32_t          len,
                                   const dbg_addr_t *from)
{
    if (len < sizeof(dbg_unsubscribe_request_t)) return;

    const dbg_unsubscribe_request_t *req =
        (const dbg_unsubscribe_request_t *)data;

    dbg_unsubscribe_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    dbg_header_init(&ack.header, DBG_MSG_UNSUBSCRIBE_ACK, req->header.tx_id);
    ack.sub_id = req->sub_id;

    pub_subscription_t *sub = pub_find_sub(pub, req->sub_id);
    if (sub) {
        pub_teardown_sub(pub, sub);
        ack.status = (int16_t)DBG_OK;
    } else {
        ack.status = (int16_t)DBG_ERR_UNKNOWN_SUB;
    }

    pub_send_config(pub, &ack, sizeof(ack), from);
}

/* ══════════════════════════════════════════════════════════════════════════
   WRITE HANDLER
   ══════════════════════════════════════════════════════════════════════════ */

static void pub_handle_write(dbg_publisher_t  *pub,
                             const uint8_t    *data,
                             uint32_t          len,
                             const dbg_addr_t *from)
{
    if (len < sizeof(dbg_write_request_t)) return;

    const dbg_write_request_t *req = (const dbg_write_request_t *)data;
    uint16_t item_count = req->item_count;

    if (item_count == 0 || item_count > DBG_MAX_WRITE_ITEMS) return;

    uint32_t expected = dbg_write_request_size(item_count);
    if (len < expected) return;

    uint32_t ack_size = dbg_write_ack_size(item_count);
    if (ack_size > sizeof(pub->tx_buf)) return;

    dbg_write_ack_t *ack = (dbg_write_ack_t *)pub->tx_buf;
    memset(ack, 0, ack_size);
    dbg_header_init(&ack->header, DBG_MSG_WRITE_ACK, req->header.tx_id);
    ack->item_count = item_count;

    int16_t overall = (int16_t)DBG_OK;

    for (uint16_t i = 0; i < item_count; i++) {
        const dbg_write_item_t *wi = &req->items[i];
        dbg_write_result_t     *wr = &ack->items[i];

        wr->field_id = wi->field_id;

        pub_field_entry_t *f = pub_find_field(pub, wi->field_id);
        if (!f) {
            wr->status = (int16_t)DBG_ERR_UNKNOWN_FIELD;
            overall    = (int16_t)DBG_ERR_UNKNOWN_FIELD;
            pub->stats.writes_rejected++;
            continue;
        }

        if (f->access != DBG_ACCESS_READ_WRITE) {
            wr->status = (int16_t)DBG_ERR_ACCESS_DENIED;
            overall    = (int16_t)DBG_ERR_ACCESS_DENIED;
            pub->stats.writes_rejected++;
            continue;
        }

        if ((dbg_value_type_t)wi->value_type != f->type) {
            wr->status = (int16_t)DBG_ERR_TYPE_MISMATCH;
            overall    = (int16_t)DBG_ERR_TYPE_MISMATCH;
            pub->stats.writes_rejected++;
            continue;
        }

        if (pub->validate_cb) {
            dbg_status_t vrc = pub->validate_cb(
                wi->field_id,
                (dbg_value_type_t)wi->value_type,
                &wi->value,
                pub->validate_ctx);
            if (vrc != DBG_OK) {
                wr->status = (int16_t)vrc;
                overall    = (int16_t)vrc;
                pub->stats.writes_rejected++;
                continue;
            }
        }

        /* Apply write */
        memcpy((void *)f->ptr, &wi->value, f->size);
        wr->status = (int16_t)DBG_OK;
        pub->stats.writes_applied++;

        if (pub->write_notify_cb) {
            pub->write_notify_cb(wi->field_id,
                                 (dbg_value_type_t)wi->value_type,
                                 &wi->value,
                                 pub->write_notify_ctx);
        }
    }

    ack->status = overall;
    pub_send_config(pub, ack, ack_size, from);
}

/* ══════════════════════════════════════════════════════════════════════════
   FIELD LIST HANDLER
   ══════════════════════════════════════════════════════════════════════════ */

static void pub_handle_field_list(dbg_publisher_t  *pub,
                                  const uint8_t    *data,
                                  uint32_t          len,
                                  const dbg_addr_t *from)
{
    if (len < sizeof(dbg_field_list_request_t)) return;

    const dbg_field_list_request_t *req =
        (const dbg_field_list_request_t *)data;

    uint16_t req_offset = req->offset;
    uint16_t req_max    = req->max_count;
    if (req_max == 0) req_max = 0xFFFF;

    /* Collect active field indices */
    uint16_t total_active = 0;
    uint32_t active_indices[DBG_PUB_MAX_FIELDS];

    for (uint32_t i = 0; i < DBG_PUB_MAX_FIELDS; i++) {
        if (pub->fields[i].active) {
            active_indices[total_active++] = i;
        }
    }

    uint16_t start     = (req_offset < total_active) ? req_offset : total_active;
    uint16_t remaining = total_active - start;

    uint16_t max_per_msg = (uint16_t)(
        (sizeof(pub->tx_buf) - sizeof(dbg_field_list_response_t))
        / sizeof(dbg_field_descriptor_t));

    uint16_t to_send = remaining < req_max ? remaining : req_max;
    if (to_send > max_per_msg) to_send = max_per_msg;

    uint32_t resp_size = dbg_field_list_response_size(to_send);
    dbg_field_list_response_t *resp = (dbg_field_list_response_t *)pub->tx_buf;

    memset(resp, 0, resp_size);
    dbg_header_init(&resp->header, DBG_MSG_FIELD_LIST_RESPONSE, req->header.tx_id);
    resp->total_fields = total_active;
    resp->field_count  = to_send;
    resp->offset       = start;
    resp->status       = (int16_t)DBG_OK;

    for (uint16_t i = 0; i < to_send; i++) {
        uint32_t fi = active_indices[start + i];
        const pub_field_entry_t *f = &pub->fields[fi];
        dbg_field_descriptor_t  *d = &resp->fields[i];

        d->field_id   = f->field_id;
        d->value_type = (uint8_t)f->type;
        d->access     = (uint8_t)f->access;
        memcpy(d->name, f->name, DBG_FIELD_NAME_MAX);
    }

    pub_send_config(pub, resp, resp_size, from);
}

/* ══════════════════════════════════════════════════════════════════════════
   HEARTBEAT HANDLER
   ══════════════════════════════════════════════════════════════════════════ */

static void pub_handle_heartbeat(dbg_publisher_t  *pub,
                                 const uint8_t    *data,
                                 uint32_t          len,
                                 const dbg_addr_t *from)
{
    if (len < sizeof(dbg_heartbeat_t)) return;

    const dbg_heartbeat_t *hb = (const dbg_heartbeat_t *)data;

    /* Update last-seen time on any subscription from this address */
    uint64_t now = dbg_get_time_us();
    for (uint32_t s = 0; s < DBG_PUB_MAX_SUBSCRIPTIONS; s++) {
        pub_subscription_t *sub = &pub->subs[s];
        if (sub->active && dbg_addr_equal(&sub->client_config_addr, from)) {
            sub->last_heartbeat_us = now;
        }
    }

    /* Echo timestamp back for RTT measurement */
    dbg_heartbeat_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    dbg_header_init(&ack.header, DBG_MSG_HEARTBEAT_ACK, hb->header.tx_id);
    ack.timestamp_us = hb->timestamp_us;

    pub_send_config(pub, &ack, sizeof(ack), from);
}

/* ══════════════════════════════════════════════════════════════════════════
   HEARTBEAT TIMEOUT CHECK
   ══════════════════════════════════════════════════════════════════════════ */

static void pub_check_heartbeat_timeouts(dbg_publisher_t *pub)
{
    if (pub->heartbeat_timeout_us == 0) return;  /* disabled */

    uint64_t now = dbg_get_time_us();

    for (uint32_t s = 0; s < DBG_PUB_MAX_SUBSCRIPTIONS; s++) {
        pub_subscription_t *sub = &pub->subs[s];
        if (!sub->active) continue;

        if ((now - sub->last_heartbeat_us) > pub->heartbeat_timeout_us) {
            DBG_WARN("[pub] sub_id=%u heartbeat timeout — tearing down", sub->sub_id);
            pub_teardown_sub(pub, sub);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   CALLBACK SETTERS
   ══════════════════════════════════════════════════════════════════════════ */

void dbg_pub_set_validate_cb(dbg_publisher_t   *pub,
                              dbg_validate_cb_t  cb,
                              void              *user_ctx)
{
    if (!pub) return;
    pub->validate_cb  = cb;
    pub->validate_ctx = user_ctx;
}

void dbg_pub_set_write_notify_cb(dbg_publisher_t       *pub,
                                  dbg_write_notify_cb_t  cb,
                                  void                  *user_ctx)
{
    if (!pub) return;
    pub->write_notify_cb  = cb;
    pub->write_notify_ctx = user_ctx;
}

void dbg_pub_set_sub_event_cb(dbg_publisher_t     *pub,
                               dbg_sub_event_cb_t   cb,
                               void                *user_ctx)
{
    if (!pub) return;
    pub->sub_event_cb  = cb;
    pub->sub_event_ctx = user_ctx;
}

/* ══════════════════════════════════════════════════════════════════════════
   STATISTICS
   ══════════════════════════════════════════════════════════════════════════ */

void dbg_pub_get_stats(const dbg_publisher_t *pub,
                       dbg_pub_stats_t       *stats)
{
    if (!pub || !stats) return;
    memcpy(stats, &pub->stats, sizeof(*stats));
}

/* ══════════════════════════════════════════════════════════════════════════
   SUBSCRIPTION MANAGEMENT
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_pub_close_subscription(dbg_publisher_t *pub,
                                        uint16_t         sub_id)
{
    if (!pub) return DBG_ERR_INTERNAL;

    pub_subscription_t *sub = pub_find_sub(pub, sub_id);
    if (!sub) return DBG_ERR_UNKNOWN_SUB;

    pub_teardown_sub(pub, sub);
    return DBG_OK;
}

void dbg_pub_close_all_subscriptions(dbg_publisher_t *pub)
{
    if (!pub) return;

    for (uint32_t s = 0; s < DBG_PUB_MAX_SUBSCRIPTIONS; s++) {
        if (pub->subs[s].active) {
            pub_teardown_sub(pub, &pub->subs[s]);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   INTERNAL — SUBSCRIPTION TEARDOWN
   ══════════════════════════════════════════════════════════════════════════ */

static void pub_teardown_sub(dbg_publisher_t *pub, pub_subscription_t *sub)
{
    if (!sub->active) return;

    uint16_t sub_id  = sub->sub_id;

    sub->active      = 0;
    sub->field_count = 0;
    sub->frame_size  = 0;
    sub->sequence    = 0;

    if (pub->stats.active_subscriptions > 0) {
        pub->stats.active_subscriptions--;
    }

    if (pub->sub_event_cb) {
        pub->sub_event_cb(sub_id, 0, pub->sub_event_ctx);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   INTERNAL — FIELD REGISTRY LOOKUP
   ══════════════════════════════════════════════════════════════════════════ */

static pub_field_entry_t* pub_find_field(dbg_publisher_t *pub,
                                         uint64_t field_id)
{
    for (uint32_t i = 0; i < DBG_PUB_MAX_FIELDS; i++) {
        if (pub->fields[i].active &&
            pub->fields[i].field_id == field_id) {
            return &pub->fields[i];
        }
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
   INTERNAL — SUBSCRIPTION LOOKUP / ALLOC
   ══════════════════════════════════════════════════════════════════════════ */

static pub_subscription_t* pub_find_sub(dbg_publisher_t *pub,
                                        uint16_t sub_id)
{
    for (uint32_t s = 0; s < DBG_PUB_MAX_SUBSCRIPTIONS; s++) {
        if (pub->subs[s].active &&
            pub->subs[s].sub_id == sub_id) {
            return &pub->subs[s];
        }
    }
    return NULL;
}

static pub_subscription_t* pub_alloc_sub(dbg_publisher_t *pub)
{
    for (uint32_t s = 0; s < DBG_PUB_MAX_SUBSCRIPTIONS; s++) {
        if (!pub->subs[s].active) {
            memset(&pub->subs[s], 0, sizeof(pub->subs[s]));
            return &pub->subs[s];
        }
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
   INTERNAL — CONFIG SEND
   ══════════════════════════════════════════════════════════════════════════ */

static void pub_send_config(dbg_publisher_t  *pub,
                            const void       *data,
                            uint32_t          len,
                            const dbg_addr_t *to)
{
    /* Responses are sent from the config socket back to the client's
       source address, so the client receives them on its config socket. */
    (void)dbg_socket_sendto(&pub->config_sock, data, len, to);
}
