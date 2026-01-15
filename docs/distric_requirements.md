# DistriC 2.0 - System Requirements Document

## 1. Executive Summary

DistriC 2.0 is a distributed task execution system that processes incoming messages through a rule-based engine, triggering configured workflows across a hybrid cluster architecture. The system combines Raft consensus for coordination nodes with Gossip protocol for rapid failure detection across worker nodes.

## 2. System Architecture Overview

### 2.1 Node Types

**Coordination Nodes (3-5 nodes)**
- Maintain authoritative rule catalog via Raft consensus
- Handle leader election and cluster coordination
- Process rule evaluation for incoming messages
- Manage task distribution to worker nodes
- Small, stable cluster for consistency

**Worker Nodes (N nodes)**
- Execute assigned tasks from coordination layer
- Participate in Gossip protocol for health monitoring
- Report task completion and status updates
- Scale horizontally based on workload
- No direct rule catalog access

### 2.2 Communication Patterns

- **Coordination ↔ Coordination**: Raft consensus (TCP)
- **All Nodes**: Gossip protocol for failure detection (UDP)
- **Client → Coordination**: Message ingestion (TCP)
- **Coordination → Worker**: Task assignment (TCP)
- **Worker → Coordination**: Status updates (TCP/UDP)

---

## 3. Functional Requirements

### 3.1 Message Ingestion Layer

**REQ-MSG-001**: System SHALL accept messages via multiple transport protocols (TCP, HTTP/REST, gRPC)

**REQ-MSG-002**: System SHALL support multiple message formats (JSON, Protocol Buffers, MessagePack)

**REQ-MSG-003**: Message ingestion SHALL have configurable rate limiting per client

**REQ-MSG-004**: System SHALL validate message schema before processing

**REQ-MSG-005**: System SHALL provide synchronous acknowledgment of message receipt

**REQ-MSG-006**: System SHALL support message batching with configurable batch sizes

### 3.2 Rule Engine & Configuration-as-Code

**REQ-RULE-001**: Rules SHALL be defined in YAML/JSON configuration files

**REQ-RULE-002**: Rule syntax SHALL support:
- Pattern matching on message fields (exact, regex, range)
- Logical operators (AND, OR, NOT)
- Field transformations and enrichment
- Priority/ordering of rule evaluation

**REQ-RULE-003**: Each rule SHALL define:
- Matching conditions
- Target workflow/task to trigger
- Execution parameters
- Timeout configuration
- Retry policy

**REQ-RULE-004**: System SHALL hot-reload rule configurations without restart

**REQ-RULE-005**: Rule catalog SHALL be versioned with rollback capability

**REQ-RULE-006**: System SHALL evaluate rules in priority order with fail-fast option

**REQ-RULE-007**: System SHALL support rule composition (rule chains/DAGs)

**Example Rule Structure:**
```yaml
rules:
  - id: "rule-001"
    priority: 100
    conditions:
      - field: "event_type"
        operator: "equals"
        value: "payment_received"
      - field: "amount"
        operator: "greater_than"
        value: 1000
    actions:
      - workflow: "fraud_detection"
        params:
          threshold: "high"
        timeout: 30s
        retry: 3
```

### 3.3 Workflow & Task Execution

**REQ-WORK-001**: Workflows SHALL be defined as directed acyclic graphs (DAGs) of tasks

**REQ-WORK-002**: Tasks SHALL be implemented as plugins/modules loaded dynamically

**REQ-WORK-003**: System SHALL support task types:
- Computation tasks (data transformation, aggregation)
- I/O tasks (database, external API calls)
- Notification tasks (webhooks, messages)
- Control flow tasks (conditional branching, loops)

**REQ-WORK-004**: Task execution SHALL be idempotent and retryable

**REQ-WORK-005**: System SHALL provide task isolation (resource limits, sandboxing)

**REQ-WORK-006**: Failed tasks SHALL support configurable retry with exponential backoff

**REQ-WORK-007**: Workflows SHALL support compensation/rollback logic

**REQ-WORK-008**: System SHALL enforce task execution timeouts

### 3.4 Cluster Coordination (Raft)

**REQ-RAFT-001**: Coordination nodes SHALL use Raft for leader election

**REQ-RAFT-002**: Rule catalog SHALL be replicated via Raft log

**REQ-RAFT-003**: System SHALL tolerate (N-1)/2 coordination node failures

**REQ-RAFT-004**: Leader SHALL be sole entry point for rule catalog modifications

**REQ-RAFT-005**: Log entries SHALL include:
- Rule catalog updates
- Workflow definitions
- Cluster configuration changes

**REQ-RAFT-006**: System SHALL support dynamic membership changes (add/remove nodes)

**REQ-RAFT-007**: Raft implementation SHALL use persistent storage for log and state

**REQ-RAFT-008**: Snapshot mechanism SHALL compact logs beyond threshold

### 3.5 Failure Detection (Gossip)

**REQ-GOSSIP-001**: All nodes SHALL participate in Gossip protocol

**REQ-GOSSIP-002**: Node health SHALL propagate within 3 gossip rounds (configurable)

**REQ-GOSSIP-003**: Gossip interval SHALL be configurable (default: 1 second)

**REQ-GOSSIP-004**: System SHALL mark nodes as suspected after missing N consecutive gossips

**REQ-GOSSIP-005**: Suspected nodes SHALL be confirmed failed after timeout period

**REQ-GOSSIP-006**: Gossip messages SHALL include:
- Node ID and address
- Node role (coordination/worker)
- Current load metrics
- Protocol version

**REQ-GOSSIP-007**: System SHALL detect network partitions and handle gracefully

**REQ-GOSSIP-008**: Gossip SHALL support encryption for secure environments

### 3.6 Task Distribution & Load Balancing

**REQ-DIST-001**: Leader SHALL distribute tasks to available workers

**REQ-DIST-002**: Distribution algorithm SHALL support:
- Round-robin
- Least-loaded
- Locality-aware (data affinity)
- Custom strategies

**REQ-DIST-003**: System SHALL track worker capacity and current load

**REQ-DIST-004**: Tasks SHALL be reassigned if worker fails during execution

**REQ-DIST-005**: System SHALL prevent duplicate task execution

**REQ-DIST-006**: Worker nodes SHALL reject tasks exceeding their capacity

**REQ-DIST-007**: System SHALL support task priority queuing

### 3.7 State Management & Persistence

**REQ-STATE-001**: System SHALL persist:
- Rule catalog
- Workflow definitions
- In-flight task state
- Raft log and snapshots
- Cluster membership

**REQ-STATE-002**: State SHALL survive coordinator node crashes

**REQ-STATE-003**: System SHALL support pluggable storage backends (file, embedded DB, external DB)

**REQ-STATE-004**: Task state SHALL include:
- Task ID and workflow ID
- Current status (pending, running, completed, failed)
- Execution history
- Retry count

**REQ-STATE-005**: Completed task history SHALL be retained per retention policy

**REQ-STATE-006**: System SHALL support state snapshots for backup/restore

### 3.8 Monitoring & Observability

**REQ-MON-001**: System SHALL expose metrics:
- Message ingestion rate
- Rule evaluation latency
- Task execution duration
- Worker utilization
- Raft leader changes
- Gossip convergence time

**REQ-MON-002**: System SHALL provide structured logging (JSON format)

**REQ-MON-003**: System SHALL support distributed tracing (OpenTelemetry compatible)

**REQ-MON-004**: Health check endpoints SHALL report node status

**REQ-MON-005**: System SHALL emit events for:
- Rule catalog changes
- Node joins/leaves
- Task failures
- Leader elections

**REQ-MON-006**: Metrics SHALL be exportable (Prometheus format)

---

## 4. Non-Functional Requirements

### 4.1 Performance

**REQ-PERF-001**: System SHALL process ≥10,000 messages/second per coordinator

**REQ-PERF-002**: Rule evaluation latency SHALL be <10ms (p99)

**REQ-PERF-003**: Task assignment latency SHALL be <50ms

**REQ-PERF-004**: Raft commit latency SHALL be <100ms (p99)

**REQ-PERF-005**: Gossip failure detection SHALL occur within 5 seconds

### 4.2 Scalability

**REQ-SCALE-001**: System SHALL support 1000+ worker nodes

**REQ-SCALE-002**: System SHALL support 10,000+ concurrent tasks

**REQ-SCALE-003**: Rule catalog SHALL support 10,000+ rules

**REQ-SCALE-004**: Workers SHALL be addable/removable without downtime

### 4.3 Reliability

**REQ-REL-001**: System SHALL achieve 99.9% uptime

**REQ-REL-002**: No single point of failure in coordination layer

**REQ-REL-003**: Message loss probability SHALL be <0.01%

**REQ-REL-004**: System SHALL handle graceful degradation under load

**REQ-REL-005**: Failed tasks SHALL be automatically retried per policy

### 4.4 Security

**REQ-SEC-001**: All inter-node communication SHALL support TLS/mTLS

**REQ-SEC-002**: Client authentication SHALL be required (API keys, mTLS)

**REQ-SEC-003**: Rule catalog modifications SHALL be audit-logged

**REQ-SEC-004**: Tasks SHALL run with least-privilege principles

**REQ-SEC-005**: System SHALL support role-based access control (RBAC)

### 4.5 Maintainability

**REQ-MAINT-001**: Codebase SHALL follow modular architecture

**REQ-MAINT-002**: All modules SHALL have unit test coverage >80%

**REQ-MAINT-003**: Public APIs SHALL be documented (Doxygen/similar)

**REQ-MAINT-004**: Configuration changes SHALL not require recompilation

**REQ-MAINT-005**: System SHALL provide admin CLI for operations

---

## 5. Interface Requirements

### 5.1 Client API

**REQ-API-001**: REST API for message submission
```
POST /api/v1/messages
Content-Type: application/json

{
  "event_type": "payment_received",
  "amount": 1500,
  "user_id": "12345"
}
```

**REQ-API-002**: WebSocket support for streaming responses

**REQ-API-003**: gRPC interface for high-throughput clients

### 5.2 Admin API

**REQ-API-101**: REST API for rule management
- GET /api/v1/rules
- POST /api/v1/rules
- PUT /api/v1/rules/{id}
- DELETE /api/v1/rules/{id}

**REQ-API-102**: Cluster status endpoint
- GET /api/v1/cluster/status
- GET /api/v1/cluster/nodes

**REQ-API-103**: Task monitoring
- GET /api/v1/tasks?status={status}
- GET /api/v1/tasks/{id}

### 5.3 Task Plugin Interface

**REQ-API-201**: Tasks SHALL implement standard interface:
```c
typedef struct {
    const char* (*get_name)(void);
    int (*execute)(task_context_t* ctx, 
                   const void* input, 
                   void** output);
    void (*cleanup)(void* output);
    int (*validate)(const void* config);
} task_plugin_t;
```

---

## 6. Data Model

### 6.1 Message
- message_id (UUID)
- timestamp (ISO8601)
- source (string)
- payload (JSON/bytes)
- headers (key-value map)

### 6.2 Rule
- rule_id (string)
- version (integer)
- priority (integer)
- conditions (array of condition objects)
- actions (array of action objects)
- enabled (boolean)
- created_at / updated_at

### 6.3 Workflow
- workflow_id (string)
- version (integer)
- tasks (DAG structure)
- configuration (JSON)

### 6.4 Task Instance
- task_instance_id (UUID)
- workflow_instance_id (UUID)
- task_definition_id (string)
- status (enum)
- assigned_worker (node_id)
- started_at / completed_at
- retry_count
- result (JSON/bytes)

### 6.5 Node
- node_id (UUID)
- node_type (coordination/worker)
- address (IP:port)
- status (active/suspected/failed)
- last_seen (timestamp)
- capacity (resource limits)
- current_load (metrics)

---

## 7. Deployment Requirements

**REQ-DEPLOY-001**: System SHALL support containerized deployment (Docker)

**REQ-DEPLOY-002**: System SHALL provide Kubernetes manifests/Helm charts

**REQ-DEPLOY-003**: Configuration SHALL be externalized (environment variables, config files)

**REQ-DEPLOY-004**: System SHALL support multi-datacenter deployment

**REQ-DEPLOY-005**: Rolling updates SHALL be supported without downtime

---

## 8. Development Requirements

**REQ-DEV-001**: Language: C (C11 standard minimum) or C++ (C++17 minimum)

**REQ-DEV-002**: Build system: CMake 3.15+

**REQ-DEV-003**: Testing framework: Unity or Google Test

**REQ-DEV-004**: Dependencies SHALL be minimal and vendorable

**REQ-DEV-005**: Code SHALL pass static analysis (clang-tidy, cppcheck)

---

## 9. Documentation Requirements

**REQ-DOC-001**: Architecture decision records (ADRs) for major decisions

**REQ-DOC-002**: API documentation (OpenAPI/Swagger for REST)

**REQ-DOC-003**: Deployment guide with examples

**REQ-DOC-004**: Rule configuration guide with examples

**REQ-DOC-005**: Task plugin development guide

---

## 10. Success Criteria

1. Process 50,000 msg/sec across 5 coordinator nodes
2. Detect worker failure within 5 seconds
3. Rule catalog update propagates in <1 second
4. Handle 1000 concurrent workflows
5. Zero message loss during coordinator failover
6. Hot-reload rules without service interruption

---

## Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-01-13 | System Architect | Initial requirements |