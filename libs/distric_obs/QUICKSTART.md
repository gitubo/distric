# DistriC Observability - Quick Start Guide

## Installation

**Build from project root:**
```bash
# Clone/navigate to project root
cd /path/to/distric

# Build everything
make all

# Run tests
make test

# Optional: install system-wide
sudo make install
```

**Build artifacts:**
- Static lib: `build/libs/distric_obs/libdistric_obs.a`
- Shared lib: `build/libs/distric_obs/libdistric_obs.so`
- Tests: `build/libs/distric_obs/tests/`

## 10-Minute Complete Tutorial

### 1. Include the headers

```c
#include <distric_obs.h>
#include <distric_obs/tracing.h>
#include <distric_obs/health.h>
#include <distric_obs/http_server.h>
```

### 2. Initialize the Observability Stack

```c
// Initialize all components
metrics_registry_t* metrics;
logger_t* logger;
tracer_t* tracer;
health_registry_t* health;

metrics_init(&metrics);
log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);
trace_init(&tracer, trace_export_callback, NULL);
health_init(&health);
```

### 3. Metrics Example

```c
// Register metrics
metric_t* requests;
metric_t* latency;
metric_t* cpu_usage;

metric_label_t labels[] = {{"service", "api"}};

metrics_register_counter(metrics, "requests_total", 
                        "Total requests", labels, 1, &requests);
metrics_register_histogram(metrics, "request_latency_seconds",
                           "Request latency", labels, 1, &latency);
metrics_register_gauge(metrics, "cpu_usage_percent",
                      "CPU usage", NULL, 0, &cpu_usage);

// Use metrics (thread-safe, lock-free)
metrics_counter_inc(requests);
metrics_histogram_observe(latency, 0.042);  // 42ms
metrics_gauge_set(cpu_usage, 65.3);
```

### 4. Logging Example

```c
// Write structured logs
LOG_INFO(logger, "app", "Application started", 
         "version", "1.0.0");

LOG_ERROR(logger, "database", "Connection failed",
          "host", "localhost",
          "port", "5432",
          "error", "timeout");
```

### 5. Distributed Tracing Example

```c
// Define export callback
void trace_export_callback(trace_span_t* spans, size_t count, void* user_data) {
    // Send spans to your backend (Jaeger, Zipkin, etc.)
    for (size_t i = 0; i < count; i++) {
        printf("Span: %s, duration: %lu ns\n", 
               spans[i].operation,
               spans[i].end_time_ns - spans[i].start_time_ns);
    }
}

// Start a trace
trace_span_t* span;
trace_start_span(tracer, "handle_request", &span);
trace_add_tag(span, "http.method", "GET");
trace_add_tag(span, "http.url", "/api/users");

// Do work...

// Start child span
trace_span_t* child;
trace_start_child_span(tracer, span, "database_query", &child);
// Query database...
trace_finish_span(tracer, child);

// Finish parent span
trace_set_status(span, SPAN_STATUS_OK);
trace_finish_span(tracer, span);
```

### 6. Health Monitoring Example

```c
// Register health components
health_component_t* db_health;
health_component_t* cache_health;

health_register_component(health, "database", &db_health);
health_register_component(health, "cache", &cache_health);

// Update health status
health_update_status(db_health, HEALTH_UP, "Connected");
health_update_status(cache_health, HEALTH_DEGRADED, "High latency");

// Get overall health
health_status_t overall = health_get_overall_status(health);
printf("System health: %s\n", health_status_str(overall));
```

### 7. Start HTTP Server

```c
// Start observability HTTP server
obs_server_t* server;
obs_server_init(&server, 9090, metrics, health);

uint16_t port = obs_server_get_port(server);
printf("Observability server running on port %u\n", port);

// Server automatically exposes:
// - GET /metrics       : Prometheus metrics
// - GET /health/live   : Liveness check
// - GET /health/ready  : Readiness check
```

### 8. Access Observability Data

```bash
# Get Prometheus metrics
curl http://localhost:9090/metrics

# Check liveness (always returns UP)
curl http://localhost:9090/health/live

# Check readiness (reflects component health)
curl http://localhost:9090/health/ready
```

### 9. Context Propagation (Cross-Service Tracing)

```c
// Service A: Inject context into header
trace_span_t* span;
trace_start_span(tracer, "service_a_operation", &span);

char trace_header[256];
trace_inject_context(span, trace_header, sizeof(trace_header));

// Send trace_header to Service B via HTTP/RPC...

// Service B: Extract context and create child span
trace_context_t context;
trace_extract_context(trace_header, &context);

trace_span_t* child_span;
trace_start_span_from_context(tracer, &context, "service_b_operation", &child_span);
// Do work...
trace_finish_span(tracer, child_span);
```

### 10. Complete Cleanup

```c
// Cleanup (flushes all pending data)
obs_server_destroy(server);
trace_destroy(tracer);  // Flushes remaining spans
log_destroy(logger);    // Flushes remaining logs
health_destroy(health);
metrics_destroy(metrics);
```

## Common Patterns

### Pattern 1: HTTP Request Tracking

```c
void handle_http_request(const char* method, const char* path) {
    // Start trace
    trace_span_t* span;
    trace_start_span(tracer, "http_request", &span);
    trace_add_tag(span, "http.method", method);
    trace_add_tag(span, "http.path", path);
    
    uint64_t start = get_time_ns();
    
    // Log request
    LOG_INFO(logger, "http", "Request received",
             "method", method,
             "path", path);
    
    // Process request
    // ...
    
    // Update metrics
    metrics_counter_inc(request_counter);
    
    uint64_t duration_ns = get_time_ns() - start;
    double duration_s = duration_ns / 1e9;
    metrics_histogram_observe(latency_histogram, duration_s);
    
    // Finish trace
    trace_set_status(span, SPAN_STATUS_OK);
    trace_finish_span(tracer, span);
    
    LOG_INFO(logger, "http", "Request completed",
             "duration_ms", duration_s * 1000);
}
```

### Pattern 2: Database Operation with Health Check

```c
bool execute_query(const char* query) {
    // Start child span
    trace_span_t* span;
    trace_span_t* parent = trace_get_active_span();
    if (parent) {
        trace_start_child_span(tracer, parent, "db_query", &span);
    } else {
        trace_start_span(tracer, "db_query", &span);
    }
    trace_add_tag(span, "db.query", query);
    
    bool success = false;
    
    // Execute query
    if (db_execute(query)) {
        success = true;
        health_update_status(db_health, HEALTH_UP, "Query successful");
        trace_set_status(span, SPAN_STATUS_OK);
    } else {
        health_update_status(db_health, HEALTH_DOWN, "Query failed");
        trace_set_status(span, SPAN_STATUS_ERROR);
        LOG_ERROR(logger, "database", "Query failed", "query", query);
    }
    
    trace_finish_span(tracer, span);
    return success;
}
```

### Pattern 3: Background Worker with Monitoring

```c
void* worker_thread(void* arg) {
    while (running) {
        // Update active worker gauge
        metrics_gauge_set(active_workers, get_worker_count());
        
        // Start trace for work unit
        trace_span_t* span;
        trace_start_span(tracer, "worker_task", &span);
        
        // Do work
        process_task();
        
        // Update metrics
        metrics_counter_inc(tasks_completed);
        
        trace_finish_span(tracer, span);
        
        // Update health
        health_update_status(worker_health, HEALTH_UP, "Processing");
    }
    
    return NULL;
}
```

## Integration Test

Run the complete integration test:

```bash
# Build and run
make all
./build/libs/distric_obs/tests/test_integration

# Access observability data during test
curl http://localhost:<port>/metrics
curl http://localhost:<port>/health/ready
```

## Performance Tips

1. **Metrics**: Use counters for rates, gauges for current values, histograms for distributions
2. **Logging**: Use async mode for high-throughput scenarios (>1K logs/sec)
3. **Tracing**: Batch exports reduce overhead; adjust `SPAN_EXPORT_INTERVAL_MS` if needed
4. **Labels**: Keep label count low (≤3) for better performance
5. **Thread Safety**: All operations are thread-safe, no locks needed in your code

## Configuration

### Compile-Time Limits

Edit headers and rebuild to change limits:

```c
// In metrics.h
#define MAX_METRICS 1024

// In logging.h
#define RING_BUFFER_SIZE 8192

// In tracing.h
#define MAX_SPANS_BUFFER 1000

// In health.h
#define MAX_HEALTH_COMPONENTS 64
```

### Runtime Configuration

```c
// Logger mode
log_init(&logger, fd, LOG_MODE_ASYNC);  // or LOG_MODE_SYNC

// HTTP server port (0 = auto-assign)
obs_server_init(&server, 9090, metrics, health);

// Trace export callback
trace_init(&tracer, your_export_function, your_data);
```

## Troubleshooting

**Problem**: Logs missing in async mode  
**Solution**: Call `log_destroy()` to flush pending logs

**Problem**: Spans not exported  
**Solution**: Ensure tracer stays alive long enough (5s export interval)

**Problem**: Registry full error  
**Solution**: Increase `MAX_METRICS` in `metrics.h` and recompile

**Problem**: HTTP server won't start  
**Solution**: Check if port is already in use, or use port 0 for auto-assignment

**Problem**: Health endpoint returns 503  
**Solution**: Check component health status; at least one is DOWN/DEGRADED

## Next Steps

- Read [README.md](README.md) for detailed documentation
- Check [API.md](API.md) for complete API reference
- Run `make bench` to see performance characteristics
- Run `make valgrind` for memory leak validation
- Run integration test: `./build/libs/distric_obs/tests/test_integration`

## Support

For issues or questions, refer to the main DistriC 2.0 documentation.

## Phase 0 Complete

All Phase 0 components are now implemented:
- ✓ Metrics (0.1)
- ✓ Logging (0.2)
- ✓ Tracing (0.3)
- ✓ Health & HTTP Server (0.4)
- ✓ Integration (0.5)

Ready for Phase 1!