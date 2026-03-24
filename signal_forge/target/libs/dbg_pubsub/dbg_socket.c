/******************************************************************************
 * dbg_socket.c
 *
 * Platform socket abstraction layer for the debug pub/sub system.
 *
 * Provides a clean API over POSIX sockets with optional compile-time
 * support for Windows (Winsock2) via DBG_PLATFORM_WINDOWS.
 *
 * (c) 2025 — Internal use only.
 *****************************************************************************/

/* Request POSIX.1-2008 extensions (struct timeval, struct addrinfo,
   getaddrinfo, freeaddrinfo, etc.) when compiling under strict C99.
   Must appear before any system header is included. */
#ifndef DBG_PLATFORM_WINDOWS
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "dbg_socket.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

/* ── Platform includes ──────────────────────────────────────────────────── */
#ifdef DBG_PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")

    typedef int socklen_t;
    typedef int ssize_t;

    #define DBG_SOCK_INVALID  INVALID_SOCKET
    #define DBG_SOCK_ERROR    SOCKET_ERROR
    #define DBG_CLOSE_SOCKET  closesocket
    #define DBG_ERRNO         WSAGetLastError()
    #define DBG_EAGAIN        WSAEWOULDBLOCK
#else
    /* POSIX */
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/time.h>       /* struct timeval (SO_RCVTIMEO / SO_SNDTIMEO) */
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <poll.h>

    #define DBG_SOCK_INVALID  (-1)
    #define DBG_SOCK_ERROR    (-1)
    #define DBG_CLOSE_SOCKET  close
    #define DBG_ERRNO         errno
    #define DBG_EAGAIN        EAGAIN
#endif

/* ══════════════════════════════════════════════════════════════════════════
   GLOBAL INITIALISATION / SHUTDOWN
   ══════════════════════════════════════════════════════════════════════════ */

static int g_socket_initialised = 0;

dbg_status_t dbg_socket_init(void)
{
    if (g_socket_initialised) return DBG_OK;

#ifdef DBG_PLATFORM_WINDOWS
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) return DBG_ERR_INTERNAL;
#endif

    g_socket_initialised = 1;
    return DBG_OK;
}

void dbg_socket_shutdown(void)
{
    if (!g_socket_initialised) return;

#ifdef DBG_PLATFORM_WINDOWS
    WSACleanup();
#endif

    g_socket_initialised = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   SOCKET CREATION
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_socket_open(dbg_socket_t *out)
{
    if (!out) return DBG_ERR_INTERNAL;

    dbg_socket_init();  /* ensure platform is ready */

#ifdef DBG_PLATFORM_WINDOWS
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) {
        out->fd = -1;
        return DBG_ERR_INTERNAL;
    }
    out->fd = (int)fd;
#else
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        out->fd = -1;
        return DBG_ERR_INTERNAL;
    }
    out->fd = fd;
#endif

    /* Allow address reuse */
    int opt = 1;
    setsockopt(out->fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    out->bound      = 0;
    out->nonblocking = 0;

    return DBG_OK;
}

void dbg_socket_close(dbg_socket_t *sock)
{
    if (!sock || sock->fd < 0) return;

#ifdef DBG_PLATFORM_WINDOWS
    closesocket((SOCKET)sock->fd);
#else
    close(sock->fd);
#endif

    sock->fd         = -1;
    sock->bound      = 0;
    sock->nonblocking = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   BINDING
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_socket_bind(dbg_socket_t *sock,
                             const char   *bind_addr,
                             uint16_t      port)
{
    if (!sock || sock->fd < 0) return DBG_ERR_INTERNAL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (!bind_addr || strcmp(bind_addr, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
            return DBG_ERR_INTERNAL;
        }
    }

    if (bind(sock->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        return DBG_ERR_INTERNAL;
    }

    sock->bound = 1;
    return DBG_OK;
}

dbg_status_t dbg_socket_bind_any(dbg_socket_t *sock)
{
    return dbg_socket_bind(sock, "0.0.0.0", 0);
}

/* ══════════════════════════════════════════════════════════════════════════
   NON-BLOCKING MODE
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_socket_set_nonblocking(dbg_socket_t *sock, int enable)
{
    if (!sock || sock->fd < 0) return DBG_ERR_INTERNAL;

#ifdef DBG_PLATFORM_WINDOWS
    u_long mode = enable ? 1 : 0;
    if (ioctlsocket((SOCKET)sock->fd, FIONBIO, &mode) != 0) {
        return DBG_ERR_INTERNAL;
    }
#else
    int flags = fcntl(sock->fd, F_GETFL, 0);
    if (flags < 0) return DBG_ERR_INTERNAL;

    if (enable) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }

    if (fcntl(sock->fd, F_SETFL, flags) < 0) {
        return DBG_ERR_INTERNAL;
    }
#endif

    sock->nonblocking = enable ? 1 : 0;
    return DBG_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
   SOCKET OPTIONS
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_socket_set_send_bufsize(dbg_socket_t *sock, int size_bytes)
{
    if (!sock || sock->fd < 0) return DBG_ERR_INTERNAL;

    if (setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF,
                   (const char *)&size_bytes, sizeof(size_bytes)) < 0) {
        return DBG_ERR_INTERNAL;
    }
    return DBG_OK;
}

dbg_status_t dbg_socket_set_recv_bufsize(dbg_socket_t *sock, int size_bytes)
{
    if (!sock || sock->fd < 0) return DBG_ERR_INTERNAL;

    if (setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF,
                   (const char *)&size_bytes, sizeof(size_bytes)) < 0) {
        return DBG_ERR_INTERNAL;
    }
    return DBG_OK;
}

dbg_status_t dbg_socket_set_recv_timeout(dbg_socket_t *sock,
                                         uint32_t timeout_ms)
{
    if (!sock || sock->fd < 0) return DBG_ERR_INTERNAL;

#ifdef DBG_PLATFORM_WINDOWS
    DWORD tv = (DWORD)timeout_ms;
    if (setsockopt((SOCKET)sock->fd, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&tv, sizeof(tv)) < 0) {
        return DBG_ERR_INTERNAL;
    }
#else
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000u;
    tv.tv_usec = (timeout_ms % 1000u) * 1000u;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO,
                   &tv, sizeof(tv)) < 0) {
        return DBG_ERR_INTERNAL;
    }
#endif

    return DBG_OK;
}

dbg_status_t dbg_socket_get_recv_timeout(dbg_socket_t *sock,
                                         uint32_t *out_timeout_ms)
{
    if (!sock || sock->fd < 0 || !out_timeout_ms) return DBG_ERR_INTERNAL;

#ifdef DBG_PLATFORM_WINDOWS
    DWORD tv = 0;
    int optlen = sizeof(tv);
    if (getsockopt((SOCKET)sock->fd, SOL_SOCKET, SO_RCVTIMEO,
                   (char *)&tv, &optlen) < 0) {
        return DBG_ERR_INTERNAL;
    }
    *out_timeout_ms = (uint32_t)tv;
#else
    struct timeval tv;
    socklen_t optlen = sizeof(tv);
    if (getsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO,
                   &tv, &optlen) < 0) {
        return DBG_ERR_INTERNAL;
    }
    *out_timeout_ms = (uint32_t)(tv.tv_sec * 1000u + tv.tv_usec / 1000u);
#endif

    return DBG_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
   SEND / RECEIVE
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_socket_sendto(dbg_socket_t       *sock,
                               const void         *data,
                               uint32_t            len,
                               const dbg_addr_t   *dest)
{
    if (!sock || sock->fd < 0 || !data || !dest) return DBG_ERR_INTERNAL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(dest->port);
    addr.sin_addr.s_addr = dest->ip;

    ssize_t sent = sendto(sock->fd, (const char *)data, len, 0,
                          (struct sockaddr *)&addr, sizeof(addr));

    if (sent != (ssize_t)len) return DBG_ERR_INTERNAL;

    return DBG_OK;
}

int dbg_socket_recvfrom(dbg_socket_t *sock,
                        void         *buf,
                        uint32_t      buf_size,
                        dbg_addr_t   *from)
{
    if (!sock || sock->fd < 0 || !buf) return -1;

    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);
    memset(&src_addr, 0, sizeof(src_addr));

    ssize_t n = recvfrom(sock->fd, (char *)buf, buf_size, 0,
                         (struct sockaddr *)&src_addr, &addr_len);

    if (n <= 0) {
#ifdef DBG_PLATFORM_WINDOWS
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT) return 0;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
#endif
        return -1;  /* real error */
    }

    if (from) {
        from->ip   = src_addr.sin_addr.s_addr;
        from->port = ntohs(src_addr.sin_port);
    }

    return (int)n;
}

/* ══════════════════════════════════════════════════════════════════════════
   POLL
   ══════════════════════════════════════════════════════════════════════════ */

int dbg_socket_poll_readable(dbg_socket_t *sock, uint32_t timeout_ms)
{
    if (!sock || sock->fd < 0) return -1;

#ifdef DBG_PLATFORM_WINDOWS
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET((SOCKET)sock->fd, &read_set);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000u;
    tv.tv_usec = (timeout_ms % 1000u) * 1000u;

    int rc = select(0, &read_set, NULL, NULL, &tv);
    if (rc > 0) return 1;
    if (rc == 0) return 0;
    return -1;
#else
    struct pollfd pfd;
    pfd.fd      = sock->fd;
    pfd.events  = POLLIN;
    pfd.revents = 0;

    int rc = poll(&pfd, 1, (int)timeout_ms);
    if (rc > 0 && (pfd.revents & POLLIN)) return 1;
    if (rc == 0) return 0;
    return -1;
#endif
}

/* ══════════════════════════════════════════════════════════════════════════
   ADDRESS UTILITIES
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_addr_from_string(const char *host,
                                  uint16_t    port,
                                  dbg_addr_t *out)
{
    if (!host || !out) return DBG_ERR_INTERNAL;

    memset(out, 0, sizeof(*out));
    out->port = port;

    /* Try direct IP parse first */
    struct in_addr in;
    if (inet_pton(AF_INET, host, &in) == 1) {
        out->ip = in.s_addr;
        return DBG_OK;
    }

    /* DNS resolution */
    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int rc = getaddrinfo(host, NULL, &hints, &result);
    if (rc != 0 || !result) return DBG_ERR_INTERNAL;

    struct sockaddr_in *resolved = (struct sockaddr_in *)result->ai_addr;
    out->ip = resolved->sin_addr.s_addr;

    freeaddrinfo(result);
    return DBG_OK;
}

dbg_status_t dbg_addr_to_string(const dbg_addr_t *addr,
                                char             *buf,
                                uint32_t          buf_size)
{
    if (!addr || !buf || buf_size == 0) return DBG_ERR_INTERNAL;

    struct in_addr in;
    in.s_addr = addr->ip;

    const char *str = inet_ntop(AF_INET, &in, buf, buf_size);
    if (!str) return DBG_ERR_INTERNAL;

    /* Append :port if buffer has room */
    size_t len = strlen(buf);
    if (len + 8 < buf_size) {
        snprintf(buf + len, buf_size - len, ":%u", addr->port);
    }

    return DBG_OK;
}

int dbg_addr_equal(const dbg_addr_t *a, const dbg_addr_t *b)
{
    if (!a || !b) return 0;
    return (a->ip == b->ip) && (a->port == b->port);
}

/* ══════════════════════════════════════════════════════════════════════════
   LOCAL PORT QUERY
   ══════════════════════════════════════════════════════════════════════════ */

dbg_status_t dbg_socket_get_local_port(dbg_socket_t *sock,
                                       uint16_t     *out_port)
{
    if (!sock || sock->fd < 0 || !out_port) return DBG_ERR_INTERNAL;

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    if (getsockname(sock->fd, (struct sockaddr *)&addr, &addr_len) < 0) {
        return DBG_ERR_INTERNAL;
    }

    *out_port = ntohs(addr.sin_port);
    return DBG_OK;
}