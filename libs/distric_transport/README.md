# distric_transport

High-performance, observable network transport layer for the DistriC platform. Provides non-blocking TCP server/client, a thread-safe connection pool, and UDP sockets with integrated metrics, structured logging, and distributed tracing via `distric_obs`.

## Overview

`distric_transport` is the networking substrate for DistriC 2.0. All sockets are non-blocking; no library call blocks indefinitely. Observability is built in: every connection, byte, and error is automatically tracked without any instrumentation code in the caller.

## Features

- Non-blocking TCP server using epoll (Linux) or kqueue (macOS)
- TCP client with configurable connect timeout
- Thread-safe TCP connection pool with O(1) hash lookup and LRU eviction
- UDP socket with per-peer token-bucket rate limiting
- Backpressure signalled via `DISTRIC_ERR_BACKPRESSURE` rather than silent drops
- Worker pool saturation exposed as a runtime metric
- Automatic Prometheus metrics, structured JSON logs, and trace spans
- Global memory budget prevents new connections from being accepted before OOM
- Zero external dependencies beyond `distric_obs`

## Requirements

- C11 compiler with GNU extensions
- POSIX threads
- CMake 3.15 or later
- `distric_obs` library
- Linux (epoll) or macOS (kqueue)

## Build

```bash
cmake -B build -DBUILD_TESTING=ON
cmake --build build
cd build && ctest --output-on-failure
```

## Quick Start

### Single Include

```c
#include <distric_transport.h>
```

All transport functionality is available through this one header.

### TCP Server

```c
#include <distric_transport.h>

void on_connection(tcp_connection_t* conn, void* userdata) {
    char buf[4096];
    int n = tcp_recv(conn, buf, sizeof(buf), 5000);
    if (n > 0) {
        tcp_send(conn, buf, (size_t)n);
    }
    tcp_close(conn);
}

int main(void) {
    metrics_registry_t* metrics;
    logger_t*           logger;
    metrics_init(&metrics);
    log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);

    tcp_server_t* server;
    tcp_server_create("0.0.0.0", 9000, metrics, logger, &server);
    tcp_server_start(server, on_connection, NULL);

    sleep(60);  /* server runs on background threads */

    tcp_server_destroy(server);
    log_destroy(logger);
    metrics_destroy(metrics);
    return 0;
}
```

### TCP Client with Connection Pool

```c
tcp_pool_t* pool;
tcp_pool_create(100, metrics, logger, &pool);

tcp_connection_t* conn;
tcp_pool_acquire(pool, "10.0.1.5", 9000, &conn);

tcp_send(conn, "hello", 5);

char buf[64];
tcp_recv(conn, buf, sizeof(buf), 5000);

tcp_pool_release(pool, conn);  /* returns conn to pool; keeps socket alive */

tcp_pool_destroy(pool);
```

### UDP Socket

```c
udp_rate_limit_config_t rl = { .rate_limit_pps = 1000, .burst_size = 2000 };
udp_socket_t* udp;
udp_socket_create("0.0.0.0", 9001, &rl, metrics, logger, &udp);

udp_send(udp, "ping", 4, "10.0.1.6", 9001);

char    buf[1024];
char    src[64];
uint16_t src_port;
int n = udp_recv(udp, buf, sizeof(buf), src, &src_port, 5000);

udp_close(udp);
```

## Global Configuration

Call once at startup before creating any transport objects.

```c
transport_global_config_t cfg;
transport_config_init(&cfg);
transport_config_load_env(&cfg);           /* honour environment overrides */
cfg.max_transport_memory_bytes = 256 * 1024 * 1024;
cfg.handler_warn_duration_ms   = 500;
transport_config_apply(&cfg);
transport_config_register_metrics(metrics);
```

Environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `DISTRIC_MAX_TRANSPORT_MEMORY` | 512 MiB | Global memory ceiling |
| `DISTRIC_OBS_SAMPLE_PCT` | 100 | Observability sampling percentage (1–100) |
| `DISTRIC_HANDLER_WARN_DURATION_MS` | 1000 | Log WARN when handler exceeds this duration |

## Automatic Observability

### Prometheus Metrics

```
tcp_connections_active          Current active TCP connections
tcp_connections_total           Total TCP connections established
tcp_bytes_sent_total            Total bytes sent over TCP
tcp_bytes_received_total        Total bytes received over TCP
tcp_errors_total                Total TCP errors
tcp_worker_busy_ratio           Worker pool saturation (0.0–1.0)
tcp_handler_slow_total          Handlers that exceeded warn threshold

tcp_pool_size                   Current connection pool size
tcp_pool_hits_total             Pool reuse count
tcp_pool_misses_total           New connections created by pool

udp_packets_sent_total          Total UDP packets sent
udp_packets_received_total      Total UDP packets received
udp_bytes_sent_total            Total UDP bytes sent
udp_bytes_received_total        Total UDP bytes received
udp_errors_total                Total UDP errors
udp_rate_limited_total          Packets dropped by rate limiter
```

### Structured Logs (JSON)

```json
{"ts":1705147845123,"level":"INFO","component":"tcp","msg":"Connection accepted","remote_addr":"10.0.1.5","remote_port":"45231","conn_id":"123"}
{"ts":1705147845789,"level":"INFO","component":"tcp_pool","msg":"Connection reused","host":"10.0.1.5","port":"9000"}
{"ts":1705147846012,"level":"WARN","component":"tcp","msg":"Slow handler","conn_id":"456","duration_ms":"620"}
```

## Directory Structure

```
libs/distric_transport/
├── CMakeLists.txt
├── README.md
├── API.md
├── include/
│   ├── distric_transport.h         # Single public header (use this)
│   └── distric_transport/          # Internal sub-headers
│       ├── transport_config.h
│       ├── transport_error.h
│       ├── tcp.h
│       ├── tcp_pool.h
│       └── udp.h
└── src/
    ├── transport_config.c
    ├── transport_error.c
    ├── send_queue.c
    ├── circuit_breaker.c
    ├── tcp.c
    ├── tcp_pool.c
    └── udp.c
```

## API Stability

`include/distric_transport.h` is the only stable public interface. All sub-headers under `include/distric_transport/` are internal. Breaking changes to the public header increment the major version. The library is backward-compatible with any consumer compiled against major version 2 or later.

## Library Version

```c
#define DISTRIC_TRANSPORT_VERSION_STR   "3.0.0"
#define DISTRIC_TRANSPORT_COMPAT_MAJOR  2
```

## License

See repository root for license information.
