#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_obs/http_server.h"
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

#define MAX_REQUEST_SIZE 8192
#define MAX_RESPONSE_SIZE (1024 * 1024)  /* 1MB */

/* Parse HTTP request */
static int parse_request(const char* buffer, http_request_t* request) {
    /* Parse request line: METHOD PATH VERSION */
    if (sscanf(buffer, "%15s %255s %15s", 
               request->method, request->path, request->version) != 3) {
        return -1;
    }
    
    return 0;
}

/* Send HTTP response */
static void send_response(int client_fd, const http_response_t* response) {
    char header[4096];
    int header_len = snprintf(header, sizeof(header),
                             "HTTP/1.1 %d %s\r\n"
                             "Content-Type: %s\r\n"
                             "Content-Length: %zu\r\n"
                             "Connection: close\r\n"
                             "\r\n",
                             response->status_code,
                             response->status_text,
                             response->content_type,
                             response->body_length);
    
    if (header_len > 0) {
        write(client_fd, header, header_len);
    }
    
    if (response->body && response->body_length > 0) {
        write(client_fd, response->body, response->body_length);
    }
}

/* Handle /metrics endpoint */
static void handle_metrics(obs_server_t* server, int client_fd) {
    char* metrics_output = NULL;
    size_t metrics_size = 0;
    
    distric_err_t err = metrics_export_prometheus(server->metrics, 
                                                   &metrics_output, 
                                                   &metrics_size);
    
    if (err == DISTRIC_OK) {
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
        const char* error_body = "Internal Server Error";
        http_response_t response = {
            .status_code = 500,
            .status_text = "Internal Server Error",
            .content_type = "text/plain",
            .body = error_body,
            .body_length = strlen(error_body)
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
    char* health_output = NULL;
    size_t health_size = 0;
    
    distric_err_t err = health_export_json(server->health, 
                                           &health_output, 
                                           &health_size);
    
    if (err == DISTRIC_OK) {
        health_status_t overall = health_get_overall_status(server->health);
        int status_code = (overall == HEALTH_UP) ? 200 : 503;
        const char* status_text = (overall == HEALTH_UP) ? "OK" : "Service Unavailable";
        
        http_response_t response = {
            .status_code = status_code,
            .status_text = status_text,
            .content_type = "application/json",
            .body = health_output,
            .body_length = health_size
        };
        send_response(client_fd, &response);
        free(health_output);
    } else {
        const char* error_body = "{\"status\":\"ERROR\"}";
        http_response_t response = {
            .status_code = 500,
            .status_text = "Internal Server Error",
            .content_type = "application/json",
            .body = error_body,
            .body_length = strlen(error_body)
        };
        send_response(client_fd, &response);
    }
}

/* Handle 404 Not Found */
static void handle_not_found(int client_fd) {
    const char* body = "Not Found";
    http_response_t response = {
        .status_code = 404,
        .status_text = "Not Found",
        .content_type = "text/plain",
        .body = body,
        .body_length = strlen(body)
    };
    send_response(client_fd, &response);
}

/* Handle client request */
static void handle_client(obs_server_t* server, int client_fd) {
    char buffer[MAX_REQUEST_SIZE];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    
    if (bytes_read <= 0) {
        close(client_fd);
        return;
    }
    
    buffer[bytes_read] = '\0';
    
    http_request_t request;
    if (parse_request(buffer, &request) < 0) {
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
        /* Method not allowed */
        const char* body = "Method Not Allowed";
        http_response_t response = {
            .status_code = 405,
            .status_text = "Method Not Allowed",
            .content_type = "text/plain",
            .body = body,
            .body_length = strlen(body)
        };
        send_response(client_fd, &response);
    }
    
    close(client_fd);
}

/* Server thread */
static void* server_thread_fn(void* arg) {
    obs_server_t* server = (obs_server_t*)arg;
    
    /* Set socket to non-blocking for accept timeout */
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
                /* Timeout - check if still running */
                continue;
            }
            /* Other error */
            continue;
        }
        
        /* Handle client request */
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
    
    /* Create socket */
    srv->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->socket_fd < 0) {
        free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Set SO_REUSEADDR */
    int opt = 1;
    setsockopt(srv->socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    /* Bind to port */
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
    
    /* Get actual port if 0 was specified */
    if (port == 0) {
        socklen_t addr_len = sizeof(addr);
        if (getsockname(srv->socket_fd, (struct sockaddr*)&addr, &addr_len) == 0) {
            srv->port = ntohs(addr.sin_port);
        }
    } else {
        srv->port = port;
    }
    
    /* Listen */
    if (listen(srv->socket_fd, 10) < 0) {
        close(srv->socket_fd);
        free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    srv->metrics = metrics;
    srv->health = health;
    atomic_init(&srv->running, true);
    
    /* Start server thread */
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