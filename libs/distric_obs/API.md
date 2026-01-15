# DistriC Observability Library - API Reference

## Error Handling

### Error Codes

```c
typedef enum {
    DISTRIC_OK = 0,                    // Success
    DISTRIC_ERR_INVALID_ARG = -1,      // Invalid argument
    DISTRIC_ERR_ALLOC_FAILURE = -2,    // Memory allocation failed
    DISTRIC_ERR_BUFFER_OVERFLOW = -3,  // Buffer overflow
    DISTRIC_ERR_REGISTRY_FULL = -4,    // Metric registry full
    DISTRIC_ERR_NOT_FOUND = -5,        // Resource not found
    DISTRIC_ERR_INIT_FAILED = -6,      // Initialization failed
} distric_err_t;
```

### Functions

#### `distric_strerror`
```c
const char* distric_strerror(distric_err_t err);
```
Convert error code to human-readable string.

**Parameters:**
- `err`: Error code

**Returns:** Static string describing the error

---

## Metrics System

### Types

```c
typedef enum {
    METRIC_TYPE_COUNTER,    // Monotonically increasing value
    METRIC_TYPE_GAUGE,      // Point-in-time value
    METRIC_TYPE_HISTOGRAM,  // Distribution with buckets
} metric_type_t;
```

### Constants

```c
#define MAX_METRIC_LABELS 8      // Maximum labels per metric
#define MAX_LABEL_KEY_LEN 64     // Maximum label key length
#define MAX_LABEL_VALUE_LEN 128  // Maximum label value length
```

### Structures

#### `metric_label_t`
```c
typedef struct {
    char key[MAX_LABEL_KEY_LEN];
    char value[MAX_LABEL_VALUE_LEN];
} metric_label_t;
```

### Initialization

#### `metrics_init`
```c
distric_err_t metrics_init(metrics_registry_t** registry);
```
Initialize a new metrics registry.

**Parameters:**
- `registry`: Pointer to receive registry pointer

**Returns:** `DISTRIC_OK` on success, error code otherwise

**Example:**
```c
metrics_registry_t* metrics;
if (metrics_init(&metrics) != DISTRIC_OK) {
    // Handle error
}
```

#### `metrics_destroy`
```c
void metrics_destroy(metrics_registry_t* registry);
```
Destroy metrics registry and free all resources.

**Parameters:**
- `registry`: Registry to destroy

**Thread Safety:** Must not be called concurrently with metric operations

### Registration

#### `metrics_register_counter`
```c
distric_err_t metrics_register_counter(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_t* labels,
    size_t label_count,
    metric_t** out_metric
);
```
Register a new counter metric.

**Parameters:**
- `registry`: Metrics registry
- `name`: Metric name (max 128 chars)
- `help`: Help text (max 256 chars)
- `labels`: Array of labels (can be NULL)
- `label_count`: Number of labels (max 8)
- `out_metric`: Pointer to receive metric handle

**Returns:** `DISTRIC_OK` on success

**Thread Safety:** Thread-safe

**Example:**
```c
metric_t* requests;
metric_label_t labels[] = {{"method", "GET"}};
metrics_register_counter(metrics, "requests_total", 
                        "Total requests", labels, 1, &requests);
```

#### `metrics_register_gauge`
```c
distric_err_t metrics_register_gauge(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_t* labels,
    size_t label_count,
    metric_t** out_metric
);
```
Register a new gauge metric.

**Parameters:** Same as `metrics_register_counter`

**Returns:** `DISTRIC_OK` on success

**Thread Safety:** Thread-safe

#### `metrics_register_histogram`
```c
distric_err_t metrics_register_histogram(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_t* labels,
    size_t label_count,
    metric_t** out_metric
);
```
Register a new histogram metric with fixed buckets.

**Buckets:** 0.001, 0.01, 0.1, 1, 10, 100, 1000, 10000, 100000, +Inf

**Parameters:** Same as `metrics_register_counter`

**Returns:** `DISTRIC_OK` on success

**Thread Safety:** Thread-safe

### Metric Operations

#### `metrics_counter_inc`
```c
void metrics_counter_inc(metric_t* metric);
```
Increment counter by 1 (atomic, lock-free).

**Parameters:**
- `metric`: Counter metric handle

**Thread Safety:** Thread-safe, lock-free

**Performance:** ~10-20 ns per operation

#### `metrics_counter_add`
```c
void metrics_counter_add(metric_t* metric, uint64_t value);
```
Increment counter by specified value (atomic, lock-free).

**Parameters:**
- `metric`: Counter metric handle
- `value`: Amount to add

**Thread Safety:** Thread-safe, lock-free

#### `metrics_gauge_set`
```c
void metrics_gauge_set(metric_t* metric, double value);
```
Set gauge to value (atomic, lock-free).

**Parameters:**
- `metric`: Gauge metric handle
- `value`: New value

**Thread Safety:** Thread-safe, lock-free

**Performance:** ~15-25 ns per operation

#### `metrics_histogram_observe`
```c
void metrics_histogram_observe(metric_t* metric, double value);
```
Record observation in histogram (atomic, lock-free).

**Parameters:**
- `metric`: Histogram metric handle
- `value`: Observed value

**Thread Safety:** Thread-safe, lock-free

**Performance:** ~30-50 ns per operation

### Export

#### `metrics_export_prometheus`
```c
distric_err_t metrics_export_prometheus(
    metrics_registry_t* registry,
    char** out_buffer,
    size_t* out_size
);
```
Export all metrics in Prometheus text format.

**Parameters:**
- `registry`: Metrics registry
- `out_buffer`: Pointer to receive allocated buffer
- `out_size`: Pointer to receive buffer size

**Returns:** `DISTRIC_OK` on success

**Memory:** Caller must free returned buffer

**Thread Safety:** Thread-safe for reading metrics

**Example:**
```c
char* output;
size_t size;
if (metrics_export_prometheus(metrics, &output, &size) == DISTRIC_OK) {
    write(fd, output, size);
    free(output);
}
```

---

## Logging System

### Types

```c
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_FATAL = 4,
} log_level_t;

typedef enum {
    LOG_MODE_SYNC,   // Direct write to file descriptor
    LOG_MODE_ASYNC,  // Write to ring buffer, flush by thread
} log_mode_t;
```

### Initialization

#### `log_init`
```c
distric_err_t log_init(logger_t** logger, int fd, log_mode_t mode);
```
Initialize a new logger.

**Parameters:**
- `logger`: Pointer to receive logger handle
- `fd`: File descriptor for output (e.g., STDOUT_FILENO)
- `mode`: Logging mode (sync or async)

**Returns:** `DISTRIC_OK` on success

**Thread Safety:** Thread-safe after initialization

**Example:**
```c
logger_t* logger;
log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);
```

#### `log_destroy`
```c
void log_destroy(logger_t* logger);
```
Destroy logger and flush all pending logs.

**Parameters:**
- `logger`: Logger to destroy

**Thread Safety:** Must not be called concurrently with log writes

**Important:** Always call to ensure pending logs are flushed

### Logging

#### `log_write`
```c
distric_err_t log_write(
    logger_t* logger,
    log_level_t level,
    const char* component,
    const char* message,
    ...  /* key1, value1, key2, value2, ..., NULL */
);
```
Write a log entry with key-value pairs.

**Parameters:**
- `logger`: Logger handle
- `level`: Log level
- `component`: Component name
- `message`: Log message
- `...`: NULL-terminated list of key-value string pairs

**Returns:** `DISTRIC_OK` on success

**Thread Safety:** Thread-safe, uses thread-local buffers

**Memory:** No malloc in hot path

**Example:**
```c
log_write(logger, LOG_LEVEL_INFO, "http", "Request received",
          "method", "GET", "path", "/api", NULL);
```

### Macros

```c
LOG_DEBUG(logger, component, message, ...)
LOG_INFO(logger, component, message, ...)
LOG_WARN(logger, component, message, ...)
LOG_ERROR(logger, component, message, ...)
LOG_FATAL(logger, component, message, ...)
```

Convenience macros for logging at specific levels.

**Example:**
```c
LOG_INFO(logger, "database", "Connected",
         "host", "localhost", "port", "5432");
```

### Output Format

All logs are output as JSON with the following structure:

```json
{
  "timestamp": 1704067200000,
  "level": "INFO",
  "component": "http",
  "message": "Request received",
  "method": "GET",
  "path": "/api/users"
}
```

**Fields:**
- `timestamp`: Unix timestamp in milliseconds
- `level`: Log level string
- `component`: Component name
- `message`: Log message
- Additional fields from key-value pairs

**Escaping:** Special characters (quotes, backslashes, newlines) are properly escaped

---

## Thread Safety Guarantees

### Metrics
- All metric updates are atomic and lock-free
- Safe to call from any thread concurrently
- Registry operations (register/destroy) should not overlap

### Logging
- All log operations are thread-safe
- Uses thread-local buffers (4KB per thread)
- Async mode uses lock-free ring buffer
- Logger destroy must not overlap with writes

---

## Performance Characteristics

### Metrics
- Counter increment: 10-20 ns
- Gauge set: 15-25 ns
- Histogram observe: 30-50 ns
- Prometheus export: ~1ms for 100 metrics

### Logging
- Sync mode: 5-10 μs per log
- Async mode: <1 μs per log
- Throughput: >100,000 logs/sec (async)
- Multi-threaded: >500,000 logs/sec

### Memory
- Metrics registry: ~256 KB (1024 metrics)
- Logger (async): ~64 KB (ring buffer)
- Thread-local log buffer: 4 KB per thread

---

## Limits and Constants

```c
#define MAX_METRICS 1024              // Maximum metrics in registry
#define MAX_METRIC_NAME_LEN 128       // Maximum metric name length
#define MAX_METRIC_HELP_LEN 256       // Maximum help text length
#define MAX_METRIC_LABELS 8           // Maximum labels per metric
#define MAX_LABEL_KEY_LEN 64          // Maximum label key length
#define MAX_LABEL_VALUE_LEN 128       // Maximum label value length
#define HISTOGRAM_BUCKET_COUNT 10     // Number of histogram buckets
#define LOG_BUFFER_SIZE 4096          // Thread-local log buffer
#define RING_BUFFER_SIZE 8192         // Async log ring buffer
```

To modify these limits, edit the header files and recompile.