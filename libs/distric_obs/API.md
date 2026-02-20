# distric_obs API Reference

Version: current  
Header: `#include <distric_obs.h>`

All subsystem-specific types are exposed through the single top-level header. Do not include sub-headers directly.

---

## Error Codes

All functions that can fail return `distric_err_t`.

| Code | Value | Meaning |
|------|-------|---------|
| `DISTRIC_OK` | 0 | Success |
| `DISTRIC_ERR_INVALID_ARG` | — | NULL or out-of-range argument |
| `DISTRIC_ERR_NO_MEMORY` | — | Allocation failure |
| `DISTRIC_ERR_FULL` | — | Registry or buffer capacity reached |
| `DISTRIC_ERR_DROPPED` | — | Data dropped due to backpressure (informational) |
| `DISTRIC_ERR_NOT_FOUND` | — | Named item does not exist |

Observability failures must not affect application correctness. Error codes are informational; callers may safely ignore them.

---

## Metrics

### Types

```c
typedef struct metrics_registry metrics_registry_t;
typedef struct metric            metric_t;

typedef struct {
    const char* key;
    const char* value;
} metric_label_t;
```

### `metrics_init`

```c
distric_err_t metrics_init(metrics_registry_t** registry);
```

Allocate and initialise a metrics registry.

**Parameters**
- `registry` — receives the allocated registry handle.

**Returns** `DISTRIC_OK` on success.

**Thread safety** — not concurrent with itself; call once at startup.

---

### `metrics_destroy`

```c
void metrics_destroy(metrics_registry_t* registry);
```

Free all resources held by the registry.

**Thread safety** — must not be called concurrently with any metric operation.

---

### `metrics_register_counter`

```c
distric_err_t metrics_register_counter(
    metrics_registry_t*    registry,
    const char*            name,
    const char*            help,
    const metric_label_t*  labels,
    size_t                 label_count,
    metric_t**             out_metric
);
```

Register a monotonically increasing counter.

**Parameters**
- `registry` — target registry.
- `name` — metric name, max 128 bytes.
- `help` — human-readable description, max 256 bytes.
- `labels` — array of label key/value pairs, or NULL.
- `label_count` — number of labels; max 8.
- `out_metric` — receives the metric handle.

**Returns** `DISTRIC_OK` on success, `DISTRIC_ERR_FULL` if the registry is at capacity.

**Thread safety** — thread-safe; serialised internally.

---

### `metrics_register_gauge`

```c
distric_err_t metrics_register_gauge(
    metrics_registry_t*    registry,
    const char*            name,
    const char*            help,
    const metric_label_t*  labels,
    size_t                 label_count,
    metric_t**             out_metric
);
```

Register a gauge that can increase or decrease.

**Parameters** — same as `metrics_register_counter`.

**Returns** `DISTRIC_OK` on success.

---

### `metrics_register_histogram`

```c
distric_err_t metrics_register_histogram(
    metrics_registry_t*    registry,
    const char*            name,
    const char*            help,
    const metric_label_t*  labels,
    size_t                 label_count,
    metric_t**             out_metric
);
```

Register a histogram with fixed buckets: 0.001, 0.01, 0.1, 1, 10, 100, 1 000, 10 000, 100 000, +Inf.

**Parameters** — same as `metrics_register_counter`.

**Returns** `DISTRIC_OK` on success.

---

### `metrics_counter_inc`

```c
void metrics_counter_inc(metric_t* metric);
```

Atomically increment a counter by 1.

**Performance** ~10–20 ns. Lock-free.

---

### `metrics_counter_add`

```c
void metrics_counter_add(metric_t* metric, uint64_t value);
```

Atomically add `value` to a counter.

---

### `metrics_gauge_set`

```c
void metrics_gauge_set(metric_t* metric, double value);
```

Atomically set a gauge to `value`.

**Performance** ~15–25 ns. Lock-free.

---

### `metrics_histogram_observe`

```c
void metrics_histogram_observe(metric_t* metric, double value);
```

Record an observation in a histogram.

**Performance** ~30–50 ns. Lock-free.

---

### `metrics_export_prometheus`

```c
distric_err_t metrics_export_prometheus(
    metrics_registry_t*  registry,
    char**               out_buffer,
    size_t*              out_size
);
```

Export all metrics in Prometheus text format.

**Parameters**
- `out_buffer` — receives a heap-allocated buffer. Caller must `free()` it.
- `out_size` — receives the buffer length.

**Returns** `DISTRIC_OK` on success.

**Thread safety** — safe for concurrent reads.

**Example**

```c
char*  buf;
size_t size;
if (metrics_export_prometheus(metrics, &buf, &size) == DISTRIC_OK) {
    write(fd, buf, size);
    free(buf);
}
```

---

## Logging

### Types

```c
typedef struct logger logger_t;

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_FATAL = 4,
} log_level_t;

typedef enum {
    LOG_MODE_SYNC,   /* direct write; per-write mutex */
    LOG_MODE_ASYNC,  /* ring buffer; background thread */
} log_mode_t;
```

### `log_init`

```c
distric_err_t log_init(logger_t** logger, int fd, log_mode_t mode);
```

Allocate and initialise a logger.

**Parameters**
- `logger` — receives the logger handle.
- `fd` — output file descriptor (e.g. `STDOUT_FILENO`).
- `mode` — `LOG_MODE_ASYNC` is recommended for production.

**Returns** `DISTRIC_OK` on success.

---

### `log_destroy`

```c
void log_destroy(logger_t* logger);
```

Flush all pending log entries and free resources.

**Important** — always call before process exit to avoid data loss.

---

### `log_write`

```c
distric_err_t log_write(
    logger_t*   logger,
    log_level_t level,
    const char* component,
    const char* message,
    ...   /* key1, val1, key2, val2, ..., NULL */
);
```

Write a structured JSON log entry.

**Parameters**
- `component` — subsystem name, e.g. `"tcp"`.
- `message` — human-readable description.
- `...` — zero or more key/value string pairs terminated by NULL.

**Returns** `DISTRIC_OK` on success, `DISTRIC_ERR_DROPPED` if the ring buffer is full (async mode only).

---

### Logging Macros

Convenience macros that pass `__FILE__` and `__LINE__` automatically.

```c
LOG_DEBUG(logger, component, message, ...)
LOG_INFO (logger, component, message, ...)
LOG_WARN (logger, component, message, ...)
LOG_ERROR(logger, component, message, ...)
LOG_FATAL(logger, component, message, ...)
```

**Example**

```c
LOG_INFO(logger, "db", "Connected", "host", "localhost", "port", "5432");
LOG_ERROR(logger, "api", "Timeout", "duration_ms", "5000", NULL);
```

**Output format**

```json
{"ts":1705147845123,"level":"INFO","component":"db","msg":"Connected","host":"localhost","port":"5432"}
```

---

## Distributed Tracing

Include `<distric_obs/tracing.h>` in addition to `<distric_obs.h>`.

### Types

```c
typedef struct tracer     tracer_t;
typedef struct trace_span trace_span_t;

typedef struct {
    char trace_id[33];
    char span_id[17];
} trace_context_t;

typedef void (*span_export_fn)(trace_span_t* spans, size_t count, void* userdata);
```

### `trace_init`

```c
distric_err_t trace_init(tracer_t** tracer, span_export_fn export_fn, void* userdata);
```

Initialise a tracer. `export_fn` is called periodically on a background thread with completed spans.

---

### `trace_destroy`

```c
void trace_destroy(tracer_t* tracer);
```

Flush remaining spans, invoke the export callback, and free resources.

---

### `trace_start_span`

```c
distric_err_t trace_start_span(tracer_t* tracer, const char* operation, trace_span_t** span);
```

Start a new root span.

**Performance** ~50 ns. Lock-free.

---

### `trace_start_child_span`

```c
distric_err_t trace_start_child_span(
    tracer_t*     tracer,
    trace_span_t* parent,
    const char*   operation,
    trace_span_t** span
);
```

Start a child span that inherits the trace ID from `parent`.

---

### `trace_finish_span`

```c
void trace_finish_span(tracer_t* tracer, trace_span_t* span);
```

Record end time and enqueue for export.

---

### `trace_add_tag`

```c
distric_err_t trace_add_tag(trace_span_t* span, const char* key, const char* value);
```

Attach a string key/value tag to a span.

---

### `trace_inject_context`

```c
distric_err_t trace_inject_context(trace_span_t* span, char* header_out, size_t len);
```

Serialise the span context into a propagation header for transmission to a remote service.

---

### `trace_extract_context`

```c
distric_err_t trace_extract_context(const char* header, trace_context_t* ctx);
```

Deserialise a propagation header received from a remote caller.

---

### `trace_start_span_from_context`

```c
distric_err_t trace_start_span_from_context(
    tracer_t*            tracer,
    const trace_context_t* ctx,
    const char*          operation,
    trace_span_t**       span
);
```

Start a span that continues a remote trace identified by `ctx`.

---

## Health Monitoring

Include `<distric_obs/health.h>` in addition to `<distric_obs.h>`.

### Types

```c
typedef struct health_registry  health_registry_t;
typedef struct health_component health_component_t;

typedef enum {
    HEALTH_UP       = 0,
    HEALTH_DEGRADED = 1,
    HEALTH_DOWN     = 2,
} health_status_t;
```

### `health_init`

```c
distric_err_t health_init(health_registry_t** health);
```

---

### `health_destroy`

```c
void health_destroy(health_registry_t* health);
```

---

### `health_register_component`

```c
distric_err_t health_register_component(
    health_registry_t*   health,
    const char*          name,
    health_component_t** component
);
```

Register a named component. Maximum components: `MAX_HEALTH_COMPONENTS` (default 64).

---

### `health_update_status`

```c
distric_err_t health_update_status(
    health_component_t* component,
    health_status_t     status,
    const char*         message
);
```

Update component status. Uses a single CAS — lock-free.

---

### `health_get_overall_status`

```c
health_status_t health_get_overall_status(health_registry_t* health);
```

Return the worst status across all registered components.

---

### `health_export_json`

```c
distric_err_t health_export_json(
    health_registry_t* health,
    char**             out_buffer,
    size_t*            out_size
);
```

Export health state as JSON. Caller must `free()` the returned buffer.

---

## HTTP Server

Include `<distric_obs/http_server.h>` in addition to `<distric_obs.h>`.

### Types

```c
typedef struct obs_server obs_server_t;
```

### `obs_server_init`

```c
distric_err_t obs_server_init(
    obs_server_t**     server,
    uint16_t           port,
    metrics_registry_t* metrics,
    health_registry_t* health
);
```

Start the HTTP server on the given port. Pass `0` for automatic port assignment.

**Endpoints**

| Path | Description |
|------|-------------|
| `GET /metrics` | Prometheus text format metrics |
| `GET /health/ready` | Readiness probe |
| `GET /health/live` | Liveness probe |

---

### `obs_server_destroy`

```c
void obs_server_destroy(obs_server_t* server);
```

Stop the server and free resources.

---

## Initialisation Order

```
metrics_init  -->  log_init  -->  trace_init  -->  health_init  -->  obs_server_init
```

Teardown must occur in reverse order.
