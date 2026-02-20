# distric_transport API Reference

Version: 3.0.0  
Header: `#include <distric_transport.h>`

Backward-compatible with consumers compiled against major version 2.

---

## Error Codes

All functions return `distric_err_t` unless otherwise noted.

| Code | Meaning |
|------|---------|
| `DISTRIC_OK` | Success |
| `DISTRIC_ERR_INVALID_ARG` | NULL or out-of-range argument |
| `DISTRIC_ERR_NO_MEMORY` | Allocation failure or global memory budget exceeded |
| `DISTRIC_ERR_CONNECT` | Connection could not be established |
| `DISTRIC_ERR_TIMEOUT` | Operation did not complete within the deadline |
| `DISTRIC_ERR_IO` | Read or write error on socket |
| `DISTRIC_ERR_BACKPRESSURE` | Send queue high-water mark exceeded |
| `DISTRIC_ERR_CLOSED` | Operation on a closed connection |
| `DISTRIC_ERR_RATE_LIMITED` | UDP packet dropped by rate limiter |

---

## Global Configuration

Call once at startup before creating any transport objects.

### Types

```c
typedef struct {
    uint64_t max_transport_memory_bytes;  /* default: 512 MiB */
    uint32_t observability_sample_pct;    /* 1–100; default: 100 */
    uint32_t handler_warn_duration_ms;    /* default: 1000 */
} transport_global_config_t;
```

---

### `transport_config_init`

```c
void transport_config_init(transport_global_config_t* cfg);
```

Populate `cfg` with default values.

---

### `transport_config_load_env`

```c
void transport_config_load_env(transport_global_config_t* cfg);
```

Override fields in `cfg` with values from environment variables:

| Variable | Field |
|----------|-------|
| `DISTRIC_MAX_TRANSPORT_MEMORY` | `max_transport_memory_bytes` |
| `DISTRIC_OBS_SAMPLE_PCT` | `observability_sample_pct` |
| `DISTRIC_HANDLER_WARN_DURATION_MS` | `handler_warn_duration_ms` |

---

### `transport_config_apply`

```c
distric_err_t transport_config_apply(const transport_global_config_t* cfg);
```

Install the configuration globally. Thread-safe via atomics.

---

### `transport_config_get`

```c
void transport_config_get(transport_global_config_t* out);
```

Read the currently active global configuration into `out`.

---

### `transport_config_register_metrics`

```c
distric_err_t transport_config_register_metrics(metrics_registry_t* registry);
```

Register global transport metrics (memory budget utilisation, etc.) with `registry`. Optional but recommended.

---

## TCP Server

### Types

```c
typedef struct tcp_server tcp_server_t;
typedef struct tcp_connection tcp_connection_t;
typedef void (*tcp_connection_callback_t)(tcp_connection_t* conn, void* userdata);
```

---

### `tcp_server_create`

```c
distric_err_t tcp_server_create(
    const char*         bind_addr,
    uint16_t            port,
    metrics_registry_t* metrics,
    logger_t*           logger,
    tcp_server_t**      server
);
```

Create a TCP server bound to `bind_addr:port`. Does not start accepting until `tcp_server_start` is called.

**Parameters**
- `bind_addr` — IP address string, e.g. `"0.0.0.0"`.
- `port` — port number.
- `metrics` — registry for automatic metric registration; may be NULL.
- `logger` — structured logger; may be NULL.
- `server` — receives the server handle.

**Returns** `DISTRIC_OK` on success.

---

### `tcp_server_create_with_config`

```c
typedef struct {
    uint32_t worker_threads;
    uint32_t handler_warn_duration_ms;
    uint32_t observability_sample_pct;   /* 1–100 */
} tcp_server_config_t;

distric_err_t tcp_server_create_with_config(
    const char*               bind_addr,
    uint16_t                  port,
    const tcp_server_config_t* config,
    metrics_registry_t*       metrics,
    logger_t*                 logger,
    tcp_server_t**            server
);
```

Create a server with explicit configuration. If `config` is NULL, defaults are applied.

---

### `tcp_server_start`

```c
distric_err_t tcp_server_start(
    tcp_server_t*              server,
    tcp_connection_callback_t  on_connection,
    void*                      userdata
);
```

Begin accepting connections. `on_connection` is invoked on a worker thread for each accepted connection. The server runs in the background; this call returns immediately.

---

### `tcp_server_stop`

```c
void tcp_server_stop(tcp_server_t* server);
```

Signal all worker threads to stop and wait for them to finish. Existing handlers complete before return.

---

### `tcp_server_destroy`

```c
void tcp_server_destroy(tcp_server_t* server);
```

Stop the server (if running) and free all resources.

---

### `tcp_server_worker_busy_ratio`

```c
float tcp_server_worker_busy_ratio(const tcp_server_t* server);
```

Return the fraction of worker threads currently handling a connection (0.0–1.0). Use to detect worker pool saturation at runtime.

---

## TCP Connection

Connections are passed to the `tcp_connection_callback_t` by the server, or created explicitly with `tcp_connect`.

### `tcp_connect`

```c
distric_err_t tcp_connect(
    const char*         host,
    uint16_t            port,
    int                 timeout_ms,
    const void*         tls_config,     /* reserved; pass NULL */
    metrics_registry_t* metrics,
    logger_t*           logger,
    tcp_connection_t**  conn
);
```

Establish an outbound TCP connection.

**Parameters**
- `host` — hostname or IP address string.
- `port` — remote port.
- `timeout_ms` — connect timeout in milliseconds; 0 means use OS default.
- `tls_config` — reserved for future TLS support; must be NULL.
- `metrics`, `logger` — observability handles; may be NULL.
- `conn` — receives the connection handle.

**Returns** `DISTRIC_OK` on success, `DISTRIC_ERR_CONNECT` or `DISTRIC_ERR_TIMEOUT` on failure.

---

### `tcp_send`

```c
int tcp_send(tcp_connection_t* conn, const void* data, size_t len);
```

Enqueue data for sending. Returns the number of bytes accepted, or a negative `distric_err_t` value on error.

**Backpressure** — returns `DISTRIC_ERR_BACKPRESSURE` (negative) when the per-connection send queue exceeds its high-water mark. The caller must back off rather than retry in a tight loop.

**Thread safety** — safe to call from multiple threads on the same connection.

---

### `tcp_recv`

```c
int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms);
```

Receive data into `buffer`. Blocks up to `timeout_ms` milliseconds.

**Returns** number of bytes received (>0), 0 on orderly peer close, or a negative `distric_err_t` value on error or timeout.

---

### `tcp_get_remote_addr`

```c
distric_err_t tcp_get_remote_addr(
    tcp_connection_t* conn,
    char*             addr_out,
    size_t            addr_len,
    uint16_t*         port_out
);
```

Write the remote IP address string into `addr_out` and the port into `*port_out`.

---

### `tcp_get_connection_id`

```c
uint64_t tcp_get_connection_id(tcp_connection_t* conn);
```

Return the unique 64-bit connection identifier used in log and trace output.

---

### `tcp_close`

```c
void tcp_close(tcp_connection_t* conn);
```

Close the connection and free its resources. Must not be called on a connection that has been returned to a pool.

---

## TCP Connection Pool

### Types

```c
typedef struct tcp_pool tcp_pool_t;
```

---

### `tcp_pool_create`

```c
distric_err_t tcp_pool_create(
    size_t              max_connections,
    metrics_registry_t* metrics,
    logger_t*           logger,
    tcp_pool_t**        pool
);
```

Create a connection pool with a maximum of `max_connections` live sockets. Eviction uses LRU when the pool is full.

---

### `tcp_pool_acquire`

```c
distric_err_t tcp_pool_acquire(
    tcp_pool_t*        pool,
    const char*        host,
    uint16_t           port,
    tcp_connection_t** conn
);
```

Acquire a connection to `host:port`. Reuses an existing idle socket if one is available; otherwise establishes a new connection.

**Returns** `DISTRIC_OK` with `*conn` set on success.

---

### `tcp_pool_release`

```c
void tcp_pool_release(tcp_pool_t* pool, tcp_connection_t* conn);
```

Return a connection to the pool. The socket is kept alive for future reuse. Do not call `tcp_close` on a released connection.

---

### `tcp_pool_destroy`

```c
void tcp_pool_destroy(tcp_pool_t* pool);
```

Close all pooled connections and free resources.

---

### `tcp_pool_stats`

```c
typedef struct {
    size_t   pool_size;        /* current number of idle connections */
    uint64_t hits_total;       /* connections reused from pool */
    uint64_t misses_total;     /* new connections created */
    uint64_t evictions_total;  /* LRU evictions */
} tcp_pool_stats_t;

void tcp_pool_stats(const tcp_pool_t* pool, tcp_pool_stats_t* out);
```

Read current pool statistics into `out`.

---

## UDP Socket

### Types

```c
typedef struct udp_socket udp_socket_t;

typedef struct {
    uint32_t rate_limit_pps;  /* packets per second per peer; 0 = disabled */
    uint32_t burst_size;      /* token bucket burst capacity */
} udp_rate_limit_config_t;
```

---

### `udp_socket_create`

```c
distric_err_t udp_socket_create(
    const char*                    bind_addr,
    uint16_t                       port,
    const udp_rate_limit_config_t* rate_limit,  /* NULL to disable */
    metrics_registry_t*            metrics,
    logger_t*                      logger,
    udp_socket_t**                 sock
);
```

Create and bind a non-blocking UDP socket.

**Parameters**
- `bind_addr` — local IP address; use `"0.0.0.0"` for all interfaces.
- `rate_limit` — per-peer token-bucket configuration; NULL disables rate limiting.

---

### `udp_send`

```c
int udp_send(
    udp_socket_t* sock,
    const void*   data,
    size_t        len,
    const char*   dest_addr,
    uint16_t      dest_port
);
```

Send a datagram to `dest_addr:dest_port`.

**Returns** number of bytes sent, or a negative `distric_err_t` value.

---

### `udp_recv`

```c
int udp_recv(
    udp_socket_t* sock,
    void*         buffer,
    size_t        len,
    char*         src_addr_out,
    uint16_t*     src_port_out,
    int           timeout_ms
);
```

Receive a datagram. Writes the sender address into `src_addr_out` (caller-supplied, recommended minimum 64 bytes) and the sender port into `*src_port_out`.

**Returns** number of bytes received, or a negative `distric_err_t` value.

---

### `udp_close`

```c
void udp_close(udp_socket_t* sock);
```

Close the socket and free resources.

---

## Initialisation and Teardown Order

```
transport_config_apply
metrics_init + log_init
transport_config_register_metrics
  [ create servers, pools, sockets ]
  [ application runs ]
  [ destroy sockets, pools, connections ]
  [ destroy servers ]
log_destroy
metrics_destroy
```

Always destroy transport objects before destroying the observability handles they reference.
