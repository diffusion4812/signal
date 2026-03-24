/******************************************************************************
 * dbg_socket.h
 *
 * Platform-independent socket abstraction for the debug pub/sub system.
 *
 * Supports POSIX (Linux, macOS) and optionally Windows (define
 * DBG_PLATFORM_WINDOWS before including this header).
 *
 * (c) 2025 — Internal use only.
 *****************************************************************************/

#ifndef DBG_SOCKET_H
#define DBG_SOCKET_H

#include "dbg_protocol.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Socket handle ──────────────────────────────────────────────────────── */

typedef struct {
    int      fd;              /**< Platform socket descriptor.             */
    int      bound;           /**< 1 if bind() has been called.            */
    int      nonblocking;     /**< 1 if in non-blocking mode.              */
} dbg_socket_t;

/** Static initialiser for dbg_socket_t. */
#define DBG_SOCKET_INIT  { -1, 0, 0 }

/* ── Address ────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t  ip;             /**< IPv4 address in network byte order.     */
    uint16_t  port;           /**< Port in host byte order.                */
} dbg_addr_t;

/* ══════════════════════════════════════════════════════════════════════════
   GLOBAL INIT / SHUTDOWN
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Initialise the socket subsystem.
 *
 * On Windows, calls WSAStartup().  On POSIX, this is a no-op.
 * Safe to call multiple times.
 *
 * @return DBG_OK on success.
 */
dbg_status_t dbg_socket_init(void);

/**
 * @brief  Shut down the socket subsystem.
 */
void dbg_socket_shutdown(void);

/* ══════════════════════════════════════════════════════════════════════════
   SOCKET LIFECYCLE
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Create a UDP socket.
 *
 * @param  out  Output socket handle.
 * @return DBG_OK on success.
 */
dbg_status_t dbg_socket_open(dbg_socket_t *out);

/**
 * @brief  Close a socket and reset the handle.
 *
 * @param  sock  Socket handle (may be NULL or already closed — no-op).
 */
void dbg_socket_close(dbg_socket_t *sock);

/* ══════════════════════════════════════════════════════════════════════════
   BINDING
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Bind a socket to a specific address and port.
 *
 * @param  sock       Socket handle.
 * @param  bind_addr  IP string (e.g. "0.0.0.0").  NULL = INADDR_ANY.
 * @param  port       Port number.  0 = OS-assigned.
 * @return DBG_OK on success.
 */
dbg_status_t dbg_socket_bind(dbg_socket_t *sock,
                             const char   *bind_addr,
                             uint16_t      port);

/**
 * @brief  Bind to any address on an OS-assigned port.
 */
dbg_status_t dbg_socket_bind_any(dbg_socket_t *sock);

/* ══════════════════════════════════════════════════════════════════════════
   OPTIONS
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Set non-blocking mode.
 *
 * @param  sock    Socket handle.
 * @param  enable  1 = non-blocking, 0 = blocking.
 * @return DBG_OK on success.
 */
dbg_status_t dbg_socket_set_nonblocking(dbg_socket_t *sock, int enable);

/**
 * @brief  Set the send buffer size.
 */
dbg_status_t dbg_socket_set_send_bufsize(dbg_socket_t *sock, int size_bytes);

/**
 * @brief  Set the receive buffer size.
 */
dbg_status_t dbg_socket_set_recv_bufsize(dbg_socket_t *sock, int size_bytes);

/**
 * @brief  Set receive timeout for blocking operations.
 *
 * @param  sock        Socket handle.
 * @param  timeout_ms  Timeout in milliseconds.  0 = no timeout.
 * @return DBG_OK on success.
 */
dbg_status_t dbg_socket_set_recv_timeout(dbg_socket_t *sock,
                                         uint32_t timeout_ms);

/**
 * @brief  Get current receive timeout.
 *
 * @param  sock            Socket handle.
 * @param  out_timeout_ms  Output: timeout in milliseconds.
 * @return DBG_OK on success.
 */
dbg_status_t dbg_socket_get_recv_timeout(dbg_socket_t *sock,
                                         uint32_t *out_timeout_ms);

/* ══════════════════════════════════════════════════════════════════════════
   SEND / RECEIVE
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Send data to a destination address.
 *
 * @param  sock  Socket handle.
 * @param  data  Data buffer.
 * @param  len   Number of bytes to send.
 * @param  dest  Destination address.
 * @return DBG_OK on success.
 */
dbg_status_t dbg_socket_sendto(dbg_socket_t     *sock,
                               const void       *data,
                               uint32_t          len,
                               const dbg_addr_t *dest);

/**
 * @brief  Receive data from any source.
 *
 * @param  sock      Socket handle.
 * @param  buf       Receive buffer.
 * @param  buf_size  Buffer capacity.
 * @param  from      (Optional) output: source address.
 * @return Number of bytes received (> 0), 0 if no data available
 *         (non-blocking / timeout), or -1 on error.
 */
int dbg_socket_recvfrom(dbg_socket_t *sock,
                        void         *buf,
                        uint32_t      buf_size,
                        dbg_addr_t   *from);

/**
 * @brief  Poll a socket for readability.
 *
 * @param  sock        Socket handle.
 * @param  timeout_ms  Maximum wait time in milliseconds.
 * @return 1 = readable, 0 = timeout, -1 = error.
 */
int dbg_socket_poll_readable(dbg_socket_t *sock, uint32_t timeout_ms);

/* ══════════════════════════════════════════════════════════════════════════
   ADDRESS UTILITIES
   ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Resolve a hostname or IP string to a dbg_addr_t.
 *
 * @param  host  IP string or hostname.
 * @param  port  Port number (host byte order).
 * @param  out   Output address.
 * @return DBG_OK on success.
 */
dbg_status_t dbg_addr_from_string(const char *host,
                                  uint16_t    port,
                                  dbg_addr_t *out);

/**
 * @brief  Format a dbg_addr_t as "ip:port" string.
 *
 * @param  addr      Address to format.
 * @param  buf       Output buffer.
 * @param  buf_size  Buffer capacity.
 * @return DBG_OK on success.
 */
dbg_status_t dbg_addr_to_string(const dbg_addr_t *addr,
                                char             *buf,
                                uint32_t          buf_size);

/**
 * @brief  Compare two addresses for equality (ip + port).
 *
 * @return 1 if equal, 0 if not.
 */
int dbg_addr_equal(const dbg_addr_t *a, const dbg_addr_t *b);

/**
 * @brief  Get the local port assigned to a bound socket.
 *
 * Useful after dbg_socket_bind_any() to learn the assigned port.
 *
 * @param  sock      Socket handle.
 * @param  out_port  Output: port in host byte order.
 * @return DBG_OK on success.
 */
dbg_status_t dbg_socket_get_local_port(dbg_socket_t *sock,
                                       uint16_t     *out_port);

#ifdef __cplusplus
}
#endif

#endif /* DBG_SOCKET_H */