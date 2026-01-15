# DistriC 2.0 - LLM Implementation Guide

## Purpose of This Document

This guide helps you (an LLM) implement the DistriC 2.0 system incrementally, layer by layer. Each session will focus on ONE layer at a time. This document provides:

1. **Session structure**: How to approach each implementation session
2. **Validation checkpoints**: How to verify each layer works before moving on
3. **State tracking**: How to maintain context across sessions
4. **Interface contracts**: Clear APIs between layers

---

## How to Use This Guide

### For the Human Developer

**Starting a New Implementation Session:**
```
"I want to implement DistriC 2.0 Phase [N]: [Layer Name]. 
Please refer to:
- Requirements Document (distric_requirements)
- Implementation Strategy (distric_implementation) 
- This Implementation Guide (llm_implementation_guide)
- Current State Tracker (distric_state_tracker)

Generate the code for this layer according to the specifications."
```

**During Implementation:**
- Request ONE layer at a time
- Ask for specific files (e.g., "Generate src/common/metrics.c")
- Request tests immediately after implementation
- Update state tracker after each layer completes

**After Each Layer:**
```
"Update the state tracker document to reflect that Phase [N] is complete.
Mark all deliverables as done and note any deviations from the plan."
```

### For the LLM

When asked to implement a layer:

1. **Read the context**: Review requirements, implementation strategy, and current state
2. **Confirm scope**: State which layer you're implementing and its boundaries
3. **List deliverables**: Enumerate all files you'll create
4. **Generate code**: Produce complete, compilable code with:
   - Full implementation (no placeholders like "// TODO: implement")
   - Comprehensive error handling
   - Logging and metrics integration (for Phase 1+)
   - Memory safety (no leaks, proper cleanup)
   - Thread safety where required
5. **Generate tests**: Provide unit tests for the layer
6. **Provide integration notes**: Explain how to connect this layer with others
7. **Suggest next steps**: Recommend what to implement next

---

## Implementation Phases & Sessions

### Phase 0: Observability Foundation (Week 1)

**Session 0.1: Metrics System**
```
Files to create:
- include/common/metrics.h
- src/common/metrics.c
- tests/unit/test_metrics.c
```

**Expected deliverables:**
- Atomic counter/gauge/histogram implementation
- Prometheus export format
- Zero-lock metric updates
- Test: 100 threads incrementing counters concurrently

**Validation before proceeding:**
- [ ] Code compiles with `-Wall -Wextra -Werror`
- [ ] All tests pass
- [ ] Valgrind reports no leaks
- [ ] Metrics export produces valid Prometheus format

---

**Session 0.2: Structured Logging**
```
Files to create:
- include/common/logging.h
- src/common/logging.c
- tests/unit/test_logging.c
```

**Expected deliverables:**
- JSON log formatter
- Sync and async logging modes
- Thread-local buffers (no malloc in hot path)
- Ring buffer for async mode
- Test: Parse 10,000 log lines, validate JSON structure

**Validation before proceeding:**
- [ ] Logs are valid JSON
- [ ] No memory leaks in async mode
- [ ] Performance: >100k logs/sec in async mode

---

**Session 0.3: Distributed Tracing**
```
Files to create:
- include/common/tracing.h
- src/common/tracing.c
- tests/unit/test_tracing.c
```

**Expected deliverables:**
- Trace ID / Span ID generation
- Parent-child span relationships
- Span context extraction/injection
- Batch exporter
- Test: Create nested spans, verify parent-child relationships

**Validation before proceeding:**
- [ ] Trace context propagates correctly
- [ ] Span IDs are unique
- [ ] Export produces valid format

---

**Session 0.4: Health Check & HTTP Server**
```
Files to create:
- include/common/health.h
- src/common/health.c
- include/common/http_server.h
- src/common/http_server.c
- tests/unit/test_health.c
- tests/integration/test_http_server.c
```

**Expected deliverables:**
- Health registry with component status tracking
- Minimal HTTP server (GET only, no external deps)
- `/metrics` endpoint (Prometheus format)
- `/health/live` endpoint
- `/health/ready` endpoint
- Test: HTTP GET requests return correct responses

**Validation before proceeding:**
- [ ] `curl localhost:8080/metrics` returns Prometheus data
- [ ] `curl localhost:8080/health/live` returns {"status":"UP"}
- [ ] Health checks reflect component states

---

**Session 0.5: Phase 0 Integration Test**
```
Files to create:
- tests/integration/test_observability_stack.c
```

**Expected deliverables:**
- Integration test combining all Phase 0 components
- Test: Start HTTP server, update metrics, log messages, check health, fetch /metrics

**Validation before proceeding:**
- [ ] All components work together
- [ ] No resource leaks over 1-hour stress test
- [ ] Phase 0 complete - ready for Phase 1

---

### Phase 1: Transport Layer (Weeks 2-3)

**Session 1.1: TCP Server Foundation**
```
Files to create:
- include/transport/tcp.h
- src/transport/tcp.c
- tests/unit/test_tcp.c
```

**Dependencies:**
- Phase 0 (metrics, logging) must be complete

**Expected deliverables:**
- TCP server with epoll/kqueue event loop
- Non-blocking I/O
- Connection accept/close
- Integrated metrics (connections_active, bytes_sent/received)
- Integrated logging (connection events)
- Test: 1000 concurrent connections

**Interface contract:**
```c
tcp_server_t* tcp_server_create(const char* bind_addr, uint16_t port,
                                metrics_registry_t* metrics,
                                logger_t* logger);
int tcp_server_start(tcp_server_t* server, 
                     void (*on_connection)(tcp_connection_t*, void*),
                     void* userdata);
int tcp_send(tcp_connection_t* conn, const void* data, size_t len);
int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms);
void tcp_close(tcp_connection_t* conn);
void tcp_server_destroy(tcp_server_t* server);
```

**Validation before proceeding:**
- [ ] Handles 10k+ concurrent connections
- [ ] All metrics appear in /metrics endpoint
- [ ] Connection events appear in logs
- [ ] No file descriptor leaks

---

**Session 1.2: TCP Connection Pool**
```
Files to create:
- include/transport/tcp_pool.h
- src/transport/tcp_pool.c
- tests/unit/test_tcp_pool.c
```

**Expected deliverables:**
- Connection pooling with max size
- Connection reuse tracking
- Metrics: pool size, acquire duration, reuse count
- Test: Verify connection reuse, test pool exhaustion

**Interface contract:**
```c
tcp_pool_t* tcp_pool_create(size_t max_connections,
                            metrics_registry_t* metrics,
                            logger_t* logger);
tcp_connection_t* tcp_pool_acquire(tcp_pool_t* pool, 
                                   const char* host, uint16_t port);
void tcp_pool_release(tcp_pool_t* pool, tcp_connection_t* conn);
void tcp_pool_destroy(tcp_pool_t* pool);
```

**Validation before proceeding:**
- [ ] Connection reuse works (verify via metrics)
- [ ] Pool respects max_connections limit
- [ ] No connection leaks

---

**Session 1.3: UDP Socket**
```
Files to create:
- include/transport/udp.h
- src/transport/udp.c
- tests/unit/test_udp.c
```

**Expected deliverables:**
- UDP socket creation
- Send/receive with timeout
- Metrics integration
- Test: Send 10k packets, verify all received

**Interface contract:**
```c
udp_socket_t* udp_socket_create(const char* bind_addr, uint16_t port,
                                metrics_registry_t* metrics,
                                logger_t* logger);
int udp_send(udp_socket_t* sock, const void* data, size_t len,
             const char* dest_addr, uint16_t dest_port);
int udp_recv(udp_socket_t* sock, void* buffer, size_t len,
             char* src_addr, uint16_t* src_port, int timeout_ms);
void udp_close(udp_socket_t* sock);
```

**Validation before proceeding:**
- [ ] Packet send/receive works
- [ ] Handles packet loss gracefully (test with `tc netem`)
- [ ] Metrics tracked correctly

---

**Session 1.4: Phase 1 Integration**
```
Files to create:
- tests/integration/test_transport_layer.c
```

**Expected deliverables:**
- Test combining TCP server, pool, and UDP
- Stress test: High connection churn + UDP traffic
- Performance baseline: Document throughput/latency

**Validation before proceeding:**
- [ ] TCP + UDP work simultaneously
- [ ] All transport metrics visible in /metrics
- [ ] Phase 1 complete - ready for Phase 2

---

### Phase 2: Protocol Layer (Weeks 3-4)

**Session 2.1: Binary Protocol Foundation**
```
Files to create:
- include/protocol/binary.h
- src/protocol/binary.c
- tests/unit/test_binary_protocol.c
```

**Expected deliverables:**
- Message header structure (32 bytes)
- Header serialization/deserialization
- CRC32 computation
- Magic number validation
- Test: Serialize/deserialize headers, detect corruption

**Interface contract:**
```c
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t msg_type;
    uint16_t flags;
    uint32_t payload_len;
    uint64_t message_id;
    uint64_t timestamp;
    uint32_t crc32;
} message_header_t;

int serialize_header(const message_header_t* hdr, uint8_t* buffer);
int deserialize_header(const uint8_t* buffer, message_header_t* hdr);
uint32_t compute_crc32(const void* data, size_t len);
int validate_message(const message_header_t* hdr, const uint8_t* payload);
```

**Validation before proceeding:**
- [ ] Header serialization is portable (test on different endianness)
- [ ] CRC32 detects single-bit errors
- [ ] Version validation works

---

**Session 2.2: TLV Encoder/Decoder**
```
Files to create:
- include/protocol/tlv.h
- src/protocol/tlv.c
- tests/unit/test_tlv.c
```

**Expected deliverables:**
- TLV encoder (write uint8/16/32/64, string, bytes)
- TLV decoder (read fields by tag)
- Dynamic buffer growth
- Test: Encode complex nested structure, decode successfully

**Interface contract:**
```c
tlv_encoder_t* tlv_encoder_create(size_t initial_capacity);
void tlv_encode_uint32(tlv_encoder_t* enc, uint16_t tag, uint32_t value);
void tlv_encode_string(tlv_encoder_t* enc, uint16_t tag, const char* str);
uint8_t* tlv_encoder_finalize(tlv_encoder_t* enc, size_t* len_out);

tlv_decoder_t* tlv_decoder_create(const uint8_t* buffer, size_t length);
int tlv_decode_next(tlv_decoder_t* dec, tlv_field_t* field_out);
int tlv_find_field(tlv_decoder_t* dec, uint16_t tag, tlv_field_t* field_out);
uint32_t tlv_field_as_uint32(const tlv_field_t* field);
```

**Validation before proceeding:**
- [ ] Encode/decode round-trip preserves data
- [ ] Decoder skips unknown field types (forward compatibility)
- [ ] No memory leaks

---

**Session 2.3: Message Definitions**
```
Files to create:
- include/protocol/messages.h
- src/protocol/messages.c
- tests/unit/test_messages.c
```

**Expected deliverables:**
- Define structures for:
  - Raft messages (RequestVote, AppendEntries, InstallSnapshot)
  - Gossip messages (Ping, Ack, MembershipUpdate)
  - Task messages (Assignment, Result, Status)
- Serialize/deserialize functions for each
- Test: Round-trip all message types

**Interface contract:**
```c
// Example for Raft RequestVote
typedef struct {
    uint32_t term;
    char candidate_id[64];
    uint32_t last_log_index;
    uint32_t last_log_term;
} raft_request_vote_t;

int serialize_raft_request_vote(const raft_request_vote_t* rv,
                                uint8_t** buffer_out, size_t* len_out);
int deserialize_raft_request_vote(const uint8_t* buffer, size_t len,
                                  raft_request_vote_t* rv_out);
void free_raft_request_vote(raft_request_vote_t* rv);
```

**Validation before proceeding:**
- [ ] All message types serialize/deserialize correctly
- [ ] Backward compatibility: Old decoder can read messages with new fields
- [ ] Performance: <1μs per message

---

**Session 2.4: RPC Framework**
```
Files to create:
- include/protocol/rpc.h
- src/protocol/rpc.c
- tests/unit/test_rpc.c
```

**Dependencies:**
- TCP transport (Phase 1)
- Binary protocol (Session 2.1-2.3)

**Expected deliverables:**
- RPC server (register handlers by message type)
- RPC client (call with timeout, retry)
- Request ID tracking
- Metrics integration
- Distributed tracing integration
- Test: Client calls server, gets response

**Interface contract:**
```c
typedef int (*rpc_handler_t)(const uint8_t* request, size_t req_len,
                             uint8_t** response, size_t* resp_len,
                             void* userdata, trace_span_t* span);

rpc_server_t* rpc_server_create(tcp_server_t* tcp,
                                metrics_registry_t* metrics,
                                logger_t* logger);
int rpc_register_handler(rpc_server_t* server, message_type_t msg_type,
                        rpc_handler_t handler, void* userdata);

rpc_client_t* rpc_client_create(tcp_pool_t* pool,
                               metrics_registry_t* metrics,
                               logger_t* logger);
int rpc_call(rpc_client_t* client, const char* host, uint16_t port,
             message_type_t msg_type, const uint8_t* request, size_t req_len,
             uint8_t** response, size_t* resp_len, int timeout_ms);
```

**Validation before proceeding:**
- [ ] RPC call completes successfully
- [ ] Timeout works (test with slow handler)
- [ ] Retry works (test with failing handler)
- [ ] All RPC calls appear in metrics
- [ ] Trace context propagates through RPC

---

**Session 2.5: Phase 2 Integration**
```
Files to create:
- tests/integration/test_protocol_layer.c
```

**Expected deliverables:**
- End-to-end test: Client sends all message types, server responds
- Performance test: 50k RPCs/sec
- Trace verification: Confirm spans created for each RPC

**Validation before proceeding:**
- [ ] All message types work over RPC
- [ ] Performance meets target
- [ ] Phase 2 complete - ready for Phase 3

---

## Subsequent Phases

Continue this pattern for:
- **Phase 3**: Raft Consensus (5-7 sessions)
- **Phase 4**: Gossip Protocol (3-4 sessions)
- **Phase 5**: Cluster Management (4-5 sessions)
- **Phase 6**: Rule Engine (5-6 sessions)
- **Phase 7**: Task Execution (6-7 sessions)
- **Phase 8**: API Layer (3-4 sessions)
- **Phase 9**: Production Hardening (4-5 sessions)

Each phase follows the same structure:
1. Break into logical sessions (1-2 hours of implementation each)
2. Define clear interface contracts
3. List concrete deliverables
4. Provide validation checkpoints
5. Generate integration tests before moving on

---

## State Tracking Template

**Use this template in a separate "State Tracker" document:**

```markdown
# DistriC 2.0 - Implementation State Tracker

Last Updated: [Date]

## Phase 0: Observability Foundation
Status: [NOT_STARTED | IN_PROGRESS | COMPLETE]

- [x] Session 0.1: Metrics System
  - Files: metrics.h, metrics.c, test_metrics.c
  - Notes: All tests passing, Prometheus export validated
  
- [x] Session 0.2: Structured Logging
  - Files: logging.h, logging.c, test_logging.c
  - Notes: Async mode achieves 150k logs/sec
  
- [ ] Session 0.3: Distributed Tracing
  - Status: IN_PROGRESS
  - Blockers: None
  
- [ ] Session 0.4: Health & HTTP Server
- [ ] Session 0.5: Phase 0 Integration

## Phase 1: Transport Layer
Status: NOT_STARTED

- [ ] Session 1.1: TCP Server
- [ ] Session 1.2: TCP Connection Pool
- [ ] Session 1.3: UDP Socket
- [ ] Session 1.4: Phase 1 Integration

## Current Session
**Phase 0, Session 0.3**: Implementing distributed tracing

## Next Steps
1. Complete trace context extraction/injection
2. Write span export batch logic
3. Run integration test with nested spans

## Deviations from Plan
- None so far

## Performance Metrics Baseline
- Metrics update: <50ns per counter increment
- Logging: 150k logs/sec (async mode)
- (To be filled as we go)

## Open Questions
- Should we use Jaeger or Zipkin trace format? → Decision: Jaeger
```

---

## Critical Guidelines for LLM

### DO's:
✅ **Generate complete, compilable code** (no "TODO" placeholders)
✅ **Include all error handling** (check malloc, I/O, etc.)
✅ **Add comprehensive comments** for complex logic
✅ **Integrate observability** from Phase 1 onwards (metrics, logs, traces)
✅ **Write tests immediately** after implementation
✅ **Use C11 standards** (stdatomic, _Thread_local)
✅ **Follow interface contracts** exactly as specified
✅ **Check for memory leaks** (assume Valgrind will be run)
✅ **Consider thread safety** (document which functions are thread-safe)

### DON'Ts:
❌ **Never use external serialization libraries** (no Protobuf, no MsgPack)
❌ **Never leave placeholder comments** ("TODO: implement later")
❌ **Never skip error handling** ("assume this succeeds")
❌ **Never mix concerns** (keep layers independent)
❌ **Never use global state** without explicit documentation
❌ **Never use unsafe functions** (strcpy → strncpy, sprintf → snprintf)

### Code Quality Standards:
- **Compilation**: Must compile with `-Wall -Wextra -Wpedantic -Werror`
- **Memory**: Valgrind clean (no leaks, no invalid access)
- **Thread Safety**: Document thread-safety guarantees in header comments
- **Performance**: Profile hot paths, avoid allocations in tight loops
- **Portability**: Test on Linux (epoll) and macOS (kqueue)

---

## Session Prompt Template

**For the human to copy/paste at the start of each session:**

```
I am implementing DistriC 2.0.

Current Phase: Phase [N]
Current Session: Session [N.M]
Layer: [Layer Name]

Please refer to:
1. Requirements Document (distric_requirements)
2. Implementation Strategy (distric_implementation)
3. LLM Implementation Guide (llm_implementation_guide)
4. State Tracker (distric_state_tracker)

Generate the following files for this session:
- [List specific files]

Requirements:
- Complete implementation (no TODOs)
- Full error handling
- Integrated metrics/logging (if Phase 1+)
- Unit tests
- Interface contract adherence
- Memory safety (Valgrind clean)

After generation, provide:
1. Compilation instructions
2. Test run instructions
3. Validation checklist
4. Integration notes for next session
```

---

## Example Session Flow

**Human:**
```
I am implementing DistriC 2.0.

Current Phase: Phase 0
Current Session: Session 0.1
Layer: Metrics System

Please generate:
- include/common/metrics.h
- src/common/metrics.c
- tests/unit/test_metrics.c

Requirements as per the LLM Implementation Guide.
```

**LLM Response:**
```
I'll implement the metrics system with lock-free atomic operations.

=== File: include/common/metrics.h ===
[Full header file with detailed comments]

=== File: src/common/metrics.c ===
[Complete implementation, no placeholders]

=== File: tests/unit/test_metrics.c ===
[Comprehensive unit tests]

=== Compilation Instructions ===
gcc -std=c11 -pthread -I./include \
    src/common/metrics.c tests/unit/test_metrics.c \
    -o test_metrics

=== Test Run ===
./test_metrics
All tests passed (5/5)

=== Validation Checklist ===
- [x] Code compiles with -Wall -Wextra -Werror
- [x] All tests pass
- [x] Atomic operations used (no locks for counter updates)
- [ ] Run Valgrind to confirm no leaks (you should run this)

=== Integration Notes ===
The metrics_registry_t is now ready to be passed to transport layer
components in Phase 1. The Prometheus export function is available
for the HTTP server in Session 0.4.

=== Next Session ===
Proceed to Session 0.2: Structured Logging
The logger will need to integrate with this metrics system to track
log rates.
```

**Human:**
```
Update the state tracker to mark Session 0.1 complete.
```

**LLM:**
```
[Updates distric_state_tracker document]

Session 0.1 marked complete.
Ready to proceed with Session 0.2: Structured Logging.
```

---

## Validation Checklist (Per Session)

Before marking a session complete:

1. **Compilation**
   - [ ] Compiles with `-Wall -Wextra -Wpedantic -Werror`
   - [ ] No warnings

2. **Testing**
   - [ ] All unit tests pass
   - [ ] Integration tests pass (if applicable)
   - [ ] Edge cases tested (NULL pointers, buffer overflows, etc.)

3. **Memory Safety**
   - [ ] Valgrind reports no leaks
   - [ ] No invalid memory access
   - [ ] All malloc's have corresponding free's

4. **Performance** (if applicable)
   - [ ] Meets performance targets specified in requirements
   - [ ] No obvious performance bottlenecks

5. **Documentation**
   - [ ] Public API documented in header
   - [ ] Complex algorithms have inline comments
   - [ ] Thread-safety guarantees stated

6. **Integration**
   - [ ] Interface contract matches specification
   - [ ] Dependencies clearly stated
   - [ ] Works with components from previous sessions

---

## Recovery from Errors

If a session produces non-working code:

**Prompt:**
```
The code from Session [N.M] has the following issues:
- [List specific issues]
- [Compilation errors]
- [Test failures]

Please fix these issues while maintaining the interface contract.
```

The LLM should produce corrected versions of only the affected files.

---

## Conclusion

This guide ensures:
✅ Incremental, validated progress
✅ Clear session boundaries
✅ No context loss between sessions
✅ Consistent code quality
✅ Interface stability across layers

Follow this guide strictly, and you'll have a production-ready DistriC 2.0 system in ~20 weeks of focused implementation.