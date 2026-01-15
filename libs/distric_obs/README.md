# DistriC Observability Library

High-performance observability library for DistriC 2.0 with lock-free metrics, async logging, distributed tracing, health monitoring, and a Prometheus-compatible HTTP server.

## Features

- **Lock-free metrics**: Counters, gauges, and histograms using C11 atomics
- **Prometheus export**: Native Prometheus text format output via HTTP endpoint
- **Async logging**: JSON structured logging with lock-free ring buffer
- **Distributed tracing**: OpenTelemetry-compatible tracing with context propagation
- **Health monitoring**: Component health tracking with JSON export
- **HTTP server**: Minimal, non-blocking HTTP server for observability endpoints
- **Zero dependencies**: Only standard C library required
- **Thread-safe**: All operations are safe for concurrent use

## Quick Start

### Build

From project root:

```bash
# Build entire project including distric_obs
make all

# Run all tests
make test

# Run benchmarks
make bench

# Full validation with Valgrind
make valgrind
```

### Basic Usage

```c
#include <distric_obs.h>
#include <distric_obs/tracing.h>
#include <distric_obs/health.h>
#include <distric_obs/http_server.h>

// Initialize observability stack
metrics_registry_t* metrics;
logger_t* logger;
tracer_t* tracer;
health_registry_t* health;

metrics_init(&metrics);
log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);
trace_init(&tracer, export_callback, NULL);
health_init(&health);

// Start HTTP server on port 9090
obs_server_t* server;
obs_server_init(&server, 9090, metrics, health);

// Register metrics
metric_t* requests;
metrics_register_counter(metrics, "requests_total", "Total requests", NULL, 0, &requests);

// Start a trace
trace_span_t* span;
trace_start_span(tracer, "handle_request", &span);

// Log structured data
LOG_INFO(logger, "http", "Request received", "method", "GET", "path", "/api");

// Update metrics
metrics_counter_inc(requests);

// Finish trace
trace_finish_span(tracer, span);

// Access observability data:
// curl http://localhost:9090/metrics        # Prometheus metrics
// curl http://localhost:9090/health/ready   # Health status
// curl http://localhost:9090/health/live    # Liveness check

// Cleanup
obs_server_destroy(server);
trace_destroy(tracer);
log_destroy(logger);
health_destroy(health);
metrics_destroy(metrics);
```

## Directory Structure

```
libs/distric_obs/
├── CMakeLists.txt              # Library build configuration
├── README.md                   # This file
├── API.md                      # Detailed API reference
├── QUICKSTART.md               # 5-minute tutorial
├── include/
│   ├── distric_obs.h          # Main public header
│   └── distric_obs/           # Internal headers
│       ├── error.h
│       ├── metrics.h
│       ├── logging.h
│       ├── tracing.h          # Distributed tracing
│       ├── health.h           # Health monitoring
│       └── http_server.h      # HTTP server
├── src/
│   ├── error.c
│   ├── metrics.c
│   ├── logging.c
│   ├── tracing.c              # Tracing implementation
│   ├── health.c               # Health implementation
│   └── http_server.c          # HTTP server implementation
└── tests/
    ├── test_metrics.c
    ├── test_logging.c
    ├── test_tracing.c         # Tracing tests
    ├── test_health.c          # Health tests
    ├── test_http_server.c     # HTTP server tests
    ├── test_integration.c     # Full stack integration test
    ├── bench_metrics.c
    └── bench_logging.c
```

## Components

### 1. Metrics System

Lock-free metrics collection with Prometheus export.

```c
// Register metrics
metric_t* counter, *gauge, *histogram;
metrics_register_counter(metrics, "requests_total", "Requests", NULL, 0, &counter);
metrics_register_gauge(metrics, "cpu_usage", "CPU usage", NULL, 0, &gauge);
metrics_register_histogram(metrics, "latency_seconds", "Latency", NULL, 0, &histogram);

// Update metrics (thread-safe, lock-free)
metrics_counter_inc(counter);
metrics_gauge_set(gauge, 65.5);
metrics_histogram_observe(histogram, 0.042);
```

**Performance**: 10-20ns per counter increment, >10M ops/sec

### 2. Structured Logging

JSON-formatted async logging with thread-local buffers.

```c
// Initialize logger
logger_t* logger;
log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);

// Write structured logs
LOG_INFO(logger, "database", "Connected", "host", "localhost", "port", "5432");
LOG_ERROR(logger, "api", "Request failed", "error", "timeout", "duration", "30s");
```

**Performance**: <1μs per log (async), >100K logs/sec

### 3. Distributed Tracing

OpenTelemetry-compatible distributed tracing with context propagation.

```c
// Export callback
void export_spans(trace_span_t* spans, size_t count, void* data) {
    // Send to backend (Jaeger, Zipkin, etc.)
}

tracer_t* tracer;
trace_init(&tracer, export_spans, NULL);

// Start root span
trace_span_t* span;
trace_start_span(tracer, "api_call", &span);
trace_add_tag(span, "http.method", "POST");

// Start child span
trace_span_t* child;
trace_start_child_span(tracer, span, "database_query", &child);
trace_finish_span(tracer, child);

// Context propagation (for RPC)
char header[256];
trace_inject_context(span, header, sizeof(header));
// Send header to remote service...

// On remote service:
trace_context_t ctx;
trace_extract_context(header, &ctx);
trace_span_t* remote_span;
trace_start_span_from_context(tracer, &ctx, "remote_op", &remote_span);
```

**Performance**: ~50ns per span, batch export every 5s

### 4. Health Monitoring

Component health tracking with overall system status.

```c
health_registry_t* health;
health_init(&health);

// Register components
health_component_t* db, *cache;
health_register_component(health, "database", &db);
health_register_component(health, "cache", &cache);

// Update health
health_update_status(db, HEALTH_UP, "Connected");
health_update_status(cache, HEALTH_DEGRADED, "High memory usage");

// Get overall status
health_status_t status = health_get_overall_status(health);

// Export as JSON
char* json;
size_t size;
health_export_json(health, &json, &size);
```

### 5. HTTP Server

Minimal HTTP server for observability endpoints.

```c
obs_server_t* server;
obs_server_init(&server, 9090, metrics, health);

// Endpoints automatically available:
// GET /metrics        - Prometheus metrics
// GET /health/live    - Liveness probe (always returns 200)
// GET /health/ready   - Readiness probe (returns 200/503 based on health)
```

**Endpoints**:
- `/metrics`: Prometheus text format metrics
- `/health/live`: Always returns `{"status":"UP"}` (200 OK)
- `/health/ready`: Returns component health status (200 if all UP, 503 otherwise)

## Performance Characteristics

### Metrics
- Counter increment: **10-20 ns**
- Gauge set: **15-25 ns**
- Histogram observe: **30-50 ns**
- Prometheus export: **~1ms** for 100 metrics
- Multi-threaded: Linear scaling up to 8+ cores

### Logging
- Sync mode: **5-10 μs** per log
- Async mode: **<1 μs** per log
- Throughput: **>100K logs/sec** (async, single thread)
- Multi-threaded: **>500K logs/sec** (async, 8 threads)

### Tracing
- Span creation: **~50 ns**
- Tag addition: **~10 ns**
- Context injection/extraction: **~100 ns**
- Export: Batched every 5s or 1000 spans

### HTTP Server
- Request handling: **<1ms** for metrics/health endpoints
- Concurrent connections: **10 simultaneous** (configurable)

## Memory Usage

- Metrics registry: **~256 KB** (1024 metrics)
- Logger (async): **~64 KB** (ring buffer)
- Thread-local log buffer: **4 KB** per thread
- Tracer: **~256 KB** (span buffer)
- Health registry: **~64 KB** (64 components)
- HTTP server: **<1 MB** (includes buffers)

**Total**: ~600 KB base footprint

## Integration Test

Run the full Phase 0 integration test:

```bash
make all
./build/libs/distric_obs/tests/test_integration
```

This test demonstrates:
1. All observability systems working together
2. Concurrent workers generating metrics, logs, and traces
3. Health status changes
4. HTTP endpoints serving live data

## API Reference

See [API.md](API.md) for detailed API documentation.

## Thread Safety Guarantees

- **Metrics**: All updates are atomic and lock-free
- **Logging**: Thread-safe with thread-local buffers
- **Tracing**: Thread-safe span operations
- **Health**: Thread-safe status updates
- **HTTP Server**: Handles concurrent requests safely

## Configuration

### Build Options

```cmake
# Custom limits (edit headers and rebuild)
MAX_METRICS              1024   # Maximum metrics in registry
MAX_HEALTH_COMPONENTS    64     # Maximum health components
MAX_SPANS_BUFFER         1000   # Span buffer size
RING_BUFFER_SIZE         8192   # Log ring buffer size
```

### Runtime Configuration

```c
// Metrics: No runtime config needed

// Logging: Choose sync or async mode
log_init(&logger, fd, LOG_MODE_ASYNC);  // or LOG_MODE_SYNC

// Tracing: Configure export interval (in code)
#define SPAN_EXPORT_INTERVAL_MS 5000

// HTTP Server: Set port (0 for auto-assignment)
obs_server_init(&server, 9090, metrics, health);
```

## Validation

Run full validation suite:

```bash
# Build and test
make all test

# Memory leak check
make valgrind

# Performance benchmarks
make bench

# Full validation script
chmod +x validate.sh
./validate.sh
```

## Implementation Status

- [x] **Phase 0.1**: Error handling + metrics system
- [x] **Phase 0.2**: Structured logging
- [x] **Phase 0.3**: Distributed tracing
- [x] **Phase 0.4**: Health monitoring + HTTP server
- [x] **Phase 0.5**: Integration testing

**Phase 0 Complete** ✓

## Future Enhancements

Phase 1 considerations:
- gRPC support for remote telemetry
- Advanced sampling strategies for tracing
- Metric aggregation and downsampling
- Distributed health checks
- TLS support for HTTP server
- Authentication/authorization

## Examples

### Complete HTTP Service

```c
#include <distric_obs.h>
#include <distric_obs/tracing.h>
#include <distric_obs/health.h>
#include <distric_obs/http_server.h>

int main() {
    // Initialize observability
    metrics_registry_t* metrics;
    logger_t* logger;
    tracer_t* tracer;
    health_registry_t* health;
    
    metrics_init(&metrics);
    log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);
    trace_init(&tracer, export_to_backend, NULL);
    health_init(&health);
    
    // Register metrics
    metric_t* requests, *latency;
    metrics_register_counter(metrics, "http_requests_total", "Requests", NULL, 0, &requests);
    metrics_register_histogram(metrics, "http_latency_seconds", "Latency", NULL, 0, &latency);
    
    // Register health
    health_component_t* api_health;
    health_register_component(health, "api", &api_health);
    health_update_status(api_health, HEALTH_UP, "Ready");
    
    // Start observability server
    obs_server_t* obs_server;
    obs_server_init(&obs_server, 9090, metrics, health);
    
    LOG_INFO(logger, "main", "Service started", "port", "9090");
    
    // Your application logic here...
    
    // Cleanup
    obs_server_destroy(obs_server);
    trace_destroy(tracer);
    log_destroy(logger);
    health_destroy(health);
    metrics_destroy(metrics);
    
    return 0;
}
```

### Fetch Metrics with curl

```bash
# Get Prometheus metrics
curl http://localhost:9090/metrics

# Check health
curl http://localhost:9090/health/ready
curl http://localhost:9090/health/live
```

## Contributing

This is Phase 0 of DistriC 2.0. Contributions welcome!

## License

TBD