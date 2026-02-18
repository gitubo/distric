/*
 * http_server.c — DistriC Observability Library — Minimal HTTP Server
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_obs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>

#define MAX_REQUEST_SIZE 8192
#define MAX_RESPONSE_SIZE (1024 * 1024)
#define REQUEST_TIMEOUT_MS 5000

typedef struct {
    char method[16];
    char path[256];
    char version[16];
} http_request_t;

typedef struct {
    int status_code;
    const char* status_text;
    const char* content_type;
    const char* body;
    size_t body_length;
} http_response_t;

struct obs_server_s {
    int socket_fd;
    uint16_t port;
    pthread_t thread;
    _Atomic bool running;
    metrics_registry_t* metrics;
    health_registry_t* health;
};

/* Read HTTP request with timeout */
static int read_http_request(int client_fd, char* buffer, size_t buffer_size) {
    size_t total_read = 0;
    ssize_t bytes_read;
    
    struct timeval tv;
    tv.tv_sec = REQUEST_TIMEOUT_MS / 1000;
    tv.tv_usec = (REQUEST_TIMEOUT_MS % 1000) * 1000;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (total_read < buffer_size - 1) {
        bytes_read = read(client_fd, buffer + total_read, buffer_size - total_read - 1);
        
        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return -1;
        }
        
        if (bytes_read == 0) {
            break;
        }
        
        total_read += bytes_read;
        buffer[total_read] = '\0';
        
        if (strstr(buffer, "\r\n\r\n") != NULL) {
            break;
        }
        
        if (total_read >= buffer_size - 1) {
            break;
        }
    }
    
    return total_read;
}

/* Parse HTTP request */
static int parse_request(const char* buffer, http_request_t* request) {
    if (!buffer || !request) {
        return -1;
    }
    
    int parsed = sscanf(buffer, "%15s %255s %15s", 
                       request->method, request->path, request->version);
    
    if (parsed != 3) {
        return -1;
    }
    
    if (strncmp(request->version, "HTTP/1.", 7) != 0) {
        return -1;
    }
    
    if (strcmp(request->method, "GET") != 0) {
        return -1;
    }
    
    if (strstr(request->path, "..") != NULL) {
        return -1;
    }
    
    return 0;
}

/* Send HTTP response */
static void send_response(int client_fd, const http_response_t* response) {
    if (!response) return;
    
    char header[4096];
    int header_len = snprintf(header, sizeof(header),
                             "HTTP/1.1 %d %s\r\n"
                             "Content-Type: %s\r\n"
                             "Content-Length: %zu\r\n"
                             "Connection: close\r\n"
                             "Server: DistriC-Obs/1.0\r\n"
                             "\r\n",
                             response->status_code,
                             response->status_text,
                             response->content_type,
                             response->body_length);
    
    if (header_len > 0 && (size_t)header_len < sizeof(header)) {
        ssize_t sent = 0;
        while (sent < header_len) {
            ssize_t n = write(client_fd, header + sent, header_len - sent);
            if (n <= 0) break;
            sent += n;
        }
        
        if (response->body && response->body_length > 0) {
            sent = 0;
            while (sent < (ssize_t)response->body_length) {
                ssize_t n = write(client_fd, response->body + sent, 
                                 response->body_length - sent);
                if (n <= 0) break;
                sent += n;
            }
        }
    }
}

/* Handle /metrics endpoint */
static void handle_metrics(obs_server_t* server, int client_fd) {
    char* metrics_output = NULL;
    size_t metrics_size = 0;
    
    distric_err_t err = metrics_export_prometheus(server->metrics, 
                                                   &metrics_output, 
                                                   &metrics_size);
    
    if (err == DISTRIC_OK && metrics_output) {
        http_response_t response = {
            .status_code = 200,
            .status_text = "OK",
            .content_type = "text/plain; version=0.0.4",
            .body = metrics_output,
            .body_length = metrics_size
        };
        send_response(client_fd, &response);
        free(metrics_output);
    } else {
        const char* body = "Internal error\n";
        http_response_t response = {
            .status_code = 500,
            .status_text = "Internal Server Error",
            .content_type = "text/plain",
            .body = body,
            .body_length = strlen(body)
        };
        send_response(client_fd, &response);
    }
}

/* Handle /health/live endpoint */
static void handle_health_live(int client_fd) {
    const char* body = "{\"status\":\"UP\"}";
    http_response_t response = {
        .status_code = 200,
        .status_text = "OK",
        .content_type = "application/json",
        .body = body,
        .body_length = strlen(body)
    };
    send_response(client_fd, &response);
}

/* Handle /health/ready endpoint */
static void handle_health_ready(obs_server_t* server, int client_fd) {
    char* health_json = NULL;
    size_t health_size = 0;
    
    distric_err_t err = health_export_json(server->health, &health_json, &health_size);
    
    if (err == DISTRIC_OK && health_json) {
        health_status_t status = health_get_overall_status(server->health);
        int status_code = (status == HEALTH_UP) ? 200 : 503;
        const char* status_text = (status == HEALTH_UP) ? "OK" : "Service Unavailable";
        
        http_response_t response = {
            .status_code = status_code,
            .status_text = status_text,
            .content_type = "application/json",
            .body = health_json,
            .body_length = health_size
        };
        send_response(client_fd, &response);
        free(health_json);
    } else {
        const char* body = "{\"status\":\"DOWN\"}";
        http_response_t response = {
            .status_code = 503,
            .status_text = "Service Unavailable",
            .content_type = "application/json",
            .body = body,
            .body_length = strlen(body)
        };
        send_response(client_fd, &response);
    }
}

/* Handle 400 Bad Request */
static void handle_bad_request(int client_fd) {
    const char* body = "Bad Request\n";
    http_response_t response = {
        .status_code = 400,
        .status_text = "Bad Request",
        .content_type = "text/plain",
        .body = body,
        .body_length = strlen(body)
    };
    send_response(client_fd, &response);
}

/* Handle 404 Not Found */
static void handle_not_found(int client_fd) {
    const char* body = "Not Found\n";
    http_response_t response = {
        .status_code = 404,
        .status_text = "Not Found",
        .content_type = "text/plain",
        .body = body,
        .body_length = strlen(body)
    };
    send_response(client_fd, &response);
}

/* Handle 405 Method Not Allowed */
static void handle_method_not_allowed(int client_fd) {
    const char* body = "Method Not Allowed\n";
    http_response_t response = {
        .status_code = 405,
        .status_text = "Method Not Allowed",
        .content_type = "text/plain",
        .body = body,
        .body_length = strlen(body)
    };
    send_response(client_fd, &response);
}

/* Handle client request */
static void handle_client(obs_server_t* server, int client_fd) {
    char buffer[MAX_REQUEST_SIZE];
    
    int bytes_read = read_http_request(client_fd, buffer, sizeof(buffer));
    
    if (bytes_read <= 0) {
        close(client_fd);
        return;
    }
    
    http_request_t request;
    if (parse_request(buffer, &request) < 0) {
        handle_bad_request(client_fd);
        close(client_fd);
        return;
    }
    
    /* Route request */
    if (strcmp(request.method, "GET") == 0) {
        if (strcmp(request.path, "/metrics") == 0) {
            handle_metrics(server, client_fd);
        } else if (strcmp(request.path, "/health/live") == 0) {
            handle_health_live(client_fd);
        } else if (strcmp(request.path, "/health/ready") == 0) {
            handle_health_ready(server, client_fd);
        } else {
            handle_not_found(client_fd);
        }
    } else {
        handle_method_not_allowed(client_fd);
    }
    
    close(client_fd);
}

/* Server thread */
static void* server_thread_fn(void* arg) {
    obs_server_t* server = (obs_server_t*)arg;
    
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(server->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (atomic_load(&server->running)) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server->socket_fd, 
                              (struct sockaddr*)&client_addr, 
                              &client_len);
        
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            continue;
        }
        
        struct timeval client_tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &client_tv, sizeof(client_tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &client_tv, sizeof(client_tv));
        
        handle_client(server, client_fd);
    }
    
    return NULL;
}

/* Initialize HTTP server */
distric_err_t obs_server_init(obs_server_t** server,
                              uint16_t port,
                              metrics_registry_t* metrics,
                              health_registry_t* health) {
    if (!server || !metrics || !health) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    obs_server_t* srv = calloc(1, sizeof(obs_server_t));
    if (!srv) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    srv->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->socket_fd < 0) {
        free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    int opt = 1;
    setsockopt(srv->socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(srv->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(srv->socket_fd);
        free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    if (port == 0) {
        socklen_t addr_len = sizeof(addr);
        if (getsockname(srv->socket_fd, (struct sockaddr*)&addr, &addr_len) == 0) {
            srv->port = ntohs(addr.sin_port);
        }
    } else {
        srv->port = port;
    }
    
    if (listen(srv->socket_fd, 10) < 0) {
        close(srv->socket_fd);
        free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    srv->metrics = metrics;
    srv->health = health;
    atomic_init(&srv->running, true);
    
    if (pthread_create(&srv->thread, NULL, server_thread_fn, srv) != 0) {
        close(srv->socket_fd);
        free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    *server = srv;
    return DISTRIC_OK;
}

/* Destroy HTTP server */
void obs_server_destroy(obs_server_t* server) {
    if (!server) {
        return;
    }
    
    atomic_store(&server->running, false);
    pthread_join(server->thread, NULL);
    
    if (server->socket_fd >= 0) {
        close(server->socket_fd);
    }
    
    free(server);
}

/* Get server port */
uint16_t obs_server_get_port(obs_server_t* server) {
    return server ? server->port : 0;
}