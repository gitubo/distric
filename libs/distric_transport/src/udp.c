/**
 * @file udp.c
 * @brief UDP Socket Implementation
 * 
 * Non-blocking UDP socket with integrated observability.
 * Uses ONLY the public distric_obs API.
 */

#include "distric_transport/udp.h"
#include <distric_obs.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

struct udp_socket_s {
    int fd;
    uint16_t port;
    char bind_addr[256];
    
    metrics_registry_t* metrics;
    logger_t* logger;
    
    metric_t* packets_sent_metric;
    metric_t* packets_recv_metric;
    metric_t* bytes_sent_metric;
    metric_t* bytes_recv_metric;
    metric_t* errors_metric;
};

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ============================================================================
 * UDP SOCKET IMPLEMENTATION
 * ========================================================================= */

distric_err_t udp_socket_create(
    const char* bind_addr,
    uint16_t port,
    metrics_registry_t* metrics,
    logger_t* logger,
    udp_socket_t** socket
) {
    if (!bind_addr || !socket) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    udp_socket_t* sock = calloc(1, sizeof(udp_socket_t));
    if (!sock) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    strncpy(sock->bind_addr, bind_addr, sizeof(sock->bind_addr) - 1);
    sock->port = port;
    sock->metrics = metrics;
    sock->logger = logger;
    
    /* Create socket */
    sock->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock->fd < 0) {
        free(sock);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    set_nonblocking(sock->fd);
    
    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, bind_addr, &addr.sin_addr);
    
    if (bind(sock->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock->fd);
        free(sock);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Register metrics */
    if (metrics) {
        metrics_register_counter(metrics, "udp_packets_sent_total",
                                "Total UDP packets sent", NULL, 0, &sock->packets_sent_metric);
        metrics_register_counter(metrics, "udp_packets_received_total",
                                "Total UDP packets received", NULL, 0, &sock->packets_recv_metric);
        metrics_register_counter(metrics, "udp_bytes_sent_total",
                                "Total UDP bytes sent", NULL, 0, &sock->bytes_sent_metric);
        metrics_register_counter(metrics, "udp_bytes_received_total",
                                "Total UDP bytes received", NULL, 0, &sock->bytes_recv_metric);
        metrics_register_counter(metrics, "udp_errors_total",
                                "Total UDP errors", NULL, 0, &sock->errors_metric);
    }
    
    if (logger) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        
        LOG_INFO(logger, "udp", "UDP socket created",
                "bind_addr", bind_addr,
                "port", port_str);
    }
    
    *socket = sock;
    return DISTRIC_OK;
}

int udp_send(
    udp_socket_t* socket,
    const void* data,
    size_t len,
    const char* dest_addr,
    uint16_t dest_port
) {
    if (!socket || !data || len == 0 || !dest_addr) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Check datagram size (UDP limit is 65507 bytes) */
    if (len > 65507) {
        if (socket->logger) {
            LOG_WARN(socket->logger, "udp", "Datagram too large",
                    "size", "exceeds 65507 bytes");
        }
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Resolve destination */
    struct hostent* he = gethostbyname(dest_addr);
    if (!he) {
        if (socket->errors_metric) {
            metrics_counter_inc(socket->errors_metric);
        }
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dest_port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    
    /* Send datagram */
    ssize_t sent = sendto(socket->fd, data, len, 0,
                         (struct sockaddr*)&addr, sizeof(addr));
    
    if (sent > 0) {
        if (socket->packets_sent_metric) {
            metrics_counter_inc(socket->packets_sent_metric);
        }
        if (socket->bytes_sent_metric) {
            metrics_counter_add(socket->bytes_sent_metric, sent);
        }
        
        if (socket->logger) {
            char port_str[16];
            snprintf(port_str, sizeof(port_str), "%u", dest_port);
            char bytes_str[32];
            snprintf(bytes_str, sizeof(bytes_str), "%zd", sent);
            
            LOG_DEBUG(socket->logger, "udp", "Datagram sent",
                     "dest_addr", dest_addr,
                     "dest_port", port_str,
                     "bytes", bytes_str);
        }
    } else {
        if (socket->errors_metric) {
            metrics_counter_inc(socket->errors_metric);
        }
        
        if (socket->logger) {
            LOG_ERROR(socket->logger, "udp", "Send failed",
                     "error", strerror(errno));
        }
    }
    
    return (int)sent;
}

int udp_recv(
    udp_socket_t* socket,
    void* buffer,
    size_t len,
    char* src_addr,
    uint16_t* src_port,
    int timeout_ms
) {
    if (!socket || !buffer || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Handle timeout with select */
    if (timeout_ms >= 0) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket->fd, &read_fds);
        
        struct timeval tv;
        struct timeval* tv_ptr = NULL;
        
        if (timeout_ms > 0) {
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            tv_ptr = &tv;
        }
        
        int ready = select(socket->fd + 1, &read_fds, NULL, NULL, tv_ptr);
        
        if (ready == 0) {
            /* Timeout */
            return 0;
        } else if (ready < 0) {
            if (socket->errors_metric) {
                metrics_counter_inc(socket->errors_metric);
            }
            return ready;
        }
    }
    
    /* Receive datagram */
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    
    ssize_t received = recvfrom(socket->fd, buffer, len, 0,
                               (struct sockaddr*)&addr, &addr_len);
    
    if (received > 0) {
        if (socket->packets_recv_metric) {
            metrics_counter_inc(socket->packets_recv_metric);
        }
        if (socket->bytes_recv_metric) {
            metrics_counter_add(socket->bytes_recv_metric, received);
        }
        
        /* Extract source address */
        if (src_addr) {
            inet_ntop(AF_INET, &addr.sin_addr, src_addr, 256);
        }
        if (src_port) {
            *src_port = ntohs(addr.sin_port);
        }
        
        if (socket->logger) {
            char src_addr_str[256];
            inet_ntop(AF_INET, &addr.sin_addr, src_addr_str, sizeof(src_addr_str));
            char port_str[16];
            snprintf(port_str, sizeof(port_str), "%u", ntohs(addr.sin_port));
            char bytes_str[32];
            snprintf(bytes_str, sizeof(bytes_str), "%zd", received);
            
            LOG_DEBUG(socket->logger, "udp", "Datagram received",
                     "src_addr", src_addr_str,
                     "src_port", port_str,
                     "bytes", bytes_str);
        }
    } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        if (socket->errors_metric) {
            metrics_counter_inc(socket->errors_metric);
        }
        
        if (socket->logger) {
            LOG_ERROR(socket->logger, "udp", "Receive failed",
                     "error", strerror(errno));
        }
    }
    
    return (int)received;
}

void udp_close(udp_socket_t* socket) {
    if (!socket) return;
    
    if (socket->logger) {
        LOG_INFO(socket->logger, "udp", "UDP socket closed");
    }
    
    close(socket->fd);
    free(socket);
}