# distric_obs

High-performance observability library for the DistriC platform. Provides lock-free metrics, async structured logging, distributed tracing, health monitoring, and a Prometheus-compatible HTTP server.

## Overview

`distric_obs` is the foundational observability layer for DistriC 2.0. All other libraries in the platform depend on it for telemetry. It is designed around a strict non-blocking contract: every hot-path operation completes in O(1) bounded time and will never stall application threads.

## Features

- Lock-free counters, gauges, and histograms using C11 atomics
- Prometheus text format export over HTTP
- Async JSON structured logging with a lock-free MPSC ring buffer
- OpenTelemetry-compatible distributed tracing with context propagation
- Component health tracking with JSON export
- Minimal non-blocking HTTP server for observability endpoints
- Zero external dependencies — standard C library only
- Thread-safe throughout

## Requirements

- C11 compiler with GNU extensions (`-std=gnu11`)
- POSIX threads (`pthreads`)
- CMake 3.15 or later
- Linux (epoll) or macOS (kqueue)

## Build

```bash
cmake -B build -DBUILD_TESTING=ON
cmake --build build
cd build && ctest --output-on-failure
```

## Quick Start

```c
#include <distric_obs.h>
#include <distric_obs/tracing.h>
#include <distric_obs/health.h>
#include <distric_obs/http_server.h>

int main(void) {
    metrics_registry_t* metrics;
    logger_t*           logger;
    tracer_t*           tracer;
    health_registry_t*  health;

    metrics_init(&metrics);
    log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);
    trace_init(&tracer, export_callback, NULL);
    health_init(&health);

    obs_server_t* server;
    obs_server_init(&server, 9090, metrics, health);

    /* Register a metric */
    metric_t* requests;
    metrics_register_counter(metrics, "requests_total", "Total requests",
                             NULL, 0, &requests);

    /* Start a trace span */
    trace_span_t* span;
    trace_start_span(tracer, "handle_request", &span);

    /* Log a structured event */
    LOG_INFO(logger, "http", "Request received", "method", "GET", "path", "/api");

    /* Increment the counter */
    metrics_counter_inc(requests);

    trace_finish_span(tracer, span);

    /* Observability endpoints
     *   GET /metrics        -- Prometheus metrics
     *   GET /health/ready   -- Readiness check
     *   GET /health/live    -- Liveness check
     */

    obs_server_destroy(server);
    trace_destroy(tracer);
    log_destroy(logger);
    health_destroy(health);
    metrics_destroy(metrics);
    return 0;
}
```

## Components

### Metrics

Lock-free counters, gauges, and histograms. Registration is serialised (rare path); all update operations use C11 atomics and are safe to call from any thread at any frequency.

- Counter increment: ~10–20 ns
- Gauge set: ~15–25 ns
- Histogram observe: ~30–50 ns
- Throughput: >10 M operations/second

### Logging

JSON-formatted structured logging. Async mode writes to a lock-free MPSC ring buffer drained by a background thread. Sync mode uses a per-write mutex and is intended only for low-volume or debug scenarios.

- Async latency: <1 µs per log entry
- Throughput: >100 K entries/second

### Distributed Tracing

OpenTelemetry-compatible span model with parent-child relationships and W3C-style context propagation headers. Export runs on a private background thread with adaptive sampling under backpressure.

- Span start/finish: ~50 ns
- Batch export interval: 5 s (configurable at compile time)

### Health Monitoring

Per-component health status (`HEALTH_UP`, `HEALTH_DEGRADED`, `HEALTH_DOWN`) with an aggregated system-level status. Updates use a single CAS operation per component.

### HTTP Server

Minimal non-blocking HTTP server that serves `/metrics`, `/health/ready`, and `/health/live`. It copies export data before formatting, so it never holds registry locks during request handling.

## Directory Structure

```
libs/distric_obs/
├── CMakeLists.txt
├── README.md
├── API.md
├── include/
│   ├── distric_obs.h           # Stable public header (use this)
│   └── distric_obs/            # Internal sub-headers (do not include directly)
│       ├── error.h
│       ├── metrics.h
│       ├── logging.h
│       ├── tracing.h
│       ├── health.h
│       └── http_server.h
└── src/
    ├── error.c
    ├── metrics.c
    ├── logging.c
    ├── tracing.c
    ├── health.c
    └── http_server.c
```

## API Stability

`include/distric_obs.h` is the only stable public interface. All headers under `include/distric_obs/` are internal and must not be included directly by consumers. Breaking changes to the public header increment the major version.

## Thread Safety

| Subsystem | Guarantee |
|-----------|-----------|
| Metrics hot-path | Lock-free atomics, any thread |
| Metrics registration | Serialised by internal mutex |
| Logger async | MPSC lock-free ring buffer |
| Logger sync | Per-write mutex |
| Tracing start/finish | Lock-free |
| Health updates | Single CAS per component |
| HTTP server | Copies data before formatting; no registry locks held |

## Build Options

The following compile-time constants can be overridden:

| Constant | Default | Description |
|----------|---------|-------------|
| `MAX_METRICS` | 1024 | Maximum metrics in a registry |
| `MAX_HEALTH_COMPONENTS` | 64 | Maximum health components |
| `MAX_SPANS_BUFFER` | 1000 | Span buffer size |
| `RING_BUFFER_SIZE` | 8192 | Log ring buffer size |

## License

See repository root for license information.
