# DistriC 2.0 - Implementation Strategy Document

## 1. Implementation Philosophy

### Core Principles
- **Bottom-Up Construction**: Build foundation layers first, test thoroughly, then add complexity
- **Incremental Integration**: Each layer must work independently before coupling
- **Test-First Mindset**: Write tests before implementing complex logic
- **Performance by Design**: Profile early, optimize critical paths from the start
- **Operational Excellence**: Build observability in from day one, not as an afterthought

---

## 2. Development Phases

### Phase 0: Observability Foundation (Week 1)
**Goal**: Establish metrics, logging, and tracing infrastructure FIRST

### Phase 1: Transport Layer (Weeks 2-3)
**Goal**: Build network primitives with observability built-in

### Phase 2: Protocol Layer (Weeks 3-4)
**Goal**: Custom serialization with monitoring

### Phase 3: Consensus Core (Weeks 5-7)
**Goal**: Implement Raft and Gossip protocols independently

### Phase 4: Cluster Management (Weeks 8-10)
**Goal**: Integrate hybrid coordination model

### Phase 5: Rule Engine (Weeks 11-13)
**Goal**: Build configuration-as-code and rule evaluation

### Phase 6: Task Execution (Weeks 14-16)
**Goal**: Implement workflow orchestration and task distribution

### Phase 7: API & Integration (Weeks 17-18)
**Goal**: External APIs and end-to-end testing

### Phase 8: Production Hardening (Weeks 19-20)
**Goal**: Performance tuning, security, chaos testing

---

## 3. Layer-by-Layer Implementation Strategy

---

## PHASE 0: Observability Foundation (Week 1)

### Objectives
Build the observability infrastructure BEFORE any other component. Every subsequent layer will use these primitives from day one.

### Why First?
- **Debug from the start**: See what's happening in transport/protocol layers immediately
- **Performance baseline**: Establish metrics early to track regressions
- **Operational mindset**: Forces thinking about production concerns upfront
- **No retrofitting**: Avoid the pain of adding logging/metrics after the fact

### Components

#### 0.1 Metrics System (`src/common/metrics.c`)

**Purpose**: Lock-free, high-performance metrics collection

```c
// Metric types
typedef enum {
    METRIC_COUNTER,      // Monotonically increasing (e.g., total requests)
    METRIC_GAUGE,        // Point-in-time value (e.g., active connections)
    METRIC_HISTOGRAM,    // Distribution of values (e.g., latency)
    METRIC_SUMMARY       // Similar to histogram but with quantiles
} metric_type_t;

// Lock-free metric storage (using atomic operations)
typedef struct {
    char name[128];
    char help[256];
    metric_type_t type;
    
    _Atomic uint64_t value;  // For counter/gauge
    
    // For histogram: use atomic array of buckets
    _Atomic uint64_t* buckets;
    double* bucket_bounds;
    size_t bucket_count;
    
    // Labels (up to 8 label pairs)
    char label_keys[8][64];
    char label_values[8][64];
    size_t label_count;
} metric_t;

typedef struct {
    metric_t* metrics;
    size_t metric_count;
    size_t capacity;
    
    pthread_rwlock_t registry_lock;  // Only for add/remove, not for updates
} metrics_registry_t;

// Initialize global metrics registry
metrics_registry_t* metrics_init(void);

// Register metrics (called at startup)
metric_t* metrics_register_counter(metrics_registry_t* reg, 
                                   const char* name, 
                                   const char* help,
                                   const char** label_keys,
                                   size_t label_count);

metric_t* metrics_register_gauge(metrics_registry_t* reg, 
                                 const char* name, 
                                 const char* help);

metric_t* metrics_register_histogram(metrics_registry_t* reg,
                                     const char* name,
                                     const char* help,
                                     const double* bucket_bounds,
                                     size_t bucket_count);

// Fast metric updates (lock-free using atomics)
static inline void metrics_counter_inc(metric_t* metric) {
    atomic_fetch_add(&metric->value, 1);
}

static inline void metrics_gauge_set(metric_t* metric, uint64_t value) {
    atomic_store(&metric->value, value);
}

void metrics_histogram_observe(metric_t* metric, double value);

// Get metric with specific labels
metric_t* metrics_get_with_labels(metrics_registry_t* reg,
                                  const char* name,
                                  const char** label_values,
                                  size_t label_count);

// Prometheus export (called by HTTP /metrics endpoint)
char* metrics_export_prometheus(metrics_registry_t* reg);
```

**Example Usage**:
```c
// At startup: register metrics
metrics_registry_t* metrics = metrics_init();

metric_t* tcp_connections = metrics_register_gauge(
    metrics, "tcp_connections_active", "Active TCP connections");

metric_t* tcp_bytes = metrics_register_counter(
    metrics, "tcp_bytes_sent_total", "Total bytes sent over TCP");

const double latency_buckets[] = {0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0};
metric_t* rpc_latency = metrics_register_histogram(
    metrics, "rpc_duration_seconds", "RPC call duration", 
    latency_buckets, 7);

// During operation: update metrics (fast, lock-free)
metrics_gauge_set(tcp_connections, current_conn_count);
metrics_counter_inc(tcp_bytes);
metrics_histogram_observe(rpc_latency, duration_sec);
```

#### 0.2 Structured Logging (`src/common/logging.c`)

**Purpose**: Fast, structured JSON logging with zero allocation in hot path

```c
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3,
    LOG_FATAL = 4
} log_level_t;

typedef struct {
    int fd;  // File descriptor (file or stdout)
    log_level_t min_level;
    _Atomic bool async_mode;
    
    // Ring buffer for async logging
    char* log_buffer;
    size_t buffer_size;
    _Atomic size_t write_pos;
    _Atomic size_t read_pos;
    
    pthread_t writer_thread;
} logger_t;

// Initialize logger
logger_t* log_init(const char* log_file, log_level_t min_level, bool async);

// Core logging function (varargs for key-value pairs)
void log_write(logger_t* logger, log_level_t level, const char* component,
               const char* message, ...);

// Convenience macros
#define LOG_DEBUG(logger, comp, msg, ...) \
    log_write(logger, LOG_DEBUG, comp, msg, ##__VA_ARGS__, NULL)

#define LOG_INFO(logger, comp, msg, ...) \
    log_write(logger, LOG_INFO, comp, msg, ##__VA_ARGS__, NULL)

#define LOG_ERROR(logger, comp, msg, ...) \
    log_write(logger, LOG_ERROR, comp, msg, ##__VA_ARGS__, NULL)
```

**Implementation Detail** (fast JSON formatting):
```c
void log_write(logger_t* logger, log_level_t level, const char* component,
               const char* message, ...) {
    if (level < logger->min_level) return;
    
    // Pre-allocated buffer (thread-local to avoid malloc)
    static __thread char buffer[4096];
    
    // Fast timestamp (cached, updated every 100ms)
    uint64_t now_ms = get_timestamp_ms();
    
    // Manual JSON formatting (faster than libraries)
    int len = snprintf(buffer, sizeof(buffer),
        "{\"ts\":%lu,\"level\":\"%s\",\"component\":\"%s\",\"msg\":\"%s\"",
        now_ms, log_level_str(level), component, message);
    
    // Append key-value pairs from varargs
    va_list args;
    va_start(args, message);
    const char* key;
    while ((key = va_arg(args, const char*)) != NULL) {
        const char* value = va_arg(args, const char*);
        len += snprintf(buffer + len, sizeof(buffer) - len,
                       ",\"%s\":\"%s\"", key, value);
    }
    va_end(args);
    
    len += snprintf(buffer + len, sizeof(buffer) - len, "}\n");
    
    // Write to buffer or directly
    if (logger->async_mode) {
        // Copy to ring buffer (lock-free)
        write_to_ring_buffer(logger, buffer, len);
    } else {
        write(logger->fd, buffer, len);
    }
}
```

**Example Usage**:
```c
LOG_INFO(logger, "tcp", "Connection accepted",
         "remote_addr", "10.0.1.5",
         "remote_port", "45231",
         "conn_id", "conn-123");

// Output:
// {"ts":1705147845123,"level":"INFO","component":"tcp","msg":"Connection accepted","remote_addr":"10.0.1.5","remote_port":"45231","conn_id":"conn-123"}
```

#### 0.3 Distributed Tracing (`src/common/tracing.c`)

**Purpose**: Minimal overhead tracing for request flows

```c
// Trace context (16 bytes for cache-line efficiency)
typedef struct {
    uint64_t trace_id_high;
    uint64_t trace_id_low;
} trace_id_t;

typedef struct {
    uint64_t span_id;
    uint64_t parent_span_id;
} span_id_t;

typedef struct {
    trace_id_t trace_id;
    span_id_t span_id;
    
    char operation[64];
    uint64_t start_us;
    uint64_t duration_us;
    
    // Lightweight tags (max 4)
    char tag_keys[4][32];
    char tag_values[4][64];
    size_t tag_count;
} trace_span_t;

// Thread-local active span
static __thread trace_span_t* current_span = NULL;

// Start new span
trace_span_t* trace_start_span(const char* operation);

// Start child span
trace_span_t* trace_start_child_span(trace_span_t* parent, const char* operation);

// Add tag
void trace_add_tag(trace_span_t* span, const char* key, const char* value);

// Finish span (automatically exports)
void trace_finish_span(trace_span_t* span);

// Extract/inject for RPC propagation
void trace_extract_from_headers(const char* trace_header, trace_id_t* trace_id, span_id_t* span_id);
void trace_inject_into_headers(trace_span_t* span, char* header_out, size_t max_len);
```

**Export Mechanism**:
```c
// Batch exporter (exports every 5 seconds or 1000 spans)
typedef struct {
    trace_span_t* span_buffer;
    size_t buffer_size;
    _Atomic size_t span_count;
    
    pthread_t export_thread;
    
    // Export destination (file, HTTP endpoint, etc.)
    const char* export_endpoint;
} trace_exporter_t;

void trace_exporter_init(const char* endpoint);
```

#### 0.4 Health Check System (`src/common/health.c`)

**Purpose**: Component health tracking

```c
typedef enum {
    HEALTH_UP,
    HEALTH_DOWN,
    HEALTH_DEGRADED
} health_status_t;

typedef struct {
    char component_name[64];
    health_status_t status;
    char status_message[256];
    uint64_t last_check_time;
} health_check_t;

typedef struct {
    health_check_t* checks;
    size_t check_count;
    pthread_rwlock_t lock;
} health_registry_t;

health_registry_t* health_init(void);

// Register component
void health_register(health_registry_t* reg, const char* component_name);

// Update component health
void health_update(health_registry_t* reg, const char* component_name,
                  health_status_t status, const char* message);

// Check overall health
health_status_t health_check_all(health_registry_t* reg);

// Export for /health endpoint
char* health_export_json(health_registry_t* reg);
```

#### 0.5 Simple HTTP Server for Metrics/Health (`src/common/http_server.c`)

**Purpose**: Minimal HTTP server for observability endpoints only

```c
typedef struct {
    uint16_t port;
    int listen_fd;
    pthread_t server_thread;
    
    metrics_registry_t* metrics;
    health_registry_t* health;
} observability_server_t;

observability_server_t* obs_server_create(uint16_t port,
                                          metrics_registry_t* metrics,
                                          health_registry_t* health);

int obs_server_start(observability_server_t* server);

// Handles:
// GET /metrics  -> Prometheus format
// GET /health/live  -> {"status": "UP"}
// GET /health/ready -> {"status": "UP", "checks": {...}}
```

### Testing Strategy
- **Performance test**: Measure overhead of metrics/logging (target: <1% CPU)
- **Concurrency test**: 100 threads logging simultaneously, no data corruption
- **Export test**: Verify Prometheus format correctness
- **Async logging**: Verify no log loss under high load

### Deliverables
- ✅ Metrics system with <1% overhead
- ✅ Structured logger (sync and async modes)
- ✅ Trace context propagation working
- ✅ Health check registry functional
- ✅ HTTP server exposing /metrics and /health
- ✅ Zero external dependencies (except libc)

---

## LAYER 1: Transport Layer (Week 2-3)

### Objectives
Build reliable, high-performance network communication primitives with built-in observability.

### Components

#### 1.1 TCP Transport (`src/transport/tcp.c`)

**Purpose**: Reliable, connection-oriented communication for Raft and task distribution

**Key Features with Observability**:
```c
// Core API
typedef struct tcp_server tcp_server_t;
typedef struct tcp_connection tcp_connection_t;

// Server with integrated metrics
tcp_server_t* tcp_server_create(const char* bind_addr, uint16_t port,
                                metrics_registry_t* metrics,
                                logger_t* logger);

int tcp_server_start(tcp_server_t* server, 
                     void (*on_connection)(tcp_connection_t* conn, void* userdata),
                     void* userdata);

// Connection management with automatic metrics
int tcp_send(tcp_connection_t* conn, const void* data, size_t len);
int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms);
void tcp_close(tcp_connection_t* conn);

// Metrics automatically tracked:
// - tcp_connections_active (gauge)
// - tcp_connections_total (counter)
// - tcp_bytes_sent_total (counter)
// - tcp_bytes_received_total (counter)
// - tcp_errors_total (counter, labeled by error_type)
```

**Implementation with Observability**:
```c
typedef struct tcp_connection {
    int fd;
    char remote_addr[256];
    uint16_t remote_port;
    uint64_t conn_id;
    uint64_t created_at;
    
    // Metrics
    metric_t* bytes_sent_metric;
    metric_t* bytes_recv_metric;
    
    // Tracing
    trace_span_t* current_span;
} tcp_connection_t;

int tcp_send(tcp_connection_t* conn, const void* data, size_t len) {
    // Start trace span
    trace_span_t* span = trace_start_span("tcp_send");
    trace_add_tag(span, "conn_id", conn->conn_id);
    
    ssize_t sent = send(conn->fd, data, len, 0);
    
    if (sent > 0) {
        // Update metrics
        atomic_fetch_add(&conn->bytes_sent_metric->value, sent);
        
        // Log success
        LOG_DEBUG(conn->logger, "tcp", "Data sent",
                 "conn_id", conn->conn_id,
                 "bytes", sent);
    } else {
        // Log error
        LOG_ERROR(conn->logger, "tcp", "Send failed",
                 "conn_id", conn->conn_id,
                 "errno", errno,
                 "error", strerror(errno));
    }
    
    trace_finish_span(span);
    return sent;
}
```

**Connection pooling with metrics**:
```c
typedef struct tcp_pool tcp_pool_t;

tcp_pool_t* tcp_pool_create(size_t max_connections,
                            metrics_registry_t* metrics,
                            logger_t* logger);

tcp_connection_t* tcp_pool_acquire(tcp_pool_t* pool, 
                                   const char* host, uint16_t port);

void tcp_pool_release(tcp_pool_t* pool, tcp_connection_t* conn);

// Additional metrics:
// - tcp_pool_size (gauge)
// - tcp_pool_acquire_duration_seconds (histogram)
// - tcp_pool_connection_reuse_total (counter)
```

#### 1.2 UDP Transport (`src/transport/udp.c`)

**Purpose**: Fast, connectionless communication for Gossip protocol

```c
typedef struct udp_socket udp_socket_t;

udp_socket_t* udp_socket_create(const char* bind_addr, uint16_t port,
                                metrics_registry_t* metrics,
                                logger_t* logger);

int udp_send(udp_socket_t* sock, const void* data, size_t len,
             const char* dest_addr, uint16_t dest_port);

int udp_recv(udp_socket_t* sock, void* buffer, size_t len,
             char* src_addr, uint16_t* src_port, int timeout_ms);

// Metrics automatically tracked:
// - udp_packets_sent_total (counter)
// - udp_packets_received_total (counter)
// - udp_bytes_sent_total (counter)
// - udp_bytes_received_total (counter)
// - udp_send_errors_total (counter)
```

### Testing Strategy
- ✅ Unit tests: Connection lifecycle with metrics validation
- ✅ Stress tests: 10,000+ concurrent connections, verify metrics accuracy
- ✅ Observability test: Confirm all metrics appear in /metrics endpoint
- ✅ Log verification: Parse JSON logs, ensure structure correctness

### Deliverables
- ✅ TCP server with integrated metrics/logging
- ✅ UDP socket with integrated metrics/logging
- ✅ All transport operations traced
- ✅ Zero external dependencies for serialization

---

## LAYER 2: Protocol Layer (Week 3-4)

### Objectives
Efficient, custom binary serialization with built-in monitoring.

### Design Philosophy: Custom Binary Protocol

**Why Custom Instead of Protobuf?**
1. **Zero dependencies**: Complete control, no external library issues
2. **Predictable performance**: No hidden allocations or complexity
3. **Optimized for our use case**: Tailor wire format to our needs
4. **Easier debugging**: Simple hex dump reveals structure
5. **Versioning control**: Explicit version handling in our code

### Components

#### 2.1 Binary Protocol Foundation (`src/protocol/binary.c`)

**Core Design Principles**:
- **Fixed header**: Every message starts with known structure
- **Type-Length-Value (TLV)**: Flexible payload encoding
- **Network byte order**: Big-endian for portability
- **CRC32 checksums**: Detect corruption
- **Version field**: Protocol evolution support

**Message Structure**:
```
┌─────────────────────────────────────────┐
│         HEADER (32 bytes)               │
├─────────────────────────────────────────┤
│  Magic (4 bytes): 0x44495354 ("DIST")  │
│  Version (2 bytes): 0x0001              │
│  Message Type (2 bytes)                 │
│  Flags (2 bytes)                        │
│  Payload Length (4 bytes)               │
│  Message ID (8 bytes)                   │
│  Timestamp (8 bytes)                    │
│  CRC32 (4 bytes) - of header+payload    │
├─────────────────────────────────────────┤
│         PAYLOAD (variable)              │
│  TLV fields or structured data          │
└─────────────────────────────────────────┘
```

**Header Definition**:
```c
#define PROTOCOL_MAGIC 0x44495354  // "DIST"
#define PROTOCOL_VERSION 0x0001

typedef struct __attribute__((packed)) {
    uint32_t magic;           // Must be PROTOCOL_MAGIC
    uint16_t version;         // Protocol version
    uint16_t msg_type;        // Message type enum
    uint16_t flags;           // Message flags
    uint32_t payload_len;     // Length of payload
    uint64_t message_id;      // Unique message ID
    uint64_t timestamp;       // Unix timestamp (microseconds)
    uint32_t crc32;           // CRC32 of everything (this field = 0 when computing)
} message_header_t;

// Message types
typedef enum {
    // Raft messages
    MSG_RAFT_REQUEST_VOTE = 0x1001,
    MSG_RAFT_REQUEST_VOTE_RESPONSE = 0x1002,
    MSG_RAFT_APPEND_ENTRIES = 0x1003,
    MSG_RAFT_APPEND_ENTRIES_RESPONSE = 0x1004,
    MSG_RAFT_INSTALL_SNAPSHOT = 0x1005,
    
    // Gossip messages
    MSG_GOSSIP_PING = 0x2001,
    MSG_GOSSIP_ACK = 0x2002,
    MSG_GOSSIP_INDIRECT_PING = 0x2003,
    MSG_GOSSIP_MEMBERSHIP_UPDATE = 0x2004,
    
    // Task messages
    MSG_TASK_ASSIGNMENT = 0x3001,
    MSG_TASK_RESULT = 0x3002,
    MSG_TASK_STATUS = 0x3003,
    
    // Client messages
    MSG_CLIENT_SUBMIT = 0x4001,
    MSG_CLIENT_RESPONSE = 0x4002,
    MSG_CLIENT_QUERY = 0x4003
} message_type_t;

// Flags
#define MSG_FLAG_COMPRESSED   0x0001
#define MSG_FLAG_ENCRYPTED    0x0002
#define MSG_FLAG_URGENT       0x0004
```

#### 2.2 TLV Payload Encoding (`src/protocol/tlv.c`)

**Purpose**: Flexible field encoding for complex messages

```c
// TLV field types
typedef enum {
    TLV_UINT8 = 0x01,
    TLV_UINT16 = 0x02,
    TLV_UINT32 = 0x03,
    TLV_UINT64 = 0x04,
    TLV_STRING = 0x10,    // Length-prefixed UTF-8 string
    TLV_BYTES = 0x11,     // Length-prefixed byte array
    TLV_ARRAY = 0x20,     // Array of TLV fields
    TLV_MAP = 0x21        // Key-value pairs
} tlv_type_t;

typedef struct {
    uint8_t type;         // TLV type
    uint16_t tag;         // Field identifier (e.g., FIELD_NODE_ID)
    uint32_t length;      // Length of value
    uint8_t* value;       // Pointer to value data
} tlv_field_t;

// TLV encoder/decoder
typedef struct {
    uint8_t* buffer;
    size_t capacity;
    size_t offset;
} tlv_encoder_t;

tlv_encoder_t* tlv_encoder_create(size_t initial_capacity);

// Encode functions
void tlv_encode_uint32(tlv_encoder_t* enc, uint16_t tag, uint32_t value);
void tlv_encode_uint64(tlv_encoder_t* enc, uint16_t tag, uint64_t value);
void tlv_encode_string(tlv_encoder_t* enc, uint16_t tag, const char* str);
void tlv_encode_bytes(tlv_encoder_t* enc, uint16_t tag, const uint8_t* data, size_t len);

// Get final buffer
uint8_t* tlv_encoder_finalize(tlv_encoder_t* enc, size_t* len_out);

// Decoder
typedef struct {
    const uint8_t* buffer;
    size_t length;
    size_t offset;
} tlv_decoder_t;

tlv_decoder_t* tlv_decoder_create(const uint8_t* buffer, size_t length);

// Decode functions
int tlv_decode_next(tlv_decoder_t* dec, tlv_field_t* field_out);
int tlv_find_field(tlv_decoder_t* dec, uint16_t tag, tlv_field_t* field_out);

// Helper to extract typed values
uint32_t tlv_field_as_uint32(const tlv_field_t* field);
uint64_t tlv_field_as_uint64(const tlv_field_t* field);
const char* tlv_field_as_string(const tlv_field_t* field);
```

#### 2.3 Message-Specific Structures (`src/protocol/messages.c`)

**Example: Raft RequestVote**:

```c
// Field tags for Raft RequestVote
#define FIELD_TERM            0x0001
#define FIELD_CANDIDATE_ID    0x0002
#define FIELD_LAST_LOG_INDEX  0x0003
#define FIELD_LAST_LOG_TERM   0x0004

typedef struct {
    uint32_t term;
    char candidate_id[64];
    uint32_t last_log_index;
    uint32_t last_log_term;
} raft_request_vote_t;

// Serialize
int serialize_raft_request_vote(const raft_request_vote_t* rv,
                                uint8_t** buffer_out,
                                size_t* len_out) {
    tlv_encoder_t* enc = tlv_encoder_create(256);
    
    tlv_encode_uint32(enc, FIELD_TERM, rv->term);
    tlv_encode_string(enc, FIELD_CANDIDATE_ID, rv->candidate_id);
    tlv_encode_uint32(enc, FIELD_LAST_LOG_INDEX, rv->last_log_index);
    tlv_encode_uint32(enc, FIELD_LAST_LOG_TERM, rv->last_log_term);
    
    *buffer_out = tlv_encoder_finalize(enc, len_out);
    return 0;
}

// Deserialize
int deserialize_raft_request_vote(const uint8_t* buffer,
                                  size_t len,
                                  raft_request_vote_t* rv_out) {
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    tlv_field_t field;
    
    while (tlv_decode_next(dec, &field) == 0) {
        switch (field.tag) {
            case FIELD_TERM:
                rv_out->term = tlv_field_as_uint32(&field);
                break;
            case FIELD_CANDIDATE_ID:
                strncpy(rv_out->candidate_id, tlv_field_as_string(&field), 63);
                break;
            case FIELD_LAST_LOG_INDEX:
                rv_out->last_log_index = tlv_field_as_uint32(&field);
                break;
            case FIELD_LAST_LOG_TERM:
                rv_out->last_log_term = tlv_field_as_uint32(&field);
                break;
        }
    }
    
    tlv_decoder_free(dec);
    return 0;
}
```

**Example: Task Assignment**:

```c
#define FIELD_TASK_ID         0x0101
#define FIELD_WORKFLOW_ID     0x0102
#define FIELD_TASK_TYPE       0x0103
#define FIELD_TIMEOUT         0x0104
#define FIELD_INPUT_DATA      0x0105
#define FIELD_CONFIG_JSON     0x0106

typedef struct {
    char task_id[128];
    char workflow_id[128];
    char task_type[64];
    uint32_t timeout_sec;
    uint8_t* input_data;
    size_t input_data_len;
    char* config_json;
} task_assignment_t;

int serialize_task_assignment(const task_assignment_t* ta,
                              uint8_t** buffer_out,
                              size_t* len_out) {
    tlv_encoder_t* enc = tlv_encoder_create(1024);
    
    tlv_encode_string(enc, FIELD_TASK_ID, ta->task_id);
    tlv_encode_string(enc, FIELD_WORKFLOW_ID, ta->workflow_id);
    tlv_encode_string(enc, FIELD_TASK_TYPE, ta->task_type);
    tlv_encode_uint32(enc, FIELD_TIMEOUT, ta->timeout_sec);
    tlv_encode_bytes(enc, FIELD_INPUT_DATA, ta->input_data, ta->input_data_len);
    tlv_encode_string(enc, FIELD_CONFIG_JSON, ta->config_json);
    
    *buffer_out = tlv_encoder_finalize(enc, len_out);
    return 0;
}
```

#### 2.4 RPC Framework (`src/protocol/rpc.c`)

**Purpose**: Request-response pattern over TCP with observability

```c
typedef struct rpc_client rpc_client_t;
typedef struct rpc_server rpc_server_t;

// RPC handler signature
typedef int (*rpc_handler_t)(const uint8_t* request, size_t req_len,
                             uint8_t** response, size_t* resp_len,
                             void* userdata,
                             trace_span_t* span);  // For distributed tracing

// Server with observability
rpc_server_t* rpc_server_create(tcp_server_t* tcp_server,
                                metrics_registry_t* metrics,
                                logger_t* logger);

int rpc_register_handler(rpc_server_t* server, 
                        message_type_t msg_type,
                        rpc_handler_t handler, 
                        void* userdata);

// Client with observability
rpc_client_t* rpc_client_create(tcp_pool_t* pool,
                               metrics_registry_t* metrics,
                               logger_t* logger);

int rpc_call(rpc_client_t* client, 
             const char* host, uint16_t port,
             message_type_t msg_type,
             const uint8_t* request, size_t req_len,
             uint8_t** response, size_t* resp_len,
             int timeout_ms);

// Metrics tracked:
// - rpc_calls_total (counter, labeled by msg_type)
// - rpc_call_duration_seconds (histogram, labeled by msg_type)
// - rpc_errors_total (counter, labeled by msg_type, error_type)
```

**RPC Implementation with Full Observability**:
```c
int rpc_call(rpc_client_t* client, const char* host, uint16_t port,
             message_type_t msg_type, const uint8_t* request, size_t req_len,
             uint8_t** response, size_t* resp_len, int timeout_ms) {
    
    // Start trace span
    trace_span_t* span = trace_start_span("rpc_call");
    trace_add_tag(span, "msg_type", message_type_to_string(msg_type));
    trace_add_tag(span, "host", host);
    
    uint64_t start_time = get_timestamp_us();
    
    // Build message
    message_header_t header = {
        .magic = PROTOCOL_MAGIC,
        .version = PROTOCOL_VERSION,
        .msg_type = msg_type,
        .payload_len = req_len,
        .message_id = generate_message_id(),
        .timestamp = start_time
    };
    
    // Inject trace context into flags/message_id
    inject_trace_context(&header, span);
    
    // Compute CRC
    header.crc32 = compute_crc32(&header, request, req_len);
    
    // Send
    tcp_connection_t* conn = tcp_pool_acquire(client->pool, host, port);
    if (!conn) {
        LOG_ERROR(client->logger, "rpc", "Failed to acquire connection");
        trace_add_tag(span, "error", "no_connection");
        trace_finish_span(span);
        return -1;
    }
    
    int sent = tcp_send(conn, &header, sizeof(header));
    sent += tcp_send(conn, request, req_len);
    
    // Receive response
    message_header_t resp_header;
    int received = tcp_recv(conn, &resp_header, sizeof(resp_header), timeout_ms);
    
    if (received != sizeof(resp_header)) {
        LOG_ERROR(client->logger, "rpc", "Failed to receive response header");
        tcp_pool_release(client->pool, conn);
        trace_add_tag(span, "error", "recv_failed");
        trace_finish_span(span);
        return -1;
    }
    
    // Validate response
    if (resp_header.magic != PROTOCOL_MAGIC) {
        LOG_ERROR(client->logger, "rpc", "Invalid response magic");
        tcp_pool_release(client->pool, conn);
        return -1;
    }
    
    // Read payload
    *response = malloc(resp_header.payload_len);
    received = tcp_recv(conn, *response, resp_header.payload_len, timeout_ms);
    *resp_len = received;
    
    tcp_pool_release(client->pool, conn);
    
    // Update metrics
    uint64_t duration_us = get_timestamp_us() - start_time;
    metrics_histogram_observe(client->rpc_duration_metric, duration_us / 1000000.0);
    metrics_counter_inc(client->rpc_calls_metric);
    
    // Log
    LOG_INFO(client->logger, "rpc", "Call completed",
             "msg_type", message_type_to_string(msg_type),
             "duration_ms", duration_us / 1000);
    
    trace_finish_span(span);
    return 0;
}
```

### Testing Strategy
- ✅ Unit tests: Serialize/deserialize all message types
- ✅ Compatibility tests: Old format readers can skip unknown TLV fields
- ✅ Corruption tests: CRC32 detects modified payloads
- ✅ Performance tests: Serialization <1μs per message
- ✅ Metrics validation: All RPC calls appear in metrics

### Deliverables
- ✅ Custom binary protocol with TLV encoding
- ✅ Message definitions for all protocol types
- ✅ RPC framework with full observability
- ✅ Zero external serialization libraries
- ✅ Protocol versioning strategy documented

---

## LAYER 3: Raft Consensus (Week 5-6)
```c
// Core API
typedef struct tcp_server tcp_server_t;
typedef struct tcp_connection tcp_connection_t;

// Non-blocking server with epoll/kqueue
tcp_server_t* tcp_server_create(const char* bind_addr, uint16_t port);
int tcp_server_start(tcp_server_t* server, 
                     void (*on_connection)(tcp_connection_t* conn, void* userdata),
                     void* userdata);

// Connection management
int tcp_send(tcp_connection_t* conn, const void* data, size_t len);
int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms);
void tcp_close(tcp_connection_t* conn);

// Connection pooling
typedef struct tcp_pool tcp_pool_t;
tcp_pool_t* tcp_pool_create(size_t max_connections);
tcp_connection_t* tcp_pool_acquire(tcp_pool_t* pool, const char* host, uint16_t port);
void tcp_pool_release(tcp_pool_t* pool, tcp_connection_t* conn);
```

**Implementation Details**:
- Use `epoll` (Linux) or `kqueue` (BSD/macOS) for event-driven I/O
- Implement connection pooling to reuse TCP connections
- Support TLS via OpenSSL/mbedTLS (optional but architected)
- Buffer management: Ring buffers for send/recv queues
- Graceful shutdown with proper socket cleanup

**Testing Strategy**:
- Unit tests: Connection lifecycle, send/recv, error handling
- Stress tests: 10,000+ concurrent connections
- Failure injection: Network drops, timeouts, partial writes

#### 1.2 UDP Transport (`src/transport/udp.c`)

**Purpose**: Fast, connectionless communication for Gossip protocol

**Key Features**:
```c
typedef struct udp_socket udp_socket_t;

// Simple datagram API
udp_socket_t* udp_socket_create(const char* bind_addr, uint16_t port);
int udp_send(udp_socket_t* sock, const void* data, size_t len,
             const char* dest_addr, uint16_t dest_port);
int udp_recv(udp_socket_t* sock, void* buffer, size_t len,
             char* src_addr, uint16_t* src_port, int timeout_ms);
void udp_close(udp_socket_t* sock);

// Multicast support for gossip
int udp_join_multicast(udp_socket_t* sock, const char* group_addr);
```

**Implementation Details**:
- Non-blocking I/O with `select()` or event loop integration
- Packet size limited to safe MTU (1400 bytes)
- Optional checksum verification
- Rate limiting to prevent UDP flood

**Testing Strategy**:
- Packet loss simulation (drop random packets)
- Reordering tests
- Bandwidth saturation tests

#### 1.3 Network Utilities (`src/transport/net_utils.c`)

**Utilities**:
- DNS resolution (async preferred)
- Address parsing and validation
- Network interface enumeration
- Latency measurement utilities

### Deliverables
- ✅ Rule parser handling complex YAML configurations
- ✅ Evaluator achieving <5ms latency per rule
- ✅ Rule catalog with hot-reload capability
- ✅ Integration with Raft for consistent replication

---

## LAYER 7: Task Execution Engine (Week 11-13)

### Objectives
Workflow orchestration and distributed task execution.

### Components

#### 7.1 Workflow Definition (`src/task_engine/workflow.c`)

**Purpose**: Define workflows as DAGs of tasks

```c
typedef struct {
    char task_id[128];
    char task_type[64];  // plugin name
    char* config_json;
    
    // Dependencies
    char** depends_on;  // array of task_ids
    size_t dependency_count;
    
    uint32_t timeout_sec;
    uint32_t retry_count;
} workflow_task_def_t;

typedef struct {
    char workflow_id[128];
    uint32_t version;
    
    workflow_task_def_t* tasks;
    size_t task_count;
    
    char* initial_task_id;  // entry point
} workflow_definition_t;

// Parse workflow from YAML
int parse_workflow(const char* yaml_str, workflow_definition_t** workflow);

// Topological sort to determine execution order
int workflow_get_execution_order(const workflow_definition_t* wf, 
                                 char*** task_order, size_t* count);
```

**Example Workflow YAML**:
```yaml
workflow:
  id: "fraud_detection"
  version: 1
  tasks:
    - id: "validate_payment"
      type: "payment_validator"
      config:
        threshold: 10000
      timeout: 10
      retry: 2
      
    - id: "check_user_history"
      type: "user_history_checker"
      depends_on: ["validate_payment"]
      timeout: 15
      retry: 3
      
    - id: "ml_fraud_score"
      type: "ml_inference"
      depends_on: ["validate_payment", "check_user_history"]
      config:
        model: "fraud_v2"
      timeout: 20
      retry: 1
      
    - id: "send_notification"
      type: "webhook"
      depends_on: ["ml_fraud_score"]
      config:
        url: "https://api.example.com/fraud-alert"
      timeout: 5
      retry: 3
```

#### 7.2 Task Plugin System (`src/task_engine/plugin_loader.c`)

**Purpose**: Dynamic loading of task implementations

```c
typedef struct {
    const char* (*get_name)(void);
    const char* (*get_version)(void);
    
    // Initialize plugin (called once at load)
    int (*init)(const char* config_json);
    
    // Execute task
    int (*execute)(const char* task_id,
                   const char* input_json,
                   char** output_json,
                   void* context);
    
    // Cleanup
    void (*cleanup)(void);
    
    // Validate configuration
    int (*validate_config)(const char* config_json);
} task_plugin_interface_t;

typedef struct {
    char name[64];
    void* dl_handle;  // dlopen handle
    task_plugin_interface_t* interface;
} task_plugin_t;

// Plugin registry
typedef struct {
    task_plugin_t* plugins;
    size_t plugin_count;
    pthread_rwlock_t lock;
} plugin_registry_t;

plugin_registry_t* plugin_registry_create(void);

// Load plugin from shared library
int plugin_registry_load(plugin_registry_t* reg, const char* so_path);

// Get plugin by name
task_plugin_t* plugin_registry_get(plugin_registry_t* reg, const char* name);
```

**Example Plugin Implementation**:
```c
// plugins/payment_validator/payment_validator.c

static int payment_validator_execute(const char* task_id,
                                     const char* input_json,
                                     char** output_json,
                                     void* context) {
    // Parse input
    cJSON* input = cJSON_Parse(input_json);
    double amount = cJSON_GetObjectItem(input, "amount")->valuedouble;
    
    // Validation logic
    bool is_valid = (amount > 0 && amount < 1000000);
    
    // Build output
    cJSON* output = cJSON_CreateObject();
    cJSON_AddBoolToObject(output, "valid", is_valid);
    *output_json = cJSON_PrintUnformatted(output);
    
    cJSON_Delete(input);
    cJSON_Delete(output);
    
    return is_valid ? 0 : -1;
}

// Export plugin interface
__attribute__((visibility("default")))
task_plugin_interface_t* get_plugin_interface(void) {
    static task_plugin_interface_t interface = {
        .get_name = ...,
        .execute = payment_validator_execute,
        ...
    };
    return &interface;
}
```

#### 7.3 Workflow Executor (`src/task_engine/executor.c`)

**Purpose**: Orchestrate workflow execution across workers

```c
typedef enum {
    TASK_PENDING,
    TASK_RUNNING,
    TASK_COMPLETED,
    TASK_FAILED,
    TASK_TIMEOUT,
    TASK_CANCELLED
} task_status_t;

typedef struct {
    char task_instance_id[128];
    char workflow_instance_id[128];
    char task_def_id[128];
    
    task_status_t status;
    char* assigned_worker_id;
    
    char* input_data;
    char* output_data;
    char* error_message;
    
    uint32_t retry_count;
    uint64_t started_at;
    uint64_t completed_at;
} task_instance_t;

typedef struct {
    char workflow_instance_id[128];
    char workflow_def_id[128];
    
    task_instance_t* tasks;
    size_t task_count;
    
    char* input_message;  // original message that triggered workflow
    uint64_t created_at;
} workflow_instance_t;

typedef struct {
    workflow_instance_t* instances;
    size_t instance_count;
    pthread_rwlock_t lock;
    
    worker_pool_t* worker_pool;
    plugin_registry_t* plugins;
} workflow_executor_t;

workflow_executor_t* workflow_executor_create(worker_pool_t* pool, 
                                              plugin_registry_t* plugins);

// Start workflow execution
int workflow_executor_start(workflow_executor_t* exec,
                            const workflow_definition_t* def,
                            const char* input_message,
                            char* workflow_instance_id_out);

// Internal: Schedule next ready tasks
int schedule_ready_tasks(workflow_executor_t* exec, 
                        workflow_instance_t* instance);

// Task completion callback (from worker)
int workflow_executor_on_task_completed(workflow_executor_t* exec,
                                       const char* task_instance_id,
                                       const char* output_data);

int workflow_executor_on_task_failed(workflow_executor_t* exec,
                                    const char* task_instance_id,
                                    const char* error_msg);
```

**Execution Flow**:
1. Create workflow instance with all tasks in PENDING state
2. Determine tasks with no dependencies → mark READY
3. For each ready task:
   - Select worker from pool
   - Send TaskAssignment RPC to worker
   - Mark task as RUNNING
4. On task completion:
   - Store output
   - Check dependent tasks → mark READY if all dependencies satisfied
   - Repeat step 3
5. Workflow completes when all tasks COMPLETED or when critical task FAILED

#### 7.4 Worker Task Runner (`src/task_engine/worker_runner.c`)

**Purpose**: Execute tasks on worker nodes

```c
typedef struct {
    char worker_id[128];
    plugin_registry_t* plugins;
    
    // Concurrent task execution
    pthread_t* worker_threads;
    size_t thread_count;
    
    task_queue_t* task_queue;
} worker_runner_t;

worker_runner_t* worker_runner_create(const char* worker_id, size_t thread_count);

// Start listening for task assignments
int worker_runner_start(worker_runner_t* runner, const char* coordinator_addr);

// Worker thread: pulls tasks from queue and executes
void* worker_thread_func(void* arg) {
    worker_runner_t* runner = (worker_runner_t*)arg;
    
    while (runner->running) {
        task_assignment_t* assignment = task_queue_pop(runner->task_queue, 1000);
        if (!assignment) continue;
        
        // Get plugin
        task_plugin_t* plugin = plugin_registry_get(runner->plugins, 
                                                     assignment->task_type);
        
        // Execute
        char* output = NULL;
        int result = plugin->interface->execute(assignment->task_id,
                                               assignment->input_data,
                                               &output,
                                               NULL);
        
        // Report back to coordinator
        if (result == 0) {
            send_task_completed(runner, assignment->task_id, output);
        } else {
            send_task_failed(runner, assignment->task_id, "Execution failed");
        }
        
        free(output);
        free_task_assignment(assignment);
    }
}
```

#### 7.5 Retry & Timeout Handling

**Timeout Watchdog**:
```c
void* timeout_watchdog_thread(void* arg) {
    workflow_executor_t* exec = (workflow_executor_t*)arg;
    
    while (exec->running) {
        pthread_rwlock_rdlock(&exec->lock);
        
        uint64_t now = get_timestamp_ms();
        for (size_t i = 0; i < exec->instance_count; i++) {
            workflow_instance_t* inst = &exec->instances[i];
            
            for (size_t j = 0; j < inst->task_count; j++) {
                task_instance_t* task = &inst->tasks[j];
                
                if (task->status == TASK_RUNNING) {
                    uint64_t elapsed = now - task->started_at;
                    if (elapsed > task->timeout_ms) {
                        // Mark timed out, potentially retry
                        handle_task_timeout(exec, task);
                    }
                }
            }
        }
        
        pthread_rwlock_unlock(&exec->lock);
        sleep(1);
    }
}
```

**Retry Logic**:
```c
int handle_task_failure(workflow_executor_t* exec, task_instance_t* task) {
    if (task->retry_count < task->max_retries) {
        task->retry_count++;
        task->status = TASK_PENDING;
        
        // Exponential backoff delay
        uint64_t delay_ms = 1000 * (1 << task->retry_count);
        schedule_task_retry(exec, task, delay_ms);
        
        return 0;
    } else {
        task->status = TASK_FAILED;
        // Fail entire workflow or trigger compensation
        return -1;
    }
}
```

### Testing Strategy
- **Unit tests**: DAG topological sort, dependency resolution
- **Integration tests**: End-to-end workflow with mock plugins
- **Chaos tests**: Kill workers mid-task, verify retry
- **Performance tests**: 1000 concurrent workflows

### Deliverables
- ✅ Workflow parser and DAG executor
- ✅ Plugin system with 3+ example plugins
- ✅ Worker runner handling concurrent tasks
- ✅ Timeout and retry mechanisms working

---

## LAYER 8: Message Ingestion API (Week 13-14)

### Objectives
External API for message submission.

### Components

#### 8.1 REST API Server (`src/api/rest_server.c`)

**Dependencies**: `libmicrohttpd` or custom HTTP server

```c
typedef struct {
    uint16_t port;
    tcp_server_t* tcp_server;
    
    cluster_coordinator_t* coordinator;
    rule_catalog_t* rule_catalog;
} rest_api_server_t;

rest_api_server_t* rest_api_create(uint16_t port, 
                                   cluster_coordinator_t* coordinator);
int rest_api_start(rest_api_server_t* server);
```

**Endpoints**:

**POST /api/v1/messages** - Submit message
```json
Request:
{
  "event_type": "payment_received",
  "amount": 15000,
  "user_id": "user_12345",
  "metadata": {}
}

Response (202 Accepted):
{
  "message_id": "msg-abc123",
  "status": "accepted",
  "workflows_triggered": ["fraud_detection", "notification"]
}
```

**GET /api/v1/messages/{id}** - Query message status

**GET /api/v1/workflows/{id}** - Query workflow status

**Admin Endpoints** (authentication required):

**GET /api/v1/rules** - List rules
**POST /api/v1/rules** - Create rule
**PUT /api/v1/rules/{id}** - Update rule
**DELETE /api/v1/rules/{id}** - Delete rule

**GET /api/v1/cluster/status** - Cluster health
```json
{
  "leader": "coordinator-1",
  "coordinators": [
    {"id": "coordinator-1", "state": "leader", "address": "10.0.1.1:9000"},
    {"id": "coordinator-2", "state": "follower", "address": "10.0.1.2:9000"}
  ],
  "workers": [
    {"id": "worker-1", "address": "10.0.2.1:9001", "load": 0.45, "status": "alive"},
    {"id": "worker-2", "address": "10.0.2.2:9001", "load": 0.67, "status": "alive"}
  ]
}
```

#### 8.2 Request Processing Pipeline

```c
void handle_message_post(rest_api_server_t* server, http_request_t* req, http_response_t* resp) {
    // 1. Validate request
    if (!validate_json_schema(req->body)) {
        http_response_set_status(resp, 400);
        return;
    }
    
    // 2. Generate message ID
    char message_id[128];
    generate_uuid(message_id);
    
    // 3. Evaluate rules
    rule_t* matched_rules[64];
    size_t matched_count;
    find_matching_rules(server->rule_catalog->rules, 
                       server->rule_catalog->rule_count,
                       req->body,
                       matched_rules, &matched_count);
    
    // 4. Start workflows
    for (size_t i = 0; i < matched_count; i++) {
        for (size_t j = 0; j < matched_rules[i]->action_count; j++) {
            rule_action_t* action = &matched_rules[i]->actions[j];
            
            workflow_definition_t* wf = get_workflow_def(action->workflow_id);
            workflow_executor_start(server->coordinator->executor, wf, req->body, NULL);
        }
    }
    
    // 5. Return response
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "message_id", message_id);
    cJSON_AddStringToObject(response, "status", "accepted");
    
    http_response_set_status(resp, 202);
    http_response_set_body(resp, cJSON_PrintUnformatted(response));
    cJSON_Delete(response);
}
```

### Deliverables
- ✅ REST API with core endpoints
- ✅ Message submission triggering workflows
- ✅ Admin API for rule/workflow management
- ✅ API rate limiting implemented

---

## LAYER 9: Observability & Operations (Week 14-15)

### Objectives
Production-ready monitoring, logging, and operations tools.

### Components

#### 9.1 Metrics Collection (`src/common/metrics.c`)

**Purpose**: Prometheus-compatible metrics

```c
typedef enum {
    METRIC_COUNTER,
    METRIC_GAUGE,
    METRIC_HISTOGRAM
} metric_type_t;

typedef struct {
    char name[128];
    metric_type_t type;
    double value;
    
    // Labels
    char** label_names;
    char** label_values;
    size_t label_count;
} metric_t;

typedef struct {
    metric_t* metrics;
    size_t metric_count;
    pthread_rwlock_t lock;
} metrics_registry_t;

metrics_registry_t* metrics_registry_create(void);

// Update metrics
void metrics_counter_inc(metrics_registry_t* reg, const char* name, 
                        const char** labels, size_t label_count);
void metrics_gauge_set(metrics_registry_t* reg, const char* name, double value);
void metrics_histogram_observe(metrics_registry_t* reg, const char* name, double value);

// Expose metrics in Prometheus format
char* metrics_export_prometheus(metrics_registry_t* reg);
```

**Key Metrics**:
- `distric_messages_received_total` (counter)
- `distric_rule_evaluations_duration_seconds` (histogram)
- `distric_tasks_running` (gauge)
- `distric_workflow_duration_seconds` (histogram)
- `distric_raft_leader_changes_total` (counter)
- `distric_gossip_failure_detections_total` (counter)

**Metrics HTTP Endpoint**:
```
GET /metrics

# HELP distric_messages_received_total Total messages received
# TYPE distric_messages_received_total counter
distric_messages_received_total{coordinator="coord-1"} 125678

# HELP distric_tasks_running Currently running tasks
# TYPE distric_tasks_running gauge
distric_tasks_running{worker="worker-1"} 12
```

#### 9.2 Structured Logging (`src/common/logging.c`)

**Purpose**: JSON structured logs for centralized aggregation

```c
typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level_t;

void log_init(const char* log_file, log_level_t min_level);

void log_json(log_level_t level, const char* component, const char* message, 
              const char* key1, const char* val1, ...);

// Example usage
log_json(LOG_INFO, "raft", "Leader elected", 
         "term", "42", 
         "leader_id", "coordinator-1",
         NULL);

// Output:
// {"timestamp":"2026-01-13T10:30:45Z","level":"INFO","component":"raft","message":"Leader elected","term":"42","leader_id":"coordinator-1"}
```

#### 9.3 Distributed Tracing (`src/common/tracing.c`)

**Purpose**: OpenTelemetry-compatible tracing

```c
typedef struct {
    char trace_id[64];
    char span_id[64];
    char parent_span_id[64];
    
    char operation[128];
    uint64_t start_time_us;
    uint64_t end_time_us;
    
    // Tags
    char** tag_keys;
    char** tag_values;
    size_t tag_count;
} trace_span_t;

trace_span_t* trace_start_span(const char* operation, const char* parent_span_id);
void trace_add_tag(trace_span_t* span, const char* key, const char* value);
void trace_end_span(trace_span_t* span);

// Export spans to collector (Jaeger, Zipkin)
void trace_export_batch(trace_span_t** spans, size_t count);
```

**Trace Propagation**: Inject trace context in RPC headers

#### 9.4 Health Checks (`src/api/health.c`)

**Endpoints**:

**GET /health/live** - Liveness probe (is process running?)
```json
{"status": "UP"}
```

**GET /health/ready** - Readiness probe (ready to serve traffic?)
```json
{
  "status": "UP",
  "checks": {
    "raft": "UP",
    "gossip": "UP",
    "workers_available": "UP"
  }
}
```

#### 9.5 Admin CLI (`src/admin/cli.c`)

**Operations Tool**:
```bash
$ distric-admin cluster status
Leader: coordinator-1
Coordinators: 3 (2 followers, 1 leader)
Workers: 47 alive, 2 suspected

$ distric-admin rules list
ID                  Version  Priority  Enabled
high-value-payment  1        100       true
fraud-detection     2        90        true

$ distric-admin rules reload
Reloading rules from /etc/distric/rules.yaml...
Successfully reloaded 47 rules (version 123)

$ distric-admin workflows list --status=running
Workflow ID         Started             Tasks (Running/Total)
wf-abc123          2026-01-13 10:25    3/5
wf-def456          2026-01-13 10:28    1/3

$ distric-admin workers drain worker-5
Draining worker-5 (marking unavailable, finishing existing tasks)...
Worker drained successfully
```

### Deliverables
- ✅ Prometheus metrics exported
- ✅ Structured JSON logging
- ✅ Distributed tracing integrated
- ✅ Health check endpoints
- ✅ Admin CLI tool functional

---

## LAYER 10: Production Hardening (Week 15-17)

### 10.1 Performance Optimization

**Profiling**:
- Use `perf`, `gprof`, or `valgrind --tool=callgrind`
- Identify hot paths in rule evaluation and task distribution
- Optimize critical loops and memory allocations

**Specific Optimizations**:
- **Zero-copy message passing**: Use shared memory or memory-mapped regions for large payloads
- **Connection pooling**: Reuse TCP connections aggressively
- **Rule evaluation**: Build decision tree or bloom filters for fast pre-filtering
- **Batch operations**: Batch Raft log appends, batch task assignments

**Target Performance**:
- Message ingestion: 50k msg/sec per coordinator
- Rule evaluation: <5ms p99
- Task assignment: <20ms p99

### 10.2 Security Hardening

**TLS/mTLS**:
- Enable TLS for all inter-node communication
- Mutual TLS for authentication
- Certificate rotation support

**Authentication & Authorization**:
- API key validation for REST API
- Role-based access control (admin vs read-only)

**Input Validation**:
- Sanitize all external inputs (API, config files)
- Prevent injection attacks (SQL, command, JSON)

**Secrets Management**:
- Integrate with HashiCorp Vault or AWS Secrets Manager
- Never log secrets

### 10.3 Failure Recovery Testing

**Chaos Engineering**:
- Use `chaos-mesh` or custom scripts
- Randomly kill coordinator/worker nodes
- Introduce network latency and packet loss
- Partition network (split brain scenarios)

**Recovery Validation**:
- Raft leader election completes within 5s
- No task duplication during failover
- Message loss rate <0.01%

### 10.4 Load Testing

**Tools**: `wrk`, `hey`, or custom load generator

**Test Scenarios**:
1. **Steady state**: 10k msg/sec sustained for 1 hour
2. **Burst load**: Spike to 100k msg/sec for 1 minute
3. **Slow workers**: Introduce artificial task delays, verify backpressure
4. **Node churn**: Add/remove workers during load

**Success Criteria**:
- No crashes or memory leaks
- Latency remains within SLA (p99 <100ms)
- All messages processed successfully

### 10.5 Documentation

**Production Deployment Guide**:
- Hardware requirements (CPU, RAM, disk, network)
- Network topology recommendations
- Firewall rules
- Backup/restore procedures
- Disaster recovery playbook

**Operations Runbook**:
- How to add/remove nodes
- How to update rules without downtime
- How to roll back failed deployments
- Troubleshooting common issues
- Performance tuning guide

### Deliverables
- ✅ All performance targets met
- ✅ Security audit passed
- ✅ Chaos tests demonstrate resilience
- ✅ Load tests validate scalability
- ✅ Complete documentation ready

---

## 4. Technology Stack Recommendations

### Core Language
- **Primary**: C11 or C++17
- **Rationale**: Performance, control over memory/threading, suitable for systems programming

### Build System
- **CMake 3.15+**: Cross-platform, widely adopted
- **Conan or vcpkg**: Dependency management (optional)

### Key Dependencies

#### Essential (Zero External Libraries)
- **Networking**: Native POSIX sockets (epoll/kqueue)
- **Threading**: pthread (POSIX threads)
- **Serialization**: **Custom binary protocol** (no Protobuf, no external libs)
- **Logging**: Custom JSON logger
- **Testing**: Google Test (C++) or Unity (C)

#### Storage
- **LMDB**: Embedded key-value store for Raft log persistence
- **SQLite** (alternative): If SQL interface needed

#### Parsing
- **libyaml**: YAML parsing for rules
- **cJSON**: JSON parsing (minimal, single-file library)

#### Optional
- **OpenSSL/mbedTLS**: TLS support
- **jemalloc**: Better memory allocator for high concurrency
- **mimalloc** (alternative): Microsoft's memory allocator

### Testing Tools
- **Valgrind**: Memory leak detection
- **AddressSanitizer/UBSan**: Runtime error detection
- **cppcheck/clang-tidy**: Static analysis
- **wrk/hey**: HTTP load testing

### Observability
- **Prometheus**: Metrics collection
- **Grafana**: Visualization
- **Jaeger/Zipkin**: Distributed tracing
- **ELK Stack**: Log aggregation (Elasticsearch, Logstash, Kibana)

---

## 5. Project Structure

```
distric/
├── CMakeLists.txt
├── README.md
├── docs/
│   ├── architecture.md
│   ├── api_reference.md
│   ├── deployment_guide.md
│   └── operations_runbook.md
│
├── include/
│   ├── transport/
│   │   ├── tcp.h
│   │   └── udp.h
│   ├── protocol/
│   │   ├── serializer.h
│   │   └── rpc.h
│   ├── cluster/
│   │   ├── raft.h
│   │   ├── gossip.h
│   │   ├── coordinator.h
│   │   └── worker_pool.h
│   ├── rule_engine/
│   │   ├── parser.h
│   │   ├── evaluator.h
│   │   └── catalog.h
│   ├── task_engine/
│   │   ├── workflow.h
│   │   ├── executor.h
│   │   ├── plugin.h
│   │   └── worker_runner.h
│   ├── api/
│   │   ├── rest_server.h
│   │   └── health.h
│   └── common/
│       ├── metrics.h
│       ├── logging.h
│       ├── tracing.h
│       └── utils.h
│
├── src/
│   ├── transport/
│   ├── protocol/
│   ├── cluster/
│   ├── rule_engine/
│   ├── task_engine/
│   ├── api/
│   ├── admin/
│   │   └── cli.c
│   ├── common/
│   └── main.c
│
├── proto/
│   ├── messages.proto
│   ├── raft.proto
│   └── gossip.proto
│
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── chaos/
│   └── performance/
│
├── plugins/
│   ├── payment_validator/
│   ├── user_history_checker/
│   ├── ml_inference/
│   └── webhook/
│
├── config/
│   ├── rules.example.yaml
│   ├── workflows.example.yaml
│   └── cluster.example.yaml
│
├── scripts/
│   ├── build.sh
│   ├── test.sh
│   ├── deploy.sh
│   └── chaos_test.sh
│
└── benchmarks/
    ├── bench_raft.c
    ├── bench_rule_engine.c
    └── bench_task_execution.c
```

---

## 6. Development Workflow

### Phase-by-Phase Approach

Each phase follows this cycle:
1. **Design**: Write detailed design doc for the layer
2. **Interface-First**: Define public APIs (headers) before implementation
3. **TDD**: Write tests first for critical functions
4. **Implement**: Build the layer incrementally
5. **Test**: Unit tests, integration tests, performance tests
6. **Review**: Code review focusing on correctness, performance, maintainability
7. **Document**: Update documentation with examples
8. **Integrate**: Connect with previous layers

### Git Workflow
- **Branching**: Feature branches (`feature/raft-leader-election`)
- **Commits**: Small, atomic commits with descriptive messages
- **PR Reviews**: Required before merging to main
- **CI/CD**: Automated build, test, and static analysis on each PR

### Code Standards
- **Style**: Follow Google C++ Style Guide (or similar)
- **Comments**: Doxygen-style documentation for public APIs
- **Error Handling**: Always check return values, use error codes consistently
- **Memory Safety**: No memory leaks (Valgrind clean), bounds checking
- **Thread Safety**: Document thread-safe vs thread-unsafe functions

---

## 7. Risk Mitigation

### Technical Risks

**Risk 1: Raft Implementation Bugs**
- **Mitigation**: Use formal verification techniques, extensive chaos testing, reference Raft paper closely
- **Fallback**: Consider using proven Raft library (etcd's raft, NuRaft)

**Risk 2: Performance Not Meeting Targets**
- **Mitigation**: Profile early and often, optimize hot paths first
- **Fallback**: Use C++ with more optimization opportunities, consider JIT compilation for rules

**Risk 3: Plugin System Security**
- **Mitigation**: Run plugins in sandboxed processes (seccomp, containers), strict resource limits
- **Fallback**: Restrict plugin API surface, require plugin signing

**Risk 4: Network Partition Handling**
- **Mitigation**: Extensive partition testing with Jepsen, implement proper quorum checks
- **Fallback**: Conservative failover policies (prefer availability over consistency where safe)

### Schedule Risks

**Risk 1: Phase Overrun**
- **Mitigation**: Time-box each phase, cut scope if needed (move features to v2.0)
- **Mitigation**: Weekly progress reviews, identify blockers early

**Risk 2: Integration Issues**
- **Mitigation**: Continuous integration starting from Phase 2
- **Mitigation**: Integration tests at each phase boundary

---

## 8. Success Metrics

### Functional Completeness
- ✅ All requirements from Requirements Document implemented
- ✅ 10+ rules working end-to-end
- ✅ 5+ workflow types (including branching, parallel tasks)
- ✅ 3+ task plugins operational
- ✅ Raft consensus with 5 coordinator nodes
- ✅ Gossip protocol with 100+ worker nodes

### Performance
- ✅ 50k messages/sec ingestion (per coordinator)
- ✅ Rule evaluation <5ms (p99)
- ✅ Task assignment <20ms (p99)
- ✅ Raft commit latency <100ms (p99)
- ✅ Gossip failure detection <5 seconds

### Reliability
- ✅ 99.9% uptime in load tests
- ✅ Leader election completes in <5 seconds
- ✅ Zero message loss during coordinator failover
- ✅ Zero task duplication during worker failures
- ✅ Memory leak-free over 24-hour stress test

### Quality
- ✅ >80% code coverage in tests
- ✅ Zero critical security vulnerabilities (static analysis)
- ✅ All public APIs documented
- ✅ Operations runbook complete and validated

---

## 9. Post-Launch Roadmap (Future Enhancements)

### v2.0 Features
- **Multi-datacenter deployment**: Geographically distributed clusters
- **Advanced scheduling**: Priority queues, deadline scheduling, resource reservation
- **State machine tasks**: Long-running stateful tasks with checkpointing
- **Rule conflict detection**: Analyze rules for conflicts and suggest resolutions
- **Visual workflow designer**: Web UI for creating workflows
- **Autoscaling**: Automatic worker scaling based on load

### v2.1 Features
- **Stream processing**: Support for continuous data streams (Kafka integration)
- **Graph queries**: Query language for workflow dependencies and data lineage
- **ML model serving**: Native support for ML inference tasks
- **Multi-tenancy**: Isolated namespaces for different teams/customers

---

## 10. Conclusion

This implementation strategy provides a structured, bottom-up approach to building DistriC 2.0. By focusing on solid foundations and incremental integration, the project minimizes risk while delivering a production-grade distributed task execution system.

**Key Success Factors**:
1. **Disciplined layering**: Build and test each layer thoroughly before moving up
2. **Test-first mindset**: Write tests before complex implementation
3. **Performance by design**: Profile early, optimize critical paths
4. **Operations excellence**: Build observability from day one
5. **Iterative refinement**: Continuously improve based on testing and feedback

**Timeline Summary**:
- **Week 1**: Observability foundation (metrics, logging, tracing)
- **Weeks 2-3**: Transport layer with observability integration
- **Weeks 3-4**: Custom binary protocol (no Protobuf)
- **Weeks 5-7**: Raft & Gossip consensus
- **Weeks 8-10**: Cluster management integration
- **Weeks 11-13**: Rule engine
- **Weeks 14-16**: Task execution & API
- **Weeks 17-18**: API integration & end-to-end testing
- **Weeks 19-20**: Production hardening & launch

**Total Duration**: ~20 weeks (5 months) to production-ready system

---

## Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-01-13 | System Architect | Initial implementation strategy |
- ✅ TCP server handling 10k+ connections
- ✅ UDP socket with reliable test coverage
- ✅ Connection pool benchmarked (reuse vs new)
- ✅ Memory leak-free (Valgrind verified)

---

## LAYER 2: Protocol Layer (Week 2-3)

### Objectives
Efficient serialization and RPC framework for structured communication.

### Components

#### 2.1 Message Serialization (`src/protocol/serializer.c`)

**Purpose**: Convert C structs to/from wire format efficiently

**Design Decision**: Use **Protocol Buffers** (via nanopb for embedded C)

**Why Protocol Buffers?**
- Schema evolution (backward/forward compatibility)
- Compact binary format
- Wide tooling support
- Better than hand-rolled binary protocols

**Message Definitions** (`proto/messages.proto`):
```protobuf
syntax = "proto3";

// Generic message envelope
message MessageEnvelope {
  string message_id = 1;
  int64 timestamp = 2;
  string source = 3;
  map<string, string> headers = 4;
  bytes payload = 5;
}

// Raft messages
message RaftAppendEntries {
  uint32 term = 1;
  string leader_id = 2;
  uint32 prev_log_index = 3;
  uint32 prev_log_term = 4;
  repeated LogEntry entries = 5;
  uint32 leader_commit = 6;
}

// Gossip messages
message GossipMessage {
  string node_id = 1;
  NodeState state = 2;
  repeated NodeInfo peers = 3;
  uint64 incarnation = 4;
}

// Task messages
message TaskAssignment {
  string task_id = 1;
  string workflow_id = 2;
  string task_type = 3;
  bytes config = 4;
  bytes input_data = 5;
  int32 timeout_sec = 6;
}
```

**API**:
```c
// Generic serialization
int serialize_message(const message_t* msg, uint8_t** output, size_t* len);
int deserialize_message(const uint8_t* input, size_t len, message_t** msg);
void free_message(message_t* msg);

// Type-specific helpers
int serialize_raft_append_entries(const raft_append_entries_t* ae, uint8_t** out, size_t* len);
```

#### 2.2 RPC Framework (`src/protocol/rpc.c`)

**Purpose**: Request-response pattern over TCP with timeout and retry

**Features**:
```c
typedef struct rpc_client rpc_client_t;
typedef struct rpc_server rpc_server_t;

// RPC handler signature
typedef int (*rpc_handler_t)(const uint8_t* request, size_t req_len,
                             uint8_t** response, size_t* resp_len,
                             void* userdata);

// Server side
rpc_server_t* rpc_server_create(tcp_server_t* tcp_server);
int rpc_register_handler(rpc_server_t* server, uint32_t method_id, 
                         rpc_handler_t handler, void* userdata);

// Client side
rpc_client_t* rpc_client_create(tcp_pool_t* pool);
int rpc_call(rpc_client_t* client, const char* host, uint16_t port,
             uint32_t method_id, const uint8_t* request, size_t req_len,
             uint8_t** response, size_t* resp_len, int timeout_ms);
```

**Implementation Details**:
- Request ID generation (UUID or sequential)
- Timeout tracking with epoll/kqueue timers
- Automatic retry with exponential backoff
- Response routing (multiplex multiple requests on one connection)
- Flow control (max pending requests)

**Protocol Format** (custom framing over TCP):
```
[4 bytes: Total Length]
[4 bytes: Request ID]
[4 bytes: Method ID]
[4 bytes: Flags]
[N bytes: Protobuf Payload]
```

#### 2.3 Message Validation (`src/protocol/validator.c`)

**Purpose**: Schema validation before processing

```c
int validate_message(const message_t* msg, const schema_t* schema);
```

### Deliverables
- ✅ Protobuf schemas compiled and integrated
- ✅ RPC client/server with timeout and retry
- ✅ Benchmark: 50k RPC calls/sec on single connection
- ✅ Compatibility tests (old client vs new server)

---

## LAYER 3: Raft Consensus (Week 4-5)

### Objectives
Implement production-grade Raft for coordination nodes.

### Components

#### 3.1 Raft Core (`src/cluster/raft_core.c`)

**Implementation Phases**:

**Phase 3.1.1: Leader Election**
```c
typedef enum {
    RAFT_FOLLOWER,
    RAFT_CANDIDATE,
    RAFT_LEADER
} raft_state_t;

typedef struct {
    char node_id[64];
    raft_state_t state;
    uint32_t current_term;
    char voted_for[64];
    uint32_t commit_index;
    uint32_t last_applied;
    
    // Leader-specific
    uint32_t* next_index;   // for each peer
    uint32_t* match_index;  // for each peer
    
    // Timing
    uint64_t election_timeout_ms;
    uint64_t last_heartbeat;
} raft_node_t;

raft_node_t* raft_create(const char* node_id, const char** peers, size_t peer_count);

// Core Raft operations
int raft_tick(raft_node_t* node);  // Called periodically
int raft_handle_request_vote(raft_node_t* node, const vote_request_t* req, vote_response_t* resp);
int raft_handle_append_entries(raft_node_t* node, const append_entries_t* req, append_entries_response_t* resp);
```

**Election Algorithm**:
1. Start as follower with randomized timeout (150-300ms)
2. On timeout, become candidate, increment term, vote for self
3. Send RequestVote RPCs to all peers
4. If majority votes received, become leader
5. Send heartbeats every 50ms to maintain leadership

**Phase 3.1.2: Log Replication**
```c
typedef struct {
    uint32_t term;
    uint32_t index;
    uint8_t* data;
    size_t data_len;
} log_entry_t;

typedef struct {
    log_entry_t* entries;
    size_t count;
    size_t capacity;
} raft_log_t;

int raft_append_log(raft_node_t* node, const uint8_t* data, size_t len);
int raft_replicate_to_followers(raft_node_t* node);
int raft_apply_committed_entries(raft_node_t* node, 
                                 void (*apply_fn)(const uint8_t* data, size_t len, void* ctx),
                                 void* ctx);
```

**Replication Flow**:
1. Leader appends entry to local log
2. Sends AppendEntries RPCs to all followers in parallel
3. Waits for majority acknowledgment
4. Advances commit_index
5. Applies committed entries to state machine
6. Notifies client of success

**Phase 3.1.3: Safety & Edge Cases**
- **Log consistency check**: Reject entries if prev_log doesn't match
- **Term updates**: Always update to higher terms
- **Split brain prevention**: Old leaders step down on seeing higher term
- **Network partition handling**: Minority partition cannot commit

#### 3.2 Raft Persistence (`src/cluster/raft_persistence.c`)

**Purpose**: Durable storage for Raft state

**Persistent State**:
```c
typedef struct {
    uint32_t current_term;
    char voted_for[64];
    raft_log_t log;
} raft_persistent_state_t;

int raft_save_state(raft_node_t* node, const char* storage_path);
int raft_load_state(raft_node_t* node, const char* storage_path);
```

**Storage Format**:
- Option A: Single file with append-only log + metadata
- Option B: Embedded database (SQLite, LMDB)
- **Chosen**: LMDB for performance and ACID properties

**Snapshotting** (for log compaction):
```c
typedef struct {
    uint32_t last_included_index;
    uint32_t last_included_term;
    uint8_t* snapshot_data;
    size_t snapshot_size;
} raft_snapshot_t;

int raft_create_snapshot(raft_node_t* node, raft_snapshot_t** snapshot);
int raft_install_snapshot(raft_node_t* node, const raft_snapshot_t* snapshot);
```

**Compaction Trigger**: When log exceeds 10,000 entries

#### 3.3 Raft RPC Integration (`src/cluster/raft_rpc_integration.c`)

**Purpose**: Bridge Raft core with RPC layer

```c
void raft_start_server(raft_node_t* node, rpc_server_t* rpc_server);

// RPC method IDs
#define RPC_RAFT_REQUEST_VOTE      0x1001
#define RPC_RAFT_APPEND_ENTRIES    0x1002
#define RPC_RAFT_INSTALL_SNAPSHOT  0x1003
```

**Handlers**:
```c
int handle_request_vote_rpc(const uint8_t* req, size_t req_len,
                            uint8_t** resp, size_t* resp_len,
                            void* userdata) {
    raft_node_t* node = (raft_node_t*)userdata;
    vote_request_t vote_req;
    vote_response_t vote_resp;
    
    deserialize_vote_request(req, req_len, &vote_req);
    raft_handle_request_vote(node, &vote_req, &vote_resp);
    serialize_vote_response(&vote_resp, resp, resp_len);
    
    return 0;
}
```

### Testing Strategy
- **Unit tests**: Leader election with 3, 5, 7 nodes
- **Chaos tests**: Kill leader mid-election, partition network
- **Correctness tests**: Verify linearizability with Jepsen-style tests
- **Performance tests**: Log replication throughput (target: 10k entries/sec)

### Deliverables
- ✅ Leader election working across 5 nodes
- ✅ Log replication with crash recovery
- ✅ Snapshot/restore mechanism
- ✅ Chaos test suite (random failures, partitions)

---

## LAYER 4: Gossip Protocol (Week 5-6)

### Objectives
Rapid failure detection and membership management.

### Components

#### 4.1 Gossip Core (`src/cluster/gossip.c`)

**Purpose**: SWIM-style failure detection

**Node States**:
```c
typedef enum {
    NODE_ALIVE,
    NODE_SUSPECTED,
    NODE_FAILED,
    NODE_LEFT  // graceful departure
} node_status_t;

typedef struct {
    char node_id[64];
    char address[256];
    uint16_t port;
    node_status_t status;
    uint64_t incarnation;  // for conflict resolution
    uint64_t last_seen;
    node_role_t role;  // COORDINATOR or WORKER
} gossip_node_info_t;

typedef struct {
    gossip_node_info_t* nodes;
    size_t node_count;
    size_t capacity;
    
    udp_socket_t* socket;
    pthread_t gossip_thread;
    uint64_t protocol_period_ms;
} gossip_state_t;
```

**Gossip Algorithm** (SWIM Protocol):

**Every gossip period (1 second)**:
1. **Probe Phase**: Select random peer, send PING
2. **If no ACK within timeout (500ms)**:
   - Select K random peers (K=3)
   - Send indirect PING request through them
   - If no indirect ACK, mark as SUSPECTED
3. **Suspected → Failed**: After N periods without ACK (N=3), mark FAILED
4. **Dissemination**: Piggyback membership updates on all messages

**Message Types**:
```c
typedef enum {
    GOSSIP_PING,
    GOSSIP_ACK,
    GOSSIP_INDIRECT_PING,
    GOSSIP_MEMBERSHIP_UPDATE
} gossip_msg_type_t;

typedef struct {
    gossip_msg_type_t type;
    char sender_id[64];
    uint64_t incarnation;
    gossip_node_info_t* updates;  // piggybacked membership changes
    size_t update_count;
} gossip_message_t;
```

**API**:
```c
gossip_state_t* gossip_init(const char* node_id, const char* bind_addr, uint16_t port);
int gossip_add_seed(gossip_state_t* state, const char* addr, uint16_t port);
int gossip_start(gossip_state_t* state);
int gossip_leave(gossip_state_t* state);  // graceful departure

// Query membership
gossip_node_info_t* gossip_get_alive_nodes(gossip_state_t* state, size_t* count);
int gossip_is_node_alive(gossip_state_t* state, const char* node_id);

// Callbacks
void gossip_set_on_node_joined(gossip_state_t* state, 
                               void (*callback)(const gossip_node_info_t* node, void* ctx), 
                               void* ctx);
void gossip_set_on_node_failed(gossip_state_t* state, 
                               void (*callback)(const gossip_node_info_t* node, void* ctx), 
                               void* ctx);
```

#### 4.2 Conflict Resolution

**Incarnation Numbers**: Each node maintains its own incarnation counter. When suspected, node can refute by incrementing incarnation and broadcasting update.

```c
void gossip_refute_suspicion(gossip_state_t* state) {
    state->self_incarnation++;
    // Broadcast ALIVE status with new incarnation
}
```

**Update Ordering**: Higher incarnation always wins. If incarnation is equal, state priority: ALIVE > SUSPECTED > FAILED

#### 4.3 Integration with Raft

**Coordination Nodes**:
- Participate in both Raft and Gossip
- Use Gossip for fast failure detection of workers
- Use Raft for coordination node failures (more critical)

**Worker Nodes**:
- Participate only in Gossip
- Report status to coordinator via Gossip
- Coordinator uses Gossip info to avoid assigning tasks to failed workers

### Testing Strategy
- **Failure detection time**: Measure time from kill to detection
- **False positive rate**: Ensure healthy nodes not marked failed
- **Partition healing**: Verify convergence after partition resolves
- **Scale test**: 1000 nodes gossiping

### Deliverables
- ✅ SWIM implementation detecting failures in <5s
- ✅ False positive rate <1% under load
- ✅ Partition recovery tested
- ✅ Integration with coordinator role

---

## LAYER 5: Cluster Management (Week 7-8)

### Objectives
Unified cluster view combining Raft and Gossip.

### Components

#### 5.1 Cluster Coordinator (`src/cluster/coordinator.c`)

**Purpose**: Orchestrate Raft and Gossip, maintain authoritative cluster view

```c
typedef struct {
    raft_node_t* raft;
    gossip_state_t* gossip;
    
    // Unified node registry
    cluster_node_t* all_nodes;
    size_t node_count;
    
    // Leader-only state
    bool is_leader;
    task_queue_t* pending_tasks;
    worker_pool_t* worker_pool;
} cluster_coordinator_t;

cluster_coordinator_t* cluster_coordinator_create(const cluster_config_t* config);
int cluster_coordinator_start(cluster_coordinator_t* coord);

// Leader election callback from Raft
void on_became_leader(raft_node_t* raft, void* userdata) {
    cluster_coordinator_t* coord = (cluster_coordinator_t*)userdata;
    coord->is_leader = true;
    // Start task distribution
}

void on_became_follower(raft_node_t* raft, void* userdata) {
    cluster_coordinator_t* coord = (cluster_coordinator_t*)userdata;
    coord->is_leader = false;
    // Stop task distribution
}
```

#### 5.2 Worker Pool Management (`src/cluster/worker_pool.c`)

**Purpose**: Track available workers and their capacity

```c
typedef struct {
    char worker_id[64];
    char address[256];
    uint16_t port;
    
    // Capacity
    uint32_t max_concurrent_tasks;
    uint32_t current_task_count;
    
    // Health
    bool is_available;  // from Gossip
    uint64_t last_heartbeat;
    
    // Metrics
    double cpu_usage;
    double memory_usage;
} worker_node_t;

typedef struct {
    worker_node_t* workers;
    size_t worker_count;
    pthread_rwlock_t lock;
} worker_pool_t;

worker_pool_t* worker_pool_create(void);

// Worker registration (called when new worker detected via Gossip)
int worker_pool_add(worker_pool_t* pool, const gossip_node_info_t* node);
int worker_pool_remove(worker_pool_t* pool, const char* worker_id);

// Task assignment
worker_node_t* worker_pool_select(worker_pool_t* pool, 
                                  load_balance_strategy_t strategy);
int worker_pool_mark_busy(worker_pool_t* pool, const char* worker_id, 
                          const char* task_id);
int worker_pool_mark_free(worker_pool_t* pool, const char* worker_id, 
                          const char* task_id);
```

**Load Balancing Strategies**:
```c
typedef enum {
    LB_ROUND_ROBIN,
    LB_LEAST_LOADED,
    LB_RANDOM,
    LB_LOCALITY_AWARE  // for future data affinity
} load_balance_strategy_t;
```

#### 5.3 Configuration Management (`src/cluster/config_manager.c`)

**Purpose**: Replicate cluster configuration via Raft

**Replicated Configuration**:
- Node roles (coordinator vs worker)
- Rule catalog (see Layer 6)
- Workflow definitions (see Layer 7)
- System parameters (timeouts, thresholds)

```c
typedef struct {
    raft_node_t* raft;
    
    // In-memory state machine
    config_state_t* current_config;
    pthread_rwlock_t config_lock;
} config_manager_t;

// Apply log entry to state machine
void apply_config_change(const uint8_t* log_entry, size_t len, void* ctx) {
    config_manager_t* mgr = (config_manager_t*)ctx;
    
    config_change_t change;
    deserialize_config_change(log_entry, len, &change);
    
    pthread_rwlock_wrlock(&mgr->config_lock);
    apply_change_to_state(mgr->current_config, &change);
    pthread_rwlock_unlock(&mgr->config_lock);
}

// Leader-only: Propose configuration change
int config_manager_propose_change(config_manager_t* mgr, 
                                  const config_change_t* change) {
    if (!raft_is_leader(mgr->raft)) {
        return -1;  // Redirect to leader
    }
    
    uint8_t* serialized;
    size_t len;
    serialize_config_change(change, &serialized, &len);
    
    return raft_append_log(mgr->raft, serialized, len);
}
```

### Deliverables
- ✅ Coordinator node running both Raft and Gossip
- ✅ Worker discovery via Gossip
- ✅ Task assignment to available workers
- ✅ Configuration replication via Raft

---

## LAYER 6: Rule Engine (Week 9-11)

### Objectives
Configuration-as-code rule evaluation system.

### Components

#### 6.1 Rule Parser (`src/rule_engine/parser.c`)

**Purpose**: Parse YAML/JSON rule configurations into internal representation

**Dependencies**: `libyaml` or `yajl` for JSON

```c
typedef enum {
    OP_EQUALS,
    OP_NOT_EQUALS,
    OP_GREATER_THAN,
    OP_LESS_THAN,
    OP_CONTAINS,
    OP_REGEX,
    OP_IN_RANGE
} comparison_op_t;

typedef struct {
    char field_path[256];  // e.g., "payload.user.age"
    comparison_op_t op;
    char value[512];  // can be number, string, regex pattern
} rule_condition_t;

typedef enum {
    LOGIC_AND,
    LOGIC_OR
} logic_op_t;

typedef struct {
    rule_condition_t* conditions;
    size_t condition_count;
    logic_op_t combinator;
} rule_condition_group_t;

typedef struct {
    char workflow_id[128];
    char* params_json;  // workflow-specific parameters
    uint32_t timeout_sec;
    uint32_t retry_count;
} rule_action_t;

typedef struct {
    char rule_id[128];
    uint32_t version;
    uint32_t priority;
    bool enabled;
    
    rule_condition_group_t conditions;
    rule_action_t* actions;
    size_t action_count;
} rule_t;

// Parse rules from file
int parse_rules_from_file(const char* filepath, rule_t** rules, size_t* count);
int parse_rules_from_string(const char* yaml_str, rule_t** rules, size_t* count);
void free_rules(rule_t* rules, size_t count);
```

**Example Rule YAML**:
```yaml
rules:
  - id: "high-value-payment"
    version: 1
    priority: 100
    enabled: true
    conditions:
      combinator: AND
      conditions:
        - field: "event_type"
          op: "equals"
          value: "payment_received"
        - field: "amount"
          op: "greater_than"
          value: "10000"
    actions:
      - workflow: "fraud_check"
        params:
          risk_level: "high"
        timeout: 30
        retry: 3
```

#### 6.2 Rule Evaluator (`src/rule_engine/evaluator.c`)

**Purpose**: Evaluate rules against incoming messages

```c
// Extract field from message JSON using JSON path
int extract_field(const char* json, const char* field_path, char* output, size_t out_size);

// Evaluate single condition
bool evaluate_condition(const rule_condition_t* cond, const char* message_json);

// Evaluate entire rule
bool evaluate_rule(const rule_t* rule, const char* message_json);

// Find matching rules (returns all that match, sorted by priority)
int find_matching_rules(const rule_t* rules, size_t rule_count,
                       const char* message_json,
                       rule_t** matched_rules, size_t* matched_count);
```

**Performance Optimization**:
- **Early exit**: Short-circuit evaluation on first failing condition (AND logic)
- **Field caching**: Extract fields once, reuse across conditions
- **Rule indexing**: Group rules by common field to reduce evaluation set

#### 6.3 Rule Catalog (`src/rule_engine/catalog.c`)

**Purpose**: Manage rule lifecycle (CRUD operations)

```c
typedef struct {
    rule_t* rules;
    size_t rule_count;
    size_t capacity;
    
    pthread_rwlock_t lock;
    
    // Version tracking
    uint64_t catalog_version;
} rule_catalog_t;

rule_catalog_t* rule_catalog_create(void);

// Thread-safe read
rule_t* rule_catalog_get(rule_catalog_t* catalog, const char* rule_id);
rule_t* rule_catalog_get_all(rule_catalog_t* catalog, size_t* count);

// Leader-only writes (will be replicated via Raft)
int rule_catalog_add(rule_catalog_t* catalog, const rule_t* rule);
int rule_catalog_update(rule_catalog_t* catalog, const rule_t* rule);
int rule_catalog_delete(rule_catalog_t* catalog, const char* rule_id);

// Hot reload from file
int rule_catalog_reload(rule_catalog_t* catalog, const char* filepath);
```

#### 6.4 Integration with Raft

**Rule changes are replicated via Raft log**:

```c
typedef enum {
    RULE_OP_ADD,
    RULE_OP_UPDATE,
    RULE_OP_DELETE
} rule_operation_t;

typedef struct {
    rule_operation_t op;
    rule_t rule;  // for ADD/UPDATE
    char rule_id[128];  // for DELETE
} rule_change_t;

// Apply rule change from Raft log
void apply_rule_change(const uint8_t* log_entry, size_t len, void* ctx) {
    rule_catalog_t* catalog = (rule_catalog_t*)ctx;
    
    rule_change_t change;
    deserialize_rule_change(log_entry, len, &change);
    
    switch (change.op) {
        case RULE_OP_ADD:
            rule_catalog_add(catalog, &change.rule);
            break;
        case RULE_OP_UPDATE:
            rule_catalog_update(catalog, &change.rule);
            break;
        case RULE_OP_DELETE:
            rule_catalog_delete(catalog, change.rule_id);
            break;
    }
}
```

### Testing Strategy
- **Unit tests**: Condition evaluation for all operators
- **Integration tests**: YAML parsing → evaluation → action triggering
- **Performance tests**: Evaluate 1000 rules against 10k messages/sec
- **Correctness tests**: Complex boolean logic (nested AND/OR)

### Deliverables