/******************************************************************************
 * dbg_subscriber.c
 *
 * Subscriber (client-side) implementation for the debug pub/sub system.
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

/** Tracks per-subscription state on the client side. */
typedef struct {
    int                    active;
    uint16_t               sub_id;
    uint32_t               last_sequence;
    int                    sequence_initialised;
    dbg_sub_layout_t       layout;
    dbg_sub_stats_t        stats;
} sub_client_entry_t;

/** Main subscriber structure. */
struct dbg_subscriber {
    /* Server addresses */
    dbg_addr_t              server_config_addr;
    dbg_addr_t              server_data_addr;   /* resolved but not actively filtered */
    uint16_t                data_port;           /* local data socket port (OS-assigned) */

    /* Sockets */
    dbg_socket_t            data_sock;
    dbg_socket_t            config_sock;

    /* Configuration */
    uint32_t                config_timeout_ms;
    uint32_t                heartbeat_interval_ms;

    /* Subscription tracking */
    sub_client_entry_t      subs[DBG_PUB_MAX_SUBSCRIPTIONS];

    /* Heartbeat state */
    uint64_t                last_heartbeat_sent_us;

    /* Callbacks */
    dbg_disconnect_cb_t     disconnect_cb;
    void                   *disconnect_ctx;

    /* Transaction ID generator */
    uint32_t                next_tx_id;

    /* Buffers */
    uint8_t                 rx_buf[DBG_PUB_CONFIG_BUF_SIZE];
    uint8_t                 tx_buf[DBG_PUB_TX_BUF_SIZE];
    uint8_t                 data_rx_buf[sizeof(dbg_frame_t) + DBG_MAX_FRAME_PAYLOAD];
};

/* ══════════════════════════════════════════════════════════════════════════
   FORWARD DECLARATIONS (INTERNAL)
   ══════════════════════════════════════════════════════════════════════════ */

static dbg_status_t sub_send_config(dbg_subscriber_t *sub,
                                    const void *data, uint32_t len);
static dbg_status_t sub_recv_config(dbg_subscriber_t *sub,
                                    uint16_t expected_msg_type,
                                    uint32_t expected_tx_id,
                                    void **out_data, uint32_t *out_len);

static sub_client_entry_t* sub_find_entry(dbg_subscriber_t *sub, uint16_t sub_id);
static sub_client_entry_t* sub_alloc_entry(dbg_subscriber_t *sub);
static void                sub_free_entry(sub_client_entry_t *entry);

static void sub_check_heartbeat(dbg_subscriber_t *sub);
static void sub_track_sequence(sub_client_entry_t *entry, uint32_t seq);

/* ══════════════════════════════════════════════════════════════════════════
   LIFECYCLE
   ══════════════════════════════════════════════════════════════════════════ */

dbg_subscriber_t* dbg_sub_create(const dbg_sub_config_t *config)
{
    if (!config || !config->host) return NULL;

    dbg_subscriber_t *sub = (dbg_subscriber_t *)calloc(1, sizeof(*sub));
    if (!sub) return NULL;

    /* Apply defaults */
    uint16_t config_port = config->config_port
                           ? config->config_port
                           : DBG_CONFIG_PORT_DEFAULT;
    uint16_t data_port   = config->data_port
                           ? config->data_port
                           : DBG_DATA_PORT_DEFAULT;

    sub->config_timeout_ms     = config->config_timeout_ms
                                 ? config->config_timeout_ms
                                 : 1000u;
    sub->heartbeat_interval_ms = config->heartbeat_interval_ms
                                 ? config->heartbeat_interval_ms
                                 : 1000u;

    /* ── Resolve server addresses ────────────────────────────────────────── */
    if (dbg_addr_from_string(config->host, config_port,
                             &sub->server_config_addr) != DBG_OK) {
        DBG_ERROR("[sub] failed to resolve host '%s'", config->host);
        free(sub);
        return NULL;
    }
    if (dbg_addr_from_string(config->host, data_port,
                             &sub->server_data_addr) != DBG_OK) {
        DBG_ERROR("[sub] failed to resolve host '%s'", config->host);
        free(sub);
        return NULL;
    }

    /* ── Config socket (blocking recv with timeout for request/reply) ─────── */
    if (dbg_socket_open(&sub->config_sock) != DBG_OK) {
        DBG_ERROR("[sub] failed to open config socket");
        free(sub);
        return NULL;
    }
    if (dbg_socket_set_recv_timeout(&sub->config_sock,
                                    sub->config_timeout_ms) != DBG_OK) {
        DBG_ERROR("[sub] failed to set config socket timeout");
        dbg_socket_close(&sub->config_sock);
        free(sub);
        return NULL;
    }

    /* ── Data socket (non-blocking, OS-assigned port for inbound frames) ──── */
    if (dbg_socket_open(&sub->data_sock) != DBG_OK) {
        DBG_ERROR("[sub] failed to open data socket");
        dbg_socket_close(&sub->config_sock);
        free(sub);
        return NULL;
    }
    if (dbg_socket_bind_any(&sub->data_sock) != DBG_OK) {
        DBG_ERROR("[sub] failed to bind data socket");
        dbg_socket_close(&sub->config_sock);
        dbg_socket_close(&sub->data_sock);
        free(sub);
        return NULL;
    }
    if (dbg_socket_get_local_port(&sub->data_sock, &sub->data_port) != DBG_OK) {
        DBG_ERROR("[sub] failed to query assigned data port");
        dbg_socket_close(&sub->config_sock);
        dbg_socket_close(&sub->data_sock);
        free(sub);
        return NULL;
    }
    DBG_DEBUG("[sub] data socket bound to port %u", sub->data_port);

    if (dbg_socket_set_nonblocking(&sub->data_sock, 1) != DBG_OK) {
        DBG_ERROR("[sub] failed to set data socket non-blocking");
        dbg_socket_close(&sub->config_sock);
        dbg_socket_close(&sub->data_sock);
        free(sub);
        return NULL;
    }
    dbg_socket_set_recv_bufsize(&sub->data_sock, 524288);  /* 512 KB */

    sub->next_tx_id             = 1;
    sub->last_heartbeat_sent_us = dbg_get_time_us();

    return sub;
}

void dbg_sub_destroy(dbg_subscriber_t *sub)
{
    if (!sub) return;

    /* Best-effort unsubscribe for all active subscriptions */
    for (uint32_t i = 0; i < DBG_PUB_MAX_SUBSCRIPTIONS; i++) {
        if (sub->subs[i].active) {
            dbg_sub_unsubscribe(sub, sub->subs[i].sub_id);
        }
    }

    dbg_socket_close(&sub->data_sock);
    dbg_socket_close(&sub->config_sock);

    free(sub);
}

/* ══════════════════════════════════════════════════════════════════════════
   SUBSCRIBE
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_sub_subscribe(dbg_subscriber_t        *sub,
                               uint16_t                 sub_id,
                               const uint64_t          *field_ids,
                               const dbg_value_type_t  *types,
                               uint16_t                 count,
                               uint32_t                 interval_us,
                               dbg_sub_layout_t        *out_layout)
{
    if (!sub || !field_ids || !types) return DBG_ERR_INTERNAL;
    if (count == 0 || count > DBG_MAX_SUB_FIELDS) return DBG_ERR_TOO_MANY_FIELDS;

    uint32_t req_size = dbg_subscribe_request_size(count);
    if (req_size > sizeof(sub->tx_buf)) return DBG_ERR_TOO_MANY_FIELDS;

    dbg_subscribe_request_t *req = (dbg_subscribe_request_t *)sub->tx_buf;
    memset(req, 0, req_size);

    uint32_t tx_id = sub->next_tx_id++;
    dbg_header_init(&req->header, DBG_MSG_SUBSCRIBE, tx_id);
    req->sub_id      = sub_id;
    req->field_count = count;
    req->interval_us = interval_us;
    req->data_port   = sub->data_port;

    for (uint16_t i = 0; i < count; i++) {
        req->fields[i].field_id   = field_ids[i];
        req->fields[i].value_type = (uint8_t)types[i];
    }

    dbg_status_t rc = sub_send_config(sub, req, req_size);
    if (rc != DBG_OK) return rc;

    void    *resp_data = NULL;
    uint32_t resp_len  = 0;

    rc = sub_recv_config(sub, DBG_MSG_SUBSCRIBE_ACK, tx_id,
                         &resp_data, &resp_len);
    if (rc != DBG_OK) return rc;

    if (resp_len < sizeof(dbg_subscribe_ack_t)) return DBG_ERR_MALFORMED;

    const dbg_subscribe_ack_t *ack = (const dbg_subscribe_ack_t *)resp_data;

    if (resp_len < dbg_subscribe_ack_size(ack->field_count))
        return DBG_ERR_MALFORMED;

    if (ack->sub_id != sub_id) return DBG_ERR_MALFORMED;

    /* Replace any existing entry for this sub_id */
    sub_client_entry_t *existing = sub_find_entry(sub, sub_id);
    if (existing) sub_free_entry(existing);

    sub_client_entry_t *entry = sub_alloc_entry(sub);
    if (!entry) return DBG_ERR_TOO_MANY_SUBS;

    entry->active               = 1;
    entry->sub_id               = sub_id;
    entry->last_sequence        = 0;
    entry->sequence_initialised = 0;
    memset(&entry->stats, 0, sizeof(entry->stats));

    /* Build layout from ack */
    dbg_sub_layout_t *layout       = &entry->layout;
    layout->sub_id                 = sub_id;
    layout->field_count            = ack->field_count;
    layout->frame_size             = ack->frame_size;
    layout->actual_interval_us     = ack->actual_interval_us;

    for (uint16_t i = 0; i < ack->field_count; i++) {
        const dbg_sub_field_ack_t *fa = &ack->fields[i];
        dbg_sub_field_layout_t    *fl = &layout->fields[i];

        fl->field_id = field_ids[i];  /* preserve original request order */
        fl->type     = (dbg_value_type_t)fa->value_type;
        fl->size     = fa->size;
        fl->offset   = fa->offset;
    }

    if (out_layout) {
        memcpy(out_layout, layout, sizeof(*out_layout));
    }

    return (dbg_status_t)ack->status;
}

/* ══════════════════════════════════════════════════════════════════════════
   UNSUBSCRIBE
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_sub_unsubscribe(dbg_subscriber_t *sub,
                                 uint16_t          sub_id)
{
    if (!sub) return DBG_ERR_INTERNAL;

    dbg_unsubscribe_request_t req;
    memset(&req, 0, sizeof(req));

    uint32_t tx_id = sub->next_tx_id++;
    dbg_header_init(&req.header, DBG_MSG_UNSUBSCRIBE, tx_id);
    req.sub_id = sub_id;

    dbg_status_t rc = sub_send_config(sub, &req, sizeof(req));
    if (rc != DBG_OK) {
        /* Send failed — clean up locally anyway */
        sub_client_entry_t *entry = sub_find_entry(sub, sub_id);
        if (entry) sub_free_entry(entry);
        return rc;
    }

    void    *resp_data = NULL;
    uint32_t resp_len  = 0;

    rc = sub_recv_config(sub, DBG_MSG_UNSUBSCRIBE_ACK, tx_id,
                         &resp_data, &resp_len);

    /* Clean up local entry regardless of ack success */
    sub_client_entry_t *entry = sub_find_entry(sub, sub_id);
    if (entry) sub_free_entry(entry);

    if (rc != DBG_OK) return rc;
    if (resp_len < sizeof(dbg_unsubscribe_ack_t)) return DBG_ERR_MALFORMED;

    const dbg_unsubscribe_ack_t *ack =
        (const dbg_unsubscribe_ack_t *)resp_data;
    return (dbg_status_t)ack->status;
}

/* ══════════════════════════════════════════════════════════════════════════
   DATA FRAME POLLING
   ══════════════════════════════════════════════════════════════════════════ */

int dbg_sub_poll(dbg_subscriber_t *sub,
                 dbg_frame_cb_t    cb,
                 void             *user_ctx)
{
    if (!sub) return (int)DBG_ERR_INTERNAL;

    int frames_received = 0;
    const int max_per_poll = 256;  /* bound work per call */

    for (int m = 0; m < max_per_poll; m++) {
        int n = dbg_socket_recvfrom(&sub->data_sock,
                                    sub->data_rx_buf,
                                    sizeof(sub->data_rx_buf),
                                    NULL);  /* we don't filter by source */
        if (n == 0) break;  /* buffer empty */
        if (n < 0)  break;  /* socket error */

        if ((uint32_t)n < sizeof(dbg_frame_t)) continue;

        const dbg_frame_t *frame = (const dbg_frame_t *)sub->data_rx_buf;

        if (dbg_header_validate(&frame->header) != DBG_OK) continue;
        if (frame->header.msg_type != DBG_MSG_FRAME)       continue;

        uint32_t expected_size = dbg_frame_wire_size(frame->frame_size);
        if ((uint32_t)n < expected_size) continue;

        sub_client_entry_t *entry = sub_find_entry(sub, frame->sub_id);
        if (!entry) continue;

        sub_track_sequence(entry, frame->sequence);

        entry->stats.frames_received++;
        entry->stats.bytes_received          += (uint32_t)n;
        entry->stats.last_frame_timestamp_us  = frame->timestamp_us;
        entry->stats.last_sequence            = frame->sequence;

        if (cb) {
            cb(frame->sub_id,
               frame->sequence,
               frame->timestamp_us,
               frame->payload,
               frame->frame_size,
               user_ctx);
        }

        frames_received++;
    }

    sub_check_heartbeat(sub);

    return frames_received;
}

/* ══════════════════════════════════════════════════════════════════════════
   SINGLE WRITE
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_sub_write(dbg_subscriber_t  *sub,
                           uint64_t           field_id,
                           dbg_value_type_t   type,
                           const dbg_value_t *value)
{
    if (!sub || !value) return DBG_ERR_INTERNAL;

    dbg_write_item_t item;
    memset(&item, 0, sizeof(item));
    item.field_id   = field_id;
    item.value_type = (uint8_t)type;
    memcpy(&item.value, value, sizeof(dbg_value_t));

    return dbg_sub_write_multi(sub, &item, 1, NULL);
}

/* ══════════════════════════════════════════════════════════════════════════
   MULTI WRITE
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_sub_write_multi(dbg_subscriber_t       *sub,
                                 const dbg_write_item_t *items,
                                 uint16_t                count,
                                 dbg_status_t           *out_status)
{
    if (!sub || !items) return DBG_ERR_INTERNAL;
    if (count == 0 || count > DBG_MAX_WRITE_ITEMS) return DBG_ERR_TOO_MANY_FIELDS;

    uint32_t req_size = dbg_write_request_size(count);
    if (req_size > sizeof(sub->tx_buf)) return DBG_ERR_TOO_MANY_FIELDS;

    dbg_write_request_t *req = (dbg_write_request_t *)sub->tx_buf;
    memset(req, 0, req_size);

    uint32_t tx_id = sub->next_tx_id++;
    dbg_header_init(&req->header, DBG_MSG_WRITE, tx_id);
    req->item_count = count;

    memcpy(req->items, items, count * sizeof(dbg_write_item_t));

    dbg_status_t rc = sub_send_config(sub, req, req_size);
    if (rc != DBG_OK) return rc;

    void    *resp_data = NULL;
    uint32_t resp_len  = 0;

    rc = sub_recv_config(sub, DBG_MSG_WRITE_ACK, tx_id,
                         &resp_data, &resp_len);
    if (rc != DBG_OK) return rc;

    if (resp_len < sizeof(dbg_write_ack_t)) return DBG_ERR_MALFORMED;

    const dbg_write_ack_t *ack = (const dbg_write_ack_t *)resp_data;

    if (resp_len < dbg_write_ack_size(ack->item_count))
        return DBG_ERR_MALFORMED;

    if (out_status && ack->item_count == count) {
        for (uint16_t i = 0; i < count; i++) {
            out_status[i] = (dbg_status_t)ack->items[i].status;
        }
    }

    return (dbg_status_t)ack->status;
}

/* ══════════════════════════════════════════════════════════════════════════
   FIELD LIST DISCOVERY
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_sub_request_field_list(dbg_subscriber_t   *sub,
                                        dbg_field_list_cb_t cb,
                                        void               *user_ctx)
{
    if (!sub || !cb) return DBG_ERR_INTERNAL;

    uint16_t offset = 0;
    uint16_t total  = 0;
    int      first  = 1;

    for (;;) {
        dbg_field_list_request_t req;
        memset(&req, 0, sizeof(req));

        uint32_t tx_id = sub->next_tx_id++;
        dbg_header_init(&req.header, DBG_MSG_FIELD_LIST_REQUEST, tx_id);
        req.offset    = offset;
        req.max_count = 0;  /* let server decide page size */

        dbg_status_t rc = sub_send_config(sub, &req, sizeof(req));
        if (rc != DBG_OK) return rc;

        void    *resp_data = NULL;
        uint32_t resp_len  = 0;

        rc = sub_recv_config(sub, DBG_MSG_FIELD_LIST_RESPONSE, tx_id,
                             &resp_data, &resp_len);
        if (rc != DBG_OK) return rc;

        if (resp_len < sizeof(dbg_field_list_response_t))
            return DBG_ERR_MALFORMED;

        const dbg_field_list_response_t *resp =
            (const dbg_field_list_response_t *)resp_data;

        if (resp->status != (int16_t)DBG_OK)
            return (dbg_status_t)resp->status;

        if (resp_len < dbg_field_list_response_size(resp->field_count))
            return DBG_ERR_MALFORMED;

        if (first) {
            total = resp->total_fields;
            first = 0;
        }

        if (resp->field_count > 0) {
            cb(resp->fields, resp->field_count, resp->total_fields, user_ctx);
        }

        offset += resp->field_count;

        if (offset >= total || resp->field_count == 0) break;
    }

    return DBG_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
   HEARTBEAT
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_sub_send_heartbeat(dbg_subscriber_t *sub)
{
    if (!sub) return DBG_ERR_INTERNAL;

    dbg_heartbeat_t hb;
    memset(&hb, 0, sizeof(hb));

    uint32_t tx_id = sub->next_tx_id++;
    dbg_header_init(&hb.header, DBG_MSG_HEARTBEAT, tx_id);
    hb.timestamp_us = dbg_get_time_us();

    for (uint32_t i = 0; i < DBG_PUB_MAX_SUBSCRIPTIONS; i++) {
        if (sub->subs[i].active) hb.active_subs++;
    }

    dbg_status_t rc = sub_send_config(sub, &hb, sizeof(hb));
    if (rc != DBG_OK) return rc;

    void    *resp_data = NULL;
    uint32_t resp_len  = 0;

    rc = sub_recv_config(sub, DBG_MSG_HEARTBEAT_ACK, tx_id,
                         &resp_data, &resp_len);

    if (rc == DBG_OK && resp_len >= sizeof(dbg_heartbeat_ack_t)) {
        const dbg_heartbeat_ack_t *ack =
            (const dbg_heartbeat_ack_t *)resp_data;

        uint64_t now    = dbg_get_time_us();
        float    rtt_ms = (float)(now - ack->timestamp_us) / 1000.0f;

        for (uint32_t i = 0; i < DBG_PUB_MAX_SUBSCRIPTIONS; i++) {
            if (sub->subs[i].active) {
                sub->subs[i].stats.rtt_ms = rtt_ms;
            }
        }
    }

    sub->last_heartbeat_sent_us = dbg_get_time_us();
    return DBG_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
   DISCONNECT CALLBACK
   ══════════════════════════════════════════════════════════════════════════ */

void dbg_sub_set_disconnect_cb(dbg_subscriber_t    *sub,
                                dbg_disconnect_cb_t  cb,
                                void                *user_ctx)
{
    if (!sub) return;
    sub->disconnect_cb  = cb;
    sub->disconnect_ctx = user_ctx;
}

/* ══════════════════════════════════════════════════════════════════════════
   STATISTICS
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_sub_get_stats(const dbg_subscriber_t *sub,
                               uint16_t                sub_id,
                               dbg_sub_stats_t        *stats)
{
    if (!sub || !stats) return DBG_ERR_INTERNAL;

    for (uint32_t i = 0; i < DBG_PUB_MAX_SUBSCRIPTIONS; i++) {
        if (sub->subs[i].active && sub->subs[i].sub_id == sub_id) {
            memcpy(stats, &sub->subs[i].stats, sizeof(*stats));
            return DBG_OK;
        }
    }

    return DBG_ERR_UNKNOWN_SUB;
}

/* ══════════════════════════════════════════════════════════════════════════
   INTERNAL — CONFIG SEND / RECEIVE
   ══════════════════════════════════════════════════════════════════════════ */

static dbg_status_t sub_send_config(dbg_subscriber_t *sub,
                                    const void *data, uint32_t len)
{
    return dbg_socket_sendto(&sub->config_sock, data, len,
                             &sub->server_config_addr);
}

static dbg_status_t sub_recv_config(dbg_subscriber_t *sub,
                                    uint16_t expected_msg_type,
                                    uint32_t expected_tx_id,
                                    void **out_data, uint32_t *out_len)
{
    /*
     * Retry loop: discard stale responses from prior transactions.
     * n == 0  → timeout (blocking socket with SO_RCVTIMEO fired)
     * n <  0  → hard socket error
     * n >  0  → data received; check if it matches our transaction
     */
    const int max_retries = 8;

    for (int r = 0; r < max_retries; r++) {
        int n = dbg_socket_recvfrom(&sub->config_sock,
                                    sub->rx_buf,
                                    sizeof(sub->rx_buf),
                                    NULL);
        if (n <= 0) return DBG_ERR_INTERNAL;  /* timeout or hard error */

        if ((uint32_t)n < sizeof(dbg_header_t)) continue;

        const dbg_header_t *hdr = (const dbg_header_t *)sub->rx_buf;
        if (dbg_header_validate(hdr) != DBG_OK)      continue;
        if (hdr->msg_type != expected_msg_type)       continue;
        if (hdr->tx_id    != expected_tx_id)          continue;

        *out_data = sub->rx_buf;
        *out_len  = (uint32_t)n;
        return DBG_OK;
    }

    return DBG_ERR_INTERNAL;  /* no matching response within retry budget */
}

/* ══════════════════════════════════════════════════════════════════════════
   INTERNAL — SUBSCRIPTION ENTRY MANAGEMENT
   ══════════════════════════════════════════════════════════════════════════ */

static sub_client_entry_t* sub_find_entry(dbg_subscriber_t *sub,
                                          uint16_t sub_id)
{
    for (uint32_t i = 0; i < DBG_PUB_MAX_SUBSCRIPTIONS; i++) {
        if (sub->subs[i].active && sub->subs[i].sub_id == sub_id) {
            return &sub->subs[i];
        }
    }
    return NULL;
}

static sub_client_entry_t* sub_alloc_entry(dbg_subscriber_t *sub)
{
    for (uint32_t i = 0; i < DBG_PUB_MAX_SUBSCRIPTIONS; i++) {
        if (!sub->subs[i].active) {
            memset(&sub->subs[i], 0, sizeof(sub->subs[i]));
            return &sub->subs[i];
        }
    }
    return NULL;
}

static void sub_free_entry(sub_client_entry_t *entry)
{
    if (!entry) return;
    memset(entry, 0, sizeof(*entry));  /* sets active = 0 */
}

/* ══════════════════════════════════════════════════════════════════════════
   INTERNAL — SEQUENCE TRACKING
   ══════════════════════════════════════════════════════════════════════════ */

static void sub_track_sequence(sub_client_entry_t *entry, uint32_t seq)
{
    if (!entry->sequence_initialised) {
        entry->last_sequence        = seq;
        entry->sequence_initialised = 1;
        return;
    }

    uint32_t expected = entry->last_sequence + 1;
    if (seq != expected) {
        /*
         * Compute unsigned forward distance to handle 32-bit wrap-around
         * (0xFFFFFFFF → 0).  Large gaps (> 10000) are treated as a
         * sequence reset rather than a massive drop.
         */
        uint32_t gap = seq - expected;
        if (gap > 10000u) {
            entry->stats.frames_dropped++;
        } else {
            entry->stats.frames_dropped += gap;
        }
    }

    entry->last_sequence = seq;
}

/* ══════════════════════════════════════════════════════════════════════════
   INTERNAL — HEARTBEAT MANAGEMENT
   ══════════════════════════════════════════════════════════════════════════ */

static void sub_check_heartbeat(dbg_subscriber_t *sub)
{
    if (sub->heartbeat_interval_ms == 0) return;

    uint64_t now         = dbg_get_time_us();
    uint64_t interval_us = (uint64_t)sub->heartbeat_interval_ms * 1000u;

    if ((now - sub->last_heartbeat_sent_us) < interval_us) return;

    /*
     * Temporarily lower the config socket timeout to 50 ms so the
     * heartbeat exchange doesn't stall the data poll path for a full
     * config_timeout_ms if the server is slow to respond.
     */
    uint32_t orig_timeout_ms = 0;
    dbg_socket_get_recv_timeout(&sub->config_sock, &orig_timeout_ms);
    dbg_socket_set_recv_timeout(&sub->config_sock, 50);

    dbg_status_t rc = dbg_sub_send_heartbeat(sub);

    dbg_socket_set_recv_timeout(&sub->config_sock, orig_timeout_ms);

    if (rc != DBG_OK && sub->disconnect_cb) {
        sub->disconnect_cb(rc, sub->disconnect_ctx);
    }
}
