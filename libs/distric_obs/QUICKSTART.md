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

## 5-Minute Tutorial

### 1. Include the header

```c
#include <distric_obs.h>
```

### 2. Metrics Example

```c
// Initialize
metrics_registry_t* metrics;
metrics_init(&metrics);

// Register metrics
metric_t* requests;
metric_t* latency;
metric_t* cpu_usage;

metrics_register_counter(metrics, "requests_total", 
                        "Total requests", NULL, 0, &requests);
metrics_register_histogram(metrics, "request_latency_seconds",
                           "Request latency", NULL, 0, &latency);
metrics_register_gauge(metrics, "cpu_usage_percent",
                      "CPU usage", NULL, 0, &cpu_usage);

// Use metrics (thread-safe, lock-free)
metrics_counter_inc(requests);
metrics_histogram_observe(latency, 0.042);  // 42ms
metrics_gauge_set(cpu_usage, 65.3);

// Export to Prometheus
char* output;
size_t size;
metrics_export_prometheus(metrics, &output, &size);
printf("%s", output);
free(output);

// Cleanup
metrics_destroy(metrics);
```

### 3. Logging Example

```c
#include <unistd.h>

// Initialize logger
logger_t* logger;
log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);

// Write logs
LOG_INFO(logger, "app", "Application started", 
         "version", "1.0.0");

LOG_ERROR(logger, "database", "Connection failed",
          "host", "localhost",
          "port", "5432",
          "error", "timeout");

// Cleanup (flushes pending logs)
log_destroy(logger);
```

### 4. With Labels

```c
// Metrics with labels
metric_label_t labels[] = {
    {"method", "GET"},
    {"endpoint", "/api/users"},
    {"status", "200"}
};

metric_t* http_requests;
metrics_register_counter(metrics, "http_requests_total",
                        "HTTP requests", labels, 3, &http_requests);

metrics_counter_inc(http_requests);
```

### 5. Complete Integration Test

See `libs/distric_obs/tests/test_distric_obs.c` for a complete working example with concurrent workers.

```bash
# Run from project root
make all
./build/libs/distric_obs/tests/test_distric_obs
```

## Common Patterns

### Pattern 1: Request Tracking

```c
void handle_request(const char* path) {
    uint64_t start = get_time_ns();
    
    // Process request
    // ...
    
    uint64_t duration_ns = get_time_ns() - start;
    double duration_s = duration_ns / 1e9;
    
    metrics_counter_inc(request_counter);
    metrics_histogram_observe(request_duration, duration_s);
    
    LOG_INFO(logger, "http", "Request completed",
             "path", path,
             "duration_ms", duration_s * 1000);
}
```

### Pattern 2: Resource Monitoring

```c
void update_system_metrics() {
    metrics_gauge_set(memory_usage, get_memory_usage_mb());
    metrics_gauge_set(cpu_usage, get_cpu_percent());
    metrics_gauge_set(disk_usage, get_disk_usage_percent());
}
```

### Pattern 3: Error Tracking

```c
void handle_error(const char* component, const char* error) {
    metrics_counter_inc(error_counter);
    
    LOG_ERROR(logger, component, "Error occurred",
              "error", error,
              "timestamp", get_timestamp());
}
```

## Performance Tips

1. **Metrics**: Use counters for rates, gauges for current values, histograms for distributions
2. **Logging**: Use async mode for high-throughput scenarios
3. **Labels**: Keep label count low (≤3) for better performance
4. **Thread Safety**: All operations are thread-safe, no locks needed

## Troubleshooting

**Problem**: Logs missing in async mode
- **Solution**: Call `log_destroy()` to flush pending logs

**Problem**: Registry full error
- **Solution**: Increase `MAX_METRICS` in `metrics.h` and recompile

**Problem**: Ring buffer overflow
- **Solution**: Increase `RING_BUFFER_SIZE` or reduce log rate

## Next Steps

- Read `README.md` for detailed documentation
- Run `make bench` to see performance characteristics
- Check `tests/` directory for more examples
- Run `./validate.sh` for full validation

## Support

For issues or questions, refer to the main DistriC 2.0 documentation.