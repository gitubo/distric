/**
 * @file tcp.c
 * @brief TCP Transport Implementation
 * 
 * Non-blocking TCP server and client with integrated observability.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_transport/tcp.h"
#include <distric_obs.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

/* Global connection ID counter for client connections */
static _Atomic uint64_t g_client_connection_id_counter = 1;

struct tcp_connection_s {
    int fd;
    char remote_addr[256];
    uint16_t remote_port;
    uint64_t connection_id;
    
    metrics_registry_t* metrics;
    logger_t* logger;
    
    metric_t* bytes_sent_metric;
    metric_t* bytes_recv_metric;
};

struct tcp_server_s {
    int listen_fd;
    char bind_addr[256];
    uint16_t port;
    
    _Atomic bool running;
    pthread_t accept_thread;
    
    tcp_connection_callback_t callback;
    void* userdata;
    
    metrics_registry_t* metrics;
    logger_t* logger;
    
    metric_t* connections_total_metric;
    metric_t* active_connections_metric;
    metric_t* errors_metric;
    
    _Atomic uint64_t connection_id_counter;
};

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_tcp_nodelay(int fd) {
    int flag = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

static int set_reuseaddr(int fd) {
    int flag = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
}

/* ============================================================================
 * TCP CONNECTION IMPLEMENTATION
 * ========================================================================= */

distric_err_t tcp_connect(
    const char* host,
    uint16_t port,
    int timeout_ms,
    metrics_registry_t* metrics,
    logger_t* logger,
    tcp_connection_t** conn_out
) {
    if (!host || !conn_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Resolve hostname */
    struct hostent* he = gethostbyname(host);
    if (!he) {
        if (logger) {
            LOG_ERROR(logger, "tcp", "Failed to resolve host", "host", host, NULL);
        }
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Create socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (logger) {
            LOG_ERROR(logger, "tcp", "Failed to create socket", "errno", strerror(errno), NULL);
        }
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    set_nonblocking(fd);
    set_tcp_nodelay(fd);
    
    /* Connect */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    
    int result = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    
    if (result < 0 && errno != EINPROGRESS) {
        if (logger) {
            LOG_ERROR(logger, "tcp", "Connect failed immediately", 
                     "host", host, "errno", strerror(errno), NULL);
        }
        close(fd);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Wait for connection with timeout */
    if (errno == EINPROGRESS) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        
        int poll_result = poll(&pfd, 1, timeout_ms);
        
        if (poll_result <= 0) {
            if (logger) {
                LOG_ERROR(logger, "tcp", "Connect timeout", "host", host, NULL);
            }
            close(fd);
            return DISTRIC_ERR_TIMEOUT;
        }
        
        /* Check for connection error */
        int error;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
            if (logger) {
                LOG_ERROR(logger, "tcp", "Connect failed", 
                         "host", host, "error", strerror(error), NULL);
            }
            close(fd);
            return DISTRIC_ERR_INIT_FAILED;
        }
    }
    
    /* Create connection object */
    tcp_connection_t* conn = calloc(1, sizeof(tcp_connection_t));
    if (!conn) {
        close(fd);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    conn->fd = fd;
    inet_ntop(AF_INET, &addr.sin_addr, conn->remote_addr, sizeof(conn->remote_addr));
    conn->remote_port = port;
    conn->connection_id = atomic_fetch_add(&g_client_connection_id_counter, 1);
    conn->metrics = metrics;
    conn->logger = logger;
    
    /* Register metrics */
    if (metrics) {
        metrics_register_counter(metrics, "tcp_bytes_sent_total",
                                "Total TCP bytes sent", NULL, 0, &conn->bytes_sent_metric);
        metrics_register_counter(metrics, "tcp_bytes_received_total",
                                "Total TCP bytes received", NULL, 0, &conn->bytes_recv_metric);
    }
    
    if (logger) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        LOG_DEBUG(logger, "tcp", "Connected", 
                 "host", host, "port", port_str, NULL);
    }
    
    *conn_out = conn;
    return DISTRIC_OK;
}

int tcp_send(tcp_connection_t* conn, const void* data, size_t len) {
    if (!conn || !data || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    ssize_t total_sent = 0;
    const uint8_t* ptr = (const uint8_t*)data;
    
    while (total_sent < (ssize_t)len) {
        ssize_t sent = send(conn->fd, ptr + total_sent, len - total_sent, MSG_NOSIGNAL);
        
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Wait for socket to be writable */
                struct pollfd pfd;
                pfd.fd = conn->fd;
                pfd.events = POLLOUT;
                
                int poll_result = poll(&pfd, 1, 5000); /* 5 second timeout */
                if (poll_result <= 0) {
                    if (conn->logger) {
                        LOG_ERROR(conn->logger, "tcp", "Send timeout", NULL);
                    }
                    return -1;
                }
                continue;
            }
            
            if (conn->logger) {
                LOG_ERROR(conn->logger, "tcp", "Send failed", 
                         "errno", strerror(errno), NULL);
            }
            return -1;
        }
        
        total_sent += sent;
    }
    
    if (conn->bytes_sent_metric) {
        metrics_counter_add(conn->bytes_sent_metric, total_sent);
    }
    
    return (int)total_sent;
}

int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms) {
    if (!conn || !buffer || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Wait for data with timeout */
    if (timeout_ms >= 0) {
        struct pollfd pfd;
        pfd.fd = conn->fd;
        pfd.events = POLLIN;
        
        int poll_result = poll(&pfd, 1, timeout_ms);
        
        if (poll_result == 0) {
            /* Timeout */
            return 0;
        } else if (poll_result < 0) {
            if (conn->logger) {
                LOG_ERROR(conn->logger, "tcp", "Poll failed", 
                         "errno", strerror(errno), NULL);
            }
            return -1;
        }
    }
    
    ssize_t received = recv(conn->fd, buffer, len, 0);
    
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        
        if (conn->logger) {
            LOG_ERROR(conn->logger, "tcp", "Recv failed", 
                     "errno", strerror(errno), NULL);
        }
        return -1;
    }
    
    if (received > 0 && conn->bytes_recv_metric) {
        metrics_counter_add(conn->bytes_recv_metric, received);
    }
    
    return (int)received;
}

void tcp_close(tcp_connection_t* conn) {
    if (!conn) return;
    
    if (conn->logger) {
        LOG_DEBUG(conn->logger, "tcp", "Connection closed", 
                 "remote", conn->remote_addr, NULL);
    }
    
    close(conn->fd);
    free(conn);
}

distric_err_t tcp_get_remote_addr(
    tcp_connection_t* conn,
    char* addr_out,
    size_t addr_len,
    uint16_t* port_out
) {
    if (!conn || !addr_out || addr_len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    strncpy(addr_out, conn->remote_addr, addr_len - 1);
    addr_out[addr_len - 1] = '\0';
    
    if (port_out) {
        *port_out = conn->remote_port;
    }
    
    return DISTRIC_OK;
}

uint64_t tcp_get_connection_id(tcp_connection_t* conn) {
    return conn ? conn->connection_id : 0;
}

/* ============================================================================
 * TCP SERVER IMPLEMENTATION
 * ========================================================================= */

/* Forward declaration */
static void* lambda_wrapper(void* arg);

static void* accept_thread_func(void* arg) {
    tcp_server_t* server = (tcp_server_t*)arg;
    
    while (atomic_load(&server->running)) {
        /* Wait for incoming connection */
        struct pollfd pfd;
        pfd.fd = server->listen_fd;
        pfd.events = POLLIN;
        
        int poll_result = poll(&pfd, 1, 100); /* 100ms timeout */
        
        if (poll_result <= 0) {
            continue;
        }
        
        /* Accept connection */
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(server->listen_fd, (struct sockaddr*)&client_addr, &addr_len);
        
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            
            if (server->logger) {
                LOG_ERROR(server->logger, "tcp_server", "Accept failed", 
                         "errno", strerror(errno), NULL);
            }
            
            if (server->errors_metric) {
                metrics_counter_inc(server->errors_metric);
            }
            continue;
        }
        
        set_nonblocking(client_fd);
        set_tcp_nodelay(client_fd);
        
        /* Create connection object */
        tcp_connection_t* conn = calloc(1, sizeof(tcp_connection_t));
        if (!conn) {
            close(client_fd);
            continue;
        }
        
        conn->fd = client_fd;
        inet_ntop(AF_INET, &client_addr.sin_addr, conn->remote_addr, sizeof(conn->remote_addr));
        conn->remote_port = ntohs(client_addr.sin_port);
        conn->connection_id = atomic_fetch_add(&server->connection_id_counter, 1);
        conn->metrics = server->metrics;
        conn->logger = server->logger;
        
        /* Register metrics */
        if (server->metrics) {
            metrics_register_counter(server->metrics, "tcp_bytes_sent_total",
                                    "Total TCP bytes sent", NULL, 0, &conn->bytes_sent_metric);
            metrics_register_counter(server->metrics, "tcp_bytes_received_total",
                                    "Total TCP bytes received", NULL, 0, &conn->bytes_recv_metric);
        }
        
        /* Update server metrics */
        if (server->connections_total_metric) {
            metrics_counter_inc(server->connections_total_metric);
        }
        if (server->active_connections_metric) {
            metrics_gauge_set(server->active_connections_metric, 1.0); // Simplified
        }
        
        if (server->logger) {
            char port_str[16];
            snprintf(port_str, sizeof(port_str), "%u", conn->remote_port);
            LOG_INFO(server->logger, "tcp_server", "Connection accepted", 
                    "remote_addr", conn->remote_addr,
                    "remote_port", port_str, NULL);
        }
        
        /* Call user callback in new thread */
        if (server->callback) {
            pthread_t handler_thread;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            
            /* Create handler args */
            typedef struct {
                tcp_connection_t* conn;
                tcp_connection_callback_t callback;
                void* userdata;
            } handler_args_t;
            
            handler_args_t* args = malloc(sizeof(handler_args_t));
            if (args) {
                args->conn = conn;
                args->callback = server->callback;
                args->userdata = server->userdata;
                
                pthread_create(&handler_thread, &attr, 
                    (void*(*)(void*))lambda_wrapper, args);
            }
            
            pthread_attr_destroy(&attr);
        } else {
            tcp_close(conn);
        }
    }
    
    return NULL;
}

/* Thread wrapper for callback */
static void* lambda_wrapper(void* arg) {
    typedef struct {
        tcp_connection_t* conn;
        tcp_connection_callback_t callback;
        void* userdata;
    } handler_args_t;
    
    handler_args_t* args = (handler_args_t*)arg;
    
    args->callback(args->conn, args->userdata);
    
    free(args);
    return NULL;
}

distric_err_t tcp_server_create(
    const char* bind_addr,
    uint16_t port,
    metrics_registry_t* metrics,
    logger_t* logger,
    tcp_server_t** server_out
) {
    if (!bind_addr || !server_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    tcp_server_t* server = calloc(1, sizeof(tcp_server_t));
    if (!server) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    strncpy(server->bind_addr, bind_addr, sizeof(server->bind_addr) - 1);
    server->port = port;
    server->metrics = metrics;
    server->logger = logger;
    atomic_init(&server->running, false);
    atomic_init(&server->connection_id_counter, 1);
    
    /* Create listening socket */
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        if (logger) {
            LOG_ERROR(logger, "tcp_server", "Failed to create socket", 
                     "errno", strerror(errno), NULL);
        }
        free(server);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    set_nonblocking(server->listen_fd);
    set_reuseaddr(server->listen_fd);
    
    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, bind_addr, &addr.sin_addr);
    
    if (bind(server->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (logger) {
            LOG_ERROR(logger, "tcp_server", "Bind failed", 
                     "bind_addr", bind_addr, "errno", strerror(errno), NULL);
        }
        close(server->listen_fd);
        free(server);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Listen */
    if (listen(server->listen_fd, 128) < 0) {
        if (logger) {
            LOG_ERROR(logger, "tcp_server", "Listen failed", 
                     "errno", strerror(errno), NULL);
        }
        close(server->listen_fd);
        free(server);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Register metrics */
    if (metrics) {
        metrics_register_counter(metrics, "tcp_server_connections_total",
                                "Total TCP server connections", NULL, 0,
                                &server->connections_total_metric);
        metrics_register_gauge(metrics, "tcp_server_active_connections",
                              "Active TCP server connections", NULL, 0,
                              &server->active_connections_metric);
        metrics_register_counter(metrics, "tcp_server_errors_total",
                                "Total TCP server errors", NULL, 0,
                                &server->errors_metric);
    }
    
    if (logger) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        LOG_INFO(logger, "tcp_server", "Server created", 
                "bind_addr", bind_addr, "port", port_str, NULL);
    }
    
    *server_out = server;
    return DISTRIC_OK;
}

distric_err_t tcp_server_start(
    tcp_server_t* server,
    tcp_connection_callback_t callback,
    void* userdata
) {
    if (!server || !callback) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    server->callback = callback;
    server->userdata = userdata;
    
    atomic_store(&server->running, true);
    
    /* Start accept thread */
    if (pthread_create(&server->accept_thread, NULL, accept_thread_func, server) != 0) {
        if (server->logger) {
            LOG_ERROR(server->logger, "tcp_server", "Failed to create accept thread", NULL);
        }
        atomic_store(&server->running, false);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    if (server->logger) {
        LOG_INFO(server->logger, "tcp_server", "Server started", NULL);
    }
    
    return DISTRIC_OK;
}

void tcp_server_stop(tcp_server_t* server) {
    if (!server) return;
    
    atomic_store(&server->running, false);
    
    /* Wait for accept thread */
    pthread_join(server->accept_thread, NULL);
    
    if (server->logger) {
        LOG_INFO(server->logger, "tcp_server", "Server stopped", NULL);
    }
}

void tcp_server_destroy(tcp_server_t* server) {
    if (!server) return;
    
    if (atomic_load(&server->running)) {
        tcp_server_stop(server);
    }
    
    close(server->listen_fd);
    free(server);
}

uint16_t tcp_server_get_port(const tcp_server_t* server) {
    return server ? server->port : 0;
}