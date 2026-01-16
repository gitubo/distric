/**
 * @file tcp.c
 * @brief TCP Server and Connection Implementation
 * 
 * Non-blocking TCP server using epoll (Linux) or kqueue (BSD/macOS)
 * with integrated observability.
 * Uses ONLY the public distric_obs API.
 */

#define _POSIX_C_SOURCE 200112L

#include "distric_transport/tcp.h"
#include <distric_obs.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <stdatomic.h>

#ifdef __linux__
#include <sys/epoll.h>
#define USE_EPOLL 1
#else
#include <sys/event.h>
#define USE_KQUEUE 1
#endif

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

#define MAX_EVENTS 128
#define BACKLOG 128

struct tcp_connection_s {
    int fd;
    uint64_t conn_id;
    
    char remote_addr[256];
    uint16_t remote_port;
    
    uint64_t created_at;
    
    /* Observability */
    metrics_registry_t* metrics;
    logger_t* logger;
    
    metric_t* bytes_sent_metric;
    metric_t* bytes_recv_metric;
    
    bool is_closed;
};

struct tcp_server_s {
    int listen_fd;
    
#ifdef USE_EPOLL
    int epoll_fd;
#else
    int kqueue_fd;
#endif
    
    uint16_t port;
    char bind_addr[256];
    
    /* Thread management */
    pthread_t event_thread;
    bool running;
    pthread_mutex_t lock;
    
    /* Callback */
    tcp_connection_callback_t on_connection;
    void* userdata;
    
    /* Observability */
    metrics_registry_t* metrics;
    logger_t* logger;
    
    metric_t* connections_active;
    metric_t* connections_total;
    metric_t* bytes_sent_total;
    metric_t* bytes_recv_total;
    metric_t* errors_total;
};

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

static uint64_t get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_tcp_nodelay(int fd) {
    int flag = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

static uint64_t generate_conn_id(void) {
    static atomic_uint_fast64_t counter = 0;
    return atomic_fetch_add(&counter, 1) + 1;
}

/* ============================================================================
 * TCP CONNECTION IMPLEMENTATION
 * ========================================================================= */

static tcp_connection_t* tcp_connection_create_internal(
    int fd,
    const struct sockaddr_in* addr,
    metrics_registry_t* metrics,
    logger_t* logger
) {
    tcp_connection_t* conn = calloc(1, sizeof(tcp_connection_t));
    if (!conn) return NULL;
    
    conn->fd = fd;
    conn->conn_id = generate_conn_id();
    conn->created_at = get_timestamp_us();
    conn->metrics = metrics;
    conn->logger = logger;
    conn->is_closed = false;
    
    /* Extract remote address */
    inet_ntop(AF_INET, &addr->sin_addr, conn->remote_addr, sizeof(conn->remote_addr));
    conn->remote_port = ntohs(addr->sin_port);
    
    /* Get or create metrics */
    if (metrics) {
        metrics_register_counter(metrics, "tcp_bytes_sent_total", 
                                "Total bytes sent", NULL, 0, &conn->bytes_sent_metric);
        metrics_register_counter(metrics, "tcp_bytes_received_total",
                                "Total bytes received", NULL, 0, &conn->bytes_recv_metric);
    }
    
    return conn;
}

int tcp_send(tcp_connection_t* conn, const void* data, size_t len) {
    if (!conn || conn->is_closed || !data || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    ssize_t sent = send(conn->fd, data, len, MSG_NOSIGNAL);
    
    if (sent > 0) {
        if (conn->bytes_sent_metric) {
            metrics_counter_add(conn->bytes_sent_metric, sent);
        }
        
        if (conn->logger) {
            char conn_id_str[32];
            snprintf(conn_id_str, sizeof(conn_id_str), "%lu", conn->conn_id);
            char bytes_str[32];
            snprintf(bytes_str, sizeof(bytes_str), "%zd", sent);
            
            LOG_DEBUG(conn->logger, "tcp", "Data sent",
                     "conn_id", conn_id_str,
                     "bytes", bytes_str);
        }
    } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        if (conn->logger) {
            char conn_id_str[32];
            snprintf(conn_id_str, sizeof(conn_id_str), "%lu", conn->conn_id);
            
            LOG_ERROR(conn->logger, "tcp", "Send failed",
                     "conn_id", conn_id_str,
                     "error", strerror(errno));
        }
    }
    
    return (int)sent;
}

int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms) {
    if (!conn || conn->is_closed || !buffer || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Handle timeout with select */
    if (timeout_ms > 0) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(conn->fd, &read_fds);
        
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        
        int ready = select(conn->fd + 1, &read_fds, NULL, NULL, &tv);
        if (ready <= 0) {
            return ready; /* 0 = timeout, -1 = error */
        }
    }
    
    ssize_t received = recv(conn->fd, buffer, len, 0);
    
    if (received > 0) {
        if (conn->bytes_recv_metric) {
            metrics_counter_add(conn->bytes_recv_metric, received);
        }
        
        if (conn->logger) {
            char conn_id_str[32];
            snprintf(conn_id_str, sizeof(conn_id_str), "%lu", conn->conn_id);
            char bytes_str[32];
            snprintf(bytes_str, sizeof(bytes_str), "%zd", received);
            
            LOG_DEBUG(conn->logger, "tcp", "Data received",
                     "conn_id", conn_id_str,
                     "bytes", bytes_str);
        }
    } else if (received == 0) {
        /* Connection closed */
        if (conn->logger) {
            char conn_id_str[32];
            snprintf(conn_id_str, sizeof(conn_id_str), "%lu", conn->conn_id);
            
            LOG_INFO(conn->logger, "tcp", "Connection closed by peer",
                    "conn_id", conn_id_str);
        }
    }
    
    return (int)received;
}

void tcp_close(tcp_connection_t* conn) {
    if (!conn || conn->is_closed) return;
    
    conn->is_closed = true;
    
    if (conn->logger) {
        char conn_id_str[32];
        snprintf(conn_id_str, sizeof(conn_id_str), "%lu", conn->conn_id);
        
        LOG_INFO(conn->logger, "tcp", "Connection closed",
                "conn_id", conn_id_str,
                "remote_addr", conn->remote_addr);
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
    if (!conn || !addr_out || !port_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    strncpy(addr_out, conn->remote_addr, addr_len - 1);
    addr_out[addr_len - 1] = '\0';
    *port_out = conn->remote_port;
    
    return DISTRIC_OK;
}

uint64_t tcp_get_connection_id(tcp_connection_t* conn) {
    return conn ? conn->conn_id : 0;
}

/* ============================================================================
 * TCP SERVER IMPLEMENTATION
 * ========================================================================= */

#ifdef USE_EPOLL
static void* event_loop_epoll(void* arg) {
    tcp_server_t* server = (tcp_server_t*)arg;
    struct epoll_event events[MAX_EVENTS];
    
    while (server->running) {
        int nfds = epoll_wait(server->epoll_fd, events, MAX_EVENTS, 1000);
        
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server->listen_fd) {
                /* New connection */
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                
                int client_fd = accept(server->listen_fd, 
                                      (struct sockaddr*)&client_addr, 
                                      &addr_len);
                
                if (client_fd < 0) {
                    if (server->errors_total) {
                        metrics_counter_inc(server->errors_total);
                    }
                    continue;
                }
                
                set_nonblocking(client_fd);
                set_tcp_nodelay(client_fd);
                
                /* Create connection object */
                tcp_connection_t* conn = tcp_connection_create_internal(
                    client_fd, &client_addr, server->metrics, server->logger);
                
                if (!conn) {
                    close(client_fd);
                    continue;
                }
                
                /* Update metrics */
                if (server->connections_active) {
                    metrics_gauge_set(server->connections_active, 1);
                }
                if (server->connections_total) {
                    metrics_counter_inc(server->connections_total);
                }
                
                /* Log connection */
                if (server->logger) {
                    char conn_id_str[32];
                    snprintf(conn_id_str, sizeof(conn_id_str), "%lu", conn->conn_id);
                    char port_str[16];
                    snprintf(port_str, sizeof(port_str), "%u", conn->remote_port);
                    
                    LOG_INFO(server->logger, "tcp", "Connection accepted",
                            "conn_id", conn_id_str,
                            "remote_addr", conn->remote_addr,
                            "remote_port", port_str);
                }
                
                /* Invoke callback */
                if (server->on_connection) {
                    server->on_connection(conn, server->userdata);
                }
            }
        }
    }
    
    return NULL;
}
#endif

#ifdef USE_KQUEUE
static void* event_loop_kqueue(void* arg) {
    tcp_server_t* server = (tcp_server_t*)arg;
    struct kevent events[MAX_EVENTS];
    struct timespec timeout = {1, 0}; /* 1 second */
    
    while (server->running) {
        int nev = kevent(server->kqueue_fd, NULL, 0, events, MAX_EVENTS, &timeout);
        
        for (int i = 0; i < nev; i++) {
            if (events[i].ident == (uintptr_t)server->listen_fd) {
                /* New connection */
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                
                int client_fd = accept(server->listen_fd, 
                                      (struct sockaddr*)&client_addr, 
                                      &addr_len);
                
                if (client_fd < 0) {
                    if (server->errors_total) {
                        metrics_counter_inc(server->errors_total);
                    }
                    continue;
                }
                
                set_nonblocking(client_fd);
                set_tcp_nodelay(client_fd);
                
                /* Create connection object */
                tcp_connection_t* conn = tcp_connection_create_internal(
                    client_fd, &client_addr, server->metrics, server->logger);
                
                if (!conn) {
                    close(client_fd);
                    continue;
                }
                
                /* Update metrics */
                if (server->connections_active) {
                    metrics_gauge_set(server->connections_active, 1);
                }
                if (server->connections_total) {
                    metrics_counter_inc(server->connections_total);
                }
                
                /* Log connection */
                if (server->logger) {
                    char conn_id_str[32];
                    snprintf(conn_id_str, sizeof(conn_id_str), "%lu", conn->conn_id);
                    char port_str[16];
                    snprintf(port_str, sizeof(port_str), "%u", conn->remote_port);
                    
                    LOG_INFO(server->logger, "tcp", "Connection accepted",
                            "conn_id", conn_id_str,
                            "remote_addr", conn->remote_addr,
                            "remote_port", port_str);
                }
                
                /* Invoke callback */
                if (server->on_connection) {
                    server->on_connection(conn, server->userdata);
                }
            }
        }
    }
    
    return NULL;
}
#endif

distric_err_t tcp_server_create(
    const char* bind_addr,
    uint16_t port,
    metrics_registry_t* metrics,
    logger_t* logger,
    tcp_server_t** server
) {
    if (!bind_addr || !server) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    tcp_server_t* srv = calloc(1, sizeof(tcp_server_t));
    if (!srv) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    strncpy(srv->bind_addr, bind_addr, sizeof(srv->bind_addr) - 1);
    srv->port = port;
    srv->metrics = metrics;
    srv->logger = logger;
    srv->running = false;
    
    pthread_mutex_init(&srv->lock, NULL);
    
    /* Create socket */
    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Set socket options */
    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(srv->listen_fd);
    
    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, bind_addr, &addr.sin_addr);
    
    if (bind(srv->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(srv->listen_fd);
        free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Listen */
    if (listen(srv->listen_fd, BACKLOG) < 0) {
        close(srv->listen_fd);
        free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Create event mechanism */
#ifdef USE_EPOLL
    srv->epoll_fd = epoll_create1(0);
    if (srv->epoll_fd < 0) {
        close(srv->listen_fd);
        free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = srv->listen_fd;
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);
#else
    srv->kqueue_fd = kqueue();
    if (srv->kqueue_fd < 0) {
        close(srv->listen_fd);
        free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    struct kevent ev;
    EV_SET(&ev, srv->listen_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(srv->kqueue_fd, &ev, 1, NULL, 0, NULL);
#endif
    
    /* Register metrics */
    if (metrics) {
        metrics_register_gauge(metrics, "tcp_connections_active",
                              "Active TCP connections", NULL, 0, &srv->connections_active);
        metrics_register_counter(metrics, "tcp_connections_total",
                                "Total TCP connections", NULL, 0, &srv->connections_total);
        metrics_register_counter(metrics, "tcp_bytes_sent_total",
                                "Total bytes sent", NULL, 0, &srv->bytes_sent_total);
        metrics_register_counter(metrics, "tcp_bytes_received_total",
                                "Total bytes received", NULL, 0, &srv->bytes_recv_total);
        metrics_register_counter(metrics, "tcp_errors_total",
                                "Total TCP errors", NULL, 0, &srv->errors_total);
    }
    
    *server = srv;
    return DISTRIC_OK;
}

distric_err_t tcp_server_start(
    tcp_server_t* server,
    tcp_connection_callback_t on_connection,
    void* userdata
) {
    if (!server || !on_connection) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    server->on_connection = on_connection;
    server->userdata = userdata;
    server->running = true;
    
#ifdef USE_EPOLL
    pthread_create(&server->event_thread, NULL, event_loop_epoll, server);
#else
    pthread_create(&server->event_thread, NULL, event_loop_kqueue, server);
#endif
    
    if (server->logger) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", server->port);
        
        LOG_INFO(server->logger, "tcp", "Server started",
                "bind_addr", server->bind_addr,
                "port", port_str);
    }
    
    return DISTRIC_OK;
}

void tcp_server_stop(tcp_server_t* server) {
    if (!server || !server->running) return;
    
    server->running = false;
    pthread_join(server->event_thread, NULL);
    
    if (server->logger) {
        LOG_INFO(server->logger, "tcp", "Server stopped", "status", "shutdown");
    }
}

void tcp_server_destroy(tcp_server_t* server) {
    if (!server) return;
    
    tcp_server_stop(server);
    
#ifdef USE_EPOLL
    close(server->epoll_fd);
#else
    close(server->kqueue_fd);
#endif
    
    close(server->listen_fd);
    pthread_mutex_destroy(&server->lock);
    free(server);
}

/* ============================================================================
 * TCP CLIENT IMPLEMENTATION
 * ========================================================================= */

distric_err_t tcp_connect(
    const char* host,
    uint16_t port,
    int timeout_ms,
    metrics_registry_t* metrics,
    logger_t* logger,
    tcp_connection_t** conn
) {
    if (!host || !conn) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Resolve hostname */
    struct hostent* he = gethostbyname(host);
    if (!he) {
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Create socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
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
        close(fd);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Wait for connection with timeout */
    if (errno == EINPROGRESS) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(fd, &write_fds);
        
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        
        result = select(fd + 1, NULL, &write_fds, NULL, &tv);
        if (result <= 0) {
            close(fd);
            return DISTRIC_ERR_INIT_FAILED;
        }
        
        /* Check for errors */
        int error = 0;
        socklen_t len = sizeof(error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
        if (error != 0) {
            close(fd);
            return DISTRIC_ERR_INIT_FAILED;
        }
    }
    
    /* Create connection object */
    tcp_connection_t* c = tcp_connection_create_internal(fd, &addr, metrics, logger);
    if (!c) {
        close(fd);
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    if (logger) {
        char conn_id_str[32];
        snprintf(conn_id_str, sizeof(conn_id_str), "%lu", c->conn_id);
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        
        LOG_INFO(logger, "tcp", "Connected to server",
                "conn_id", conn_id_str,
                "host", host,
                "port", port_str);
    }
    
    *conn = c;
    return DISTRIC_OK;
}